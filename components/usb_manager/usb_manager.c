#include "usb_manager.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_net.h"
#include "class/hid/hid_device.h"

static const char *TAG = "USB_MGR";

#define CDC_RX_BUF_SIZE 512
#define TYPING_QUEUE_LEN 512

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

static usb_manager_config_t s_cfg;
static usb_mode_t s_current_mode = USB_MODE_COMPOSITE;
static bool s_driver_installed = false;
static bool s_cdc_ready = false;
static bool s_net_ready = false;

static uint8_t s_cdc_rx_temp[CDC_RX_BUF_SIZE];
static QueueHandle_t s_typing_queue = NULL;

/* -------------------- HID 报告描述符与配置描述符 -------------------- */

static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_NET,
    ITF_NUM_NET_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

enum {
    EPNUM_0_CDC_NOTIF = 1,
    EPNUM_0_CDC,
    EPNUM_NET_NOTIF,
    EPNUM_NET_DATA,
    EPNUM_HID
};

#define EPNUM_HID_IN  (0x80 | EPNUM_HID)
#define FULL_SPEED_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_composite_fs_cfg_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, FULL_SPEED_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // CDC-ACM Descriptor (Interface 0 & 1)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, 0x80 | EPNUM_0_CDC_NOTIF, 8, EPNUM_0_CDC, 0x80 | EPNUM_0_CDC, 64),

    // CDC-NCM Descriptor (Interface 2 & 3)
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NET, 5, 6, 0x80 | EPNUM_NET_NOTIF, 64, EPNUM_NET_DATA, 0x80 | EPNUM_NET_DATA, 64, CFG_TUD_NET_MTU),

    // HID Keyboard & Mouse Descriptor (Interface 4, EP 0x85 IN, 16 bytes buffer, 5ms polling)
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID_IN, 16, 5)
};

/* -------------------- 自动打字流后台任务 -------------------- */

static void hid_typing_task(void *arg)
{
    static const uint8_t conv_table[128][2] = { HID_ASCII_TO_KEYCODE };
    char ch;
    while (1) {
        if (xQueueReceive(s_typing_queue, &ch, portMAX_DELAY) == pdTRUE) {
            if (!tud_mounted()) {
                continue;
            }
            uint8_t u_ch = (uint8_t)ch;
            if (u_ch >= 128) continue;

            uint8_t mod = conv_table[u_ch][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            uint8_t kc = conv_table[u_ch][1];
            if (kc != 0) {
                uint8_t keys[6] = { kc, 0, 0, 0, 0, 0 };
                int retry = 0;
                while (!tud_hid_ready() && retry++ < 15) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, keys);
                vTaskDelay(pdMS_TO_TICKS(18));
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
                vTaskDelay(pdMS_TO_TICKS(12));
            }
        }
    }
}

/* -------------------- CDC-ACM 串口回调 -------------------- */

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, s_cdc_rx_temp, CDC_RX_BUF_SIZE - 1, &rx_size);
    if (ret == ESP_OK && rx_size > 0) {
        s_cdc_rx_temp[rx_size] = '\0';
        ESP_LOGD(TAG, "CDC RX (%d bytes): %s", (int)rx_size, s_cdc_rx_temp);

        // 如果用户注册了回调，转发给用户
        if (s_cfg.on_serial_rx) {
            s_cfg.on_serial_rx(s_cdc_rx_temp, rx_size, s_cfg.serial_user_ctx);
        } else {
            // 默认测试回环 Echo (带前缀)
            const char echo_header[] = "[ESP32-S3-GEEK Echo]: ";
            tinyusb_cdcacm_write_queue(itf, (const uint8_t *)echo_header, strlen(echo_header));
            tinyusb_cdcacm_write_queue(itf, s_cdc_rx_temp, rx_size);
            tinyusb_cdcacm_write_flush(itf, 0);
        }
    }
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC-ACM line state changed: DTR=%d, RTS=%d", dtr, rts);
}

static void cdc_line_coding_callback(int itf, cdcacm_event_t *event)
{
    if (event->line_coding_changed_data.p_line_coding) {
        uint32_t baud = event->line_coding_changed_data.p_line_coding->bit_rate;
        ESP_LOGI(TAG, "CDC-ACM line coding changed: Baudrate=%lu", (unsigned long)baud);
        if (baud == 1200) {
            ESP_LOGW(TAG, "1200-baud touch detected! Rebooting into ROM Bootloader Download Mode...");
            vTaskDelay(pdMS_TO_TICKS(100));
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }
    }
}

/* -------------------- CDC-NCM 虚拟网卡回调 -------------------- */

static esp_err_t net_rx_callback(void *buffer, uint16_t len, void *ctx)
{
    ESP_LOGD(TAG, "NCM Net frame RX (%d bytes)", len);
    if (s_cfg.on_net_rx) {
        s_cfg.on_net_rx(buffer, len, s_cfg.net_user_ctx);
    }
    return ESP_OK;
}

static void net_free_tx_buffer(void *eb, void *ctx)
{
    (void)eb;
    (void)ctx;
}

/* -------------------- 公共功能接口 -------------------- */

size_t usb_serial_write(const uint8_t *data, size_t len)
{
    if (!s_cdc_ready || !data || len == 0) return 0;

    size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return queued;
}

esp_err_t usb_net_send(const void *buffer, uint16_t len)
{
    if (!s_net_ready || !buffer || len == 0) return ESP_ERR_INVALID_STATE;
    return tinyusb_net_send_sync((void *)buffer, len, NULL, pdMS_TO_TICKS(100));
}

void usb_net_set_link_state(bool link_up)
{
    if (s_net_ready) {
        tud_network_link_state(0, link_up);
        ESP_LOGI(TAG, "NCM Net link state set to: %s", link_up ? "UP" : "DOWN");
    }
}

usb_mode_t usb_manager_get_current_mode(void)
{
    return s_current_mode;
}

/* -------------------- HID 键鼠公共接口 -------------------- */

bool usb_hid_is_ready(void)
{
    return s_driver_installed && tud_mounted() && tud_hid_ready();
}

esp_err_t usb_hid_send_keyboard(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!s_driver_installed || !tud_mounted()) return ESP_ERR_INVALID_STATE;
    bool ok = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, (uint8_t *)keycodes);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t usb_hid_send_keystroke(uint8_t modifier, uint8_t keycode)
{
    if (!usb_hid_is_ready()) return ESP_ERR_INVALID_STATE;
    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keys);
    vTaskDelay(pdMS_TO_TICKS(20));
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t usb_hid_type_string(const char *str)
{
    if (!str || !s_typing_queue) return ESP_ERR_INVALID_ARG;
    if (!s_driver_installed || !tud_mounted()) return ESP_ERR_INVALID_STATE;

    while (*str) {
        char ch = *str++;
        xQueueSend(s_typing_queue, &ch, pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

esp_err_t usb_hid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    if (!s_driver_installed || !tud_mounted()) return ESP_ERR_INVALID_STATE;
    bool ok = tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, wheel, pan);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t usb_hid_mouse_click(uint8_t button)
{
    if (!usb_hid_is_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_mouse_report(REPORT_ID_MOUSE, button, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    tud_hid_mouse_report(REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
    return ESP_OK;
}

static esp_err_t init_submodules_for_mode(usb_mode_t mode)
{
    esp_err_t ret = ESP_OK;

    // 1. 初始化 CDC-ACM 串口
    if (mode == USB_MODE_COMPOSITE || mode == USB_MODE_PURE_SERIAL) {
        tinyusb_config_cdcacm_t acm_cfg = {
            .cdc_port = TINYUSB_CDC_ACM_0,
            .callback_rx = &cdc_rx_callback,
            .callback_rx_wanted_char = NULL,
            .callback_line_state_changed = &cdc_line_state_callback,
            .callback_line_coding_changed = &cdc_line_coding_callback,
        };
        ret = tinyusb_cdcacm_init(&acm_cfg);
        if (ret == ESP_OK) {
            s_cdc_ready = true;
            ESP_LOGI(TAG, "CDC-ACM Serial Channel 0 initialized successfully");
        } else {
            ESP_LOGE(TAG, "Failed to init CDC-ACM: %d", ret);
        }
    }

    // 2. 初始化 CDC-NCM 虚拟网卡
    if (mode == USB_MODE_COMPOSITE || mode == USB_MODE_PURE_NET) {
        tinyusb_net_config_t net_cfg = {
            .on_recv_callback = net_rx_callback,
            .free_tx_buffer = net_free_tx_buffer,
            .user_context = NULL,
        };
        esp_read_mac(net_cfg.mac_addr, ESP_MAC_WIFI_STA);
        // 翻转局部/全局位，生成唯一的虚拟以太网 MAC
        net_cfg.mac_addr[0] |= 0x02;

        ESP_LOGI(TAG, "NCM Ethernet MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
                 net_cfg.mac_addr[0], net_cfg.mac_addr[1], net_cfg.mac_addr[2],
                 net_cfg.mac_addr[3], net_cfg.mac_addr[4], net_cfg.mac_addr[5]);

        ret = tinyusb_net_init(&net_cfg);
        if (ret == ESP_OK) {
            s_net_ready = true;
            ESP_LOGI(TAG, "CDC-NCM Network Interface initialized successfully");
            // 默认设置 Link Up 广播
            usb_net_set_link_state(true);
        } else {
            ESP_LOGE(TAG, "Failed to init CDC-NCM: %d", ret);
        }
    }

    return ret;
}

esp_err_t usb_manager_switch_mode(usb_mode_t new_mode)
{
    if (new_mode == s_current_mode && s_driver_installed) {
        ESP_LOGI(TAG, "Mode unchanged (%d), skip switch", new_mode);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Switching USB Mode from %d to %d...", s_current_mode, new_mode);

    // 1. 模拟物理拔出，断开 USB D+/D-
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    s_cdc_ready = false;
    s_net_ready = false;
    s_current_mode = new_mode;

    // 2. 重新初始化对应模式外设
    init_submodules_for_mode(new_mode);

    // 3. 模拟物理插入，重新上拉 D+
    vTaskDelay(pdMS_TO_TICKS(200));
    tud_connect();

    ESP_LOGI(TAG, "USB Re-enumeration completed (Mode: %d)", new_mode);
    return ESP_OK;
}

esp_err_t usb_manager_init(const usb_manager_config_t *config)
{
    if (config) {
        memcpy(&s_cfg, config, sizeof(usb_manager_config_t));
    }

    s_current_mode = config ? config->initial_mode : USB_MODE_COMPOSITE;

    ESP_LOGI(TAG, "Installing TinyUSB Driver (Initial Mode: %d)...", s_current_mode);
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.full_speed_config = s_composite_fs_cfg_desc;

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %d", ret);
        return ret;
    }
    s_driver_installed = true;

    // 初始化自动打字流后台队列与任务
    if (!s_typing_queue) {
        s_typing_queue = xQueueCreate(TYPING_QUEUE_LEN, sizeof(char));
        xTaskCreate(hid_typing_task, "hid_typing", 2560, NULL, 4, NULL);
    }

    // 初始化子模块
    ret = init_submodules_for_mode(s_current_mode);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "TinyUSB Driver installed and running (Composite: CDC-ACM + CDC-NCM + HID)!");
    return ESP_OK;
}

void usb_manager_disconnect(void)
{
    if (s_driver_installed) {
        ESP_LOGI(TAG, "USB disconnecting via tud_disconnect...");
        tud_disconnect();
    }
}

