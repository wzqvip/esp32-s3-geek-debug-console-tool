#include "usb_manager.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_net.h"

static const char *TAG = "USB_MGR";

#define CDC_RX_BUF_SIZE 512

static usb_manager_config_t s_cfg;
static usb_mode_t s_current_mode = USB_MODE_COMPOSITE;
static bool s_driver_installed = false;
static bool s_cdc_ready = false;
static bool s_net_ready = false;

static uint8_t s_cdc_rx_temp[CDC_RX_BUF_SIZE];

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
    // 如果使用堆内存发送，可以在此释放
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
            .callback_line_coding_changed = NULL,
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
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %d", ret);
        return ret;
    }
    s_driver_installed = true;

    // 初始化子模块
    ret = init_submodules_for_mode(s_current_mode);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "TinyUSB Driver installed and running!");
    return ESP_OK;
}

void usb_manager_disconnect(void)
{
    if (s_driver_installed) {
        ESP_LOGI(TAG, "USB disconnecting via tud_disconnect...");
        tud_disconnect();
    }
}

