#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "button_ctrl.h"
#include "display_ui.h"
#include "usb_manager.h"
#include "net_bridge.h"
#include "sd_logger.h"
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "APP_MAIN";

// ESP32-S3-GEEK 硬件外设引脚定义
#define GEEK_PIN_KEY        0       // BOOT 按键
#define GEEK_PIN_LCD_MOSI   11      // LCD SPI MOSI
#define GEEK_PIN_LCD_SCLK   12      // LCD SPI SCLK
#define GEEK_PIN_LCD_CS     10      // LCD SPI CS
#define GEEK_PIN_LCD_DC     8       // LCD DC (Data/Command)
#define GEEK_PIN_LCD_RST    9       // LCD Reset
#define GEEK_PIN_LCD_BL     7       // LCD Backlight

// TF 卡 (MicroSD SDMMC Slot 1) 原生硬件引脚
#define GEEK_PIN_SD_CLK     36      // SDMMC CLK
#define GEEK_PIN_SD_CMD     35      // SDMMC CMD
#define GEEK_PIN_SD_D0      37      // SDMMC Data 0
#define GEEK_PIN_SD_D1      33      // SDMMC Data 1
#define GEEK_PIN_SD_D2      38      // SDMMC Data 2
#define GEEK_PIN_SD_D3      34      // SDMMC Data 3

/**
 * @brief 虚拟串口数据接收与全量 TF 卡会话日志记录
 */
static void app_serial_rx_handler(const uint8_t *data, size_t len, void *user_ctx)
{
    // 1. 将 Host 敲入发给 Target 的命令记录到当前 Session (TX)
    sd_logger_log_tx(data, len);

    // 2. 响应 Echo 回显给 Host
    const char echo_header[] = "[ESP32-S3-GEEK Echo]: ";
    usb_serial_write((const uint8_t *)echo_header, strlen(echo_header));
    usb_serial_write(data, len);

    // 3. 将 Target 回显与输出记录到当前 Session (RX)
    sd_logger_log_rx((const uint8_t *)echo_header, strlen(echo_header));
    sd_logger_log_rx(data, len);
}

/**
 * @brief 按键事件处理回调函数
 */
static void app_button_event_handler(const button_event_data_t *evt, void *user_ctx)
{
    switch (evt->event) {
    case BUTTON_EVENT_SHORT_PRESS:
        ESP_LOGI(TAG, "Key SHORT PRESS detected (duration %lu ms) -> Switching menu item", (unsigned long)evt->press_duration_ms);
        display_ui_on_short_press();
        break;

    case BUTTON_EVENT_LONG_PRESS_TICK:
        ESP_LOGD(TAG, "Key LONG PRESS charging: %d%%", evt->progress_percent);
        display_ui_on_long_press_tick(evt->progress_percent);
        break;

    case BUTTON_EVENT_LONG_PRESS_CANCEL:
        ESP_LOGI(TAG, "Key LONG PRESS cancelled");
        display_ui_on_long_press_cancel();
        break;

    case BUTTON_EVENT_LONG_PRESS_CONFIRM:
        ESP_LOGW(TAG, "Key LONG PRESS CONFIRM (held > %lu ms) -> Executing selection!", (unsigned long)evt->press_duration_ms);
        display_ui_on_long_press_confirm();
        break;

    default:
        break;
    }
}

/**
 * @brief UI 确认长按模式生效回调函数
 */
static void app_mode_apply_handler(debugger_mode_t mode, void *user_ctx)
{
    ESP_LOGI(TAG, "=================================================");
    switch (mode) {
    case DEBUGGER_MODE_COMPOSITE:
        ESP_LOGI(TAG, "Action: Switch to COMPOSITE Mode (CDC-ACM + CDC-NCM)");
        usb_manager_switch_mode(USB_MODE_COMPOSITE);
        break;

    case DEBUGGER_MODE_PURE_SERIAL:
        ESP_LOGI(TAG, "Action: Switch to PURE SERIAL Mode (CDC-ACM Only)");
        usb_manager_switch_mode(USB_MODE_PURE_SERIAL);
        break;

    case DEBUGGER_MODE_PURE_NET:
        ESP_LOGI(TAG, "Action: Switch to PURE NETWORK Mode (CDC-NCM Only)");
        usb_manager_switch_mode(USB_MODE_PURE_NET);
        break;

    case DEBUGGER_OPT_WIFI_TOGGLE:
        ESP_LOGI(TAG, "Action: Reset Wi-Fi config and Fallback to AP Portal");
        net_bridge_reset_to_ap();
        break;

    case DEBUGGER_OPT_DOWNLOAD_MODE:
        ESP_LOGW(TAG, "Action: Entering ROM Bootloader Download Mode...");
        usb_manager_disconnect();
        vTaskDelay(pdMS_TO_TICKS(600));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
        break;

    case DEBUGGER_OPT_SD_NEW_SESSION:
        ESP_LOGI(TAG, "Action: Starting new TF Card Session file...");
        sd_logger_start_new_session();
        break;

    case DEBUGGER_OPT_SD_EJECT:
        ESP_LOGW(TAG, "Action: Flushing buffers & Ejecting TF Card...");
        sd_logger_eject();
        break;

    case DEBUGGER_OPT_REBOOT:
        ESP_LOGW(TAG, "Action: System Rebooting in 1 second...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        break;

    default:
        break;
    }
    ESP_LOGI(TAG, "=================================================");
}

static ui_status_data_t s_current_ui_status = {
    .current_mode = DEBUGGER_MODE_COMPOSITE,
    .wifi_status = UI_WIFI_STATUS_AP,
    .wifi_ssid = "GEEK-Debugger",
    .wifi_ip = "192.168.4.1",
    .target_ip = "192.168.4.2",
    .baud_rate = 115200,
    .serial_connected = true,
    .net_link_up = true,
    .rx_bytes_sec = 0,
    .tx_bytes_sec = 0,
    .sd_mounted = false,
    .sd_total_mb = 0,
    .sd_session_id = 0,
    .sd_file_bytes = 0,
};

static void app_wifi_state_handler(net_wifi_state_t state, const char *ssid, const char *ip_str, void *user_ctx)
{
    switch (state) {
    case NET_WIFI_STATE_AP_PORTAL:
        s_current_ui_status.wifi_status = UI_WIFI_STATUS_AP;
        strcpy(s_current_ui_status.wifi_ssid, "GEEK-Debugger");
        strcpy(s_current_ui_status.wifi_ip, "192.168.4.1");
        ESP_LOGI(TAG, "UI Updated: AP Portal Ready (192.168.4.1)");
        break;

    case NET_WIFI_STATE_CONNECTING:
        s_current_ui_status.wifi_status = UI_WIFI_STATUS_CONNECTING;
        if (ssid) strncpy(s_current_ui_status.wifi_ssid, ssid, sizeof(s_current_ui_status.wifi_ssid) - 1);
        ESP_LOGI(TAG, "UI Updated: Connecting to %s...", s_current_ui_status.wifi_ssid);
        break;

    case NET_WIFI_STATE_CONNECTED:
        s_current_ui_status.wifi_status = UI_WIFI_STATUS_ONLINE;
        if (ssid) strncpy(s_current_ui_status.wifi_ssid, ssid, sizeof(s_current_ui_status.wifi_ssid) - 1);
        if (ip_str) strncpy(s_current_ui_status.wifi_ip, ip_str, sizeof(s_current_ui_status.wifi_ip) - 1);
        ESP_LOGI(TAG, "UI Updated: Connected! IP=%s", s_current_ui_status.wifi_ip);
        break;

    case NET_WIFI_STATE_FALLBACK_AP:
        s_current_ui_status.wifi_status = UI_WIFI_STATUS_FALLBACK;
        strcpy(s_current_ui_status.wifi_ip, "192.168.4.1");
        ESP_LOGW(TAG, "UI Updated: Fallback to AP!");
        break;
    }
    display_ui_update_status(&s_current_ui_status);
}

/**
 * @brief 后台状态监控模拟任务 (周期性向 UI 推送心跳及监控数据)
 */
static void status_monitor_task(void *pvParameters)
{
    uint32_t counter = 0;
    while (1) {
        counter++;
        // 动态更新网络吞吐起伏
        s_current_ui_status.rx_bytes_sec = 1024 * (10 + (counter % 12));
        s_current_ui_status.tx_bytes_sec = 1024 * (1 + (counter % 4));

        // 动态更新 TF 卡状态
        sd_logger_status_t sd_st;
        sd_logger_get_status(&sd_st);
        s_current_ui_status.sd_mounted = sd_st.card_mounted;
        s_current_ui_status.sd_total_mb = sd_st.total_mb;
        s_current_ui_status.sd_session_id = sd_st.current_session_id;
        s_current_ui_status.sd_file_bytes = sd_st.current_file_bytes;

        display_ui_update_status(&s_current_ui_status);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "*****************************************************");
    ESP_LOGI(TAG, "* Waveshare ESP32-S3-GEEK Wireless Debugger Starting *");
    ESP_LOGI(TAG, "*****************************************************");

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 1.14寸 ST7789 屏幕与 UI 渲染系统
    display_ui_config_t ui_cfg = {
        .pin_mosi = GEEK_PIN_LCD_MOSI,
        .pin_sclk = GEEK_PIN_LCD_SCLK,
        .pin_cs   = GEEK_PIN_LCD_CS,
        .pin_dc   = GEEK_PIN_LCD_DC,
        .pin_rst  = GEEK_PIN_LCD_RST,
        .pin_bl   = GEEK_PIN_LCD_BL,
        .initial_mode = DEBUGGER_MODE_COMPOSITE,
        .on_apply = app_mode_apply_handler,
        .user_ctx = NULL,
    };

    ret = display_ui_init(&ui_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display UI initialization failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Display UI initialized successfully!");
    }

    // 3. 初始化单按键控制器 (GPIO 0, 消抖 20ms, 短按 50~600ms, 长按 >1500ms 确认)
    button_config_t btn_cfg = {
        .gpio_num = GEEK_PIN_KEY,
        .active_low = true,
        .debounce_ms = 20,
        .short_press_max_ms = 600,
        .long_press_threshold_ms = 1500,
        .callback = app_button_event_handler,
        .user_ctx = NULL,
    };

    ret = button_ctrl_init(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button controller initialization failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Button controller initialized on GPIO %d (Short:Next / Long:Confirm)", GEEK_PIN_KEY);
    }

    // 4. 初始化 TF 卡 (MicroSD) 与全量会话日志记录器
    sd_logger_config_t sd_cfg = {
        .pin_clk = GEEK_PIN_SD_CLK,
        .pin_cmd = GEEK_PIN_SD_CMD,
        .pin_d0  = GEEK_PIN_SD_D0,
        .pin_d1  = GEEK_PIN_SD_D1,
        .pin_d2  = GEEK_PIN_SD_D2,
        .pin_d3  = GEEK_PIN_SD_D3,
        .auto_new_session = true,
    };
    ret = sd_logger_init(&sd_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TF Card & Session Logger initialized successfully!");
    } else {
        ESP_LOGW(TAG, "TF Card not detected or failed to mount (%d), continuing in cardless mode", ret);
    }

    // 5. 初始化 USB 复合设备 (TinyUSB CDC-ACM 串口 + CDC-NCM 虚拟网卡)
    usb_manager_config_t usb_cfg = {
        .initial_mode = USB_MODE_COMPOSITE,
        .on_serial_rx = app_serial_rx_handler, // 自动全量捕获 TX/RX 并写入 TF 卡 Session 日志
        .on_net_rx = NULL,
    };
    ret = usb_manager_init(&usb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Manager initialization failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "USB Manager initialized in COMPOSITE mode (CDC-ACM + CDC-NCM)");
    }

    // 6. 初始化 Wi-Fi 管理器 (AP 配网热点 + STA 自动连接 + Fallback 回退保护 + TF卡Web管理器)
    net_bridge_config_t net_cfg = {
        .ap_ssid = "GEEK-Debugger",
        .ap_password = NULL, // 免密热点，开箱即用连入配网
        .on_state_change = app_wifi_state_handler,
        .user_ctx = NULL,
    };
    ret = net_bridge_init(&net_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Net Bridge initialization failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Net Bridge (Wi-Fi Portal & Log Manager) initialized successfully!");
    }

    // 6. 启动状态监控心跳任务
    xTaskCreate(status_monitor_task, "status_task", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "System initialization complete. Enjoy wireless debugging!");
}
