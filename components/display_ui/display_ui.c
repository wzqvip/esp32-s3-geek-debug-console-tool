#include "display_ui.h"
#include "font8x16.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "DISPLAY_UI";

#define LCD_WIDTH   240
#define LCD_HEIGHT  135
#define LCD_GAP_X   40
#define LCD_GAP_Y   53

// 常用 RGB565 颜色定义
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_NAVY        0x000F
#define COLOR_DARKCYAN    0x03EF
#define COLOR_CYAN        0x07FF
#define COLOR_GREEN       0x07E0
#define COLOR_DARKGREEN   0x03E0
#define COLOR_YELLOW      0xFFE0
#define COLOR_ORANGE      0xFD20
#define COLOR_RED         0xF800
#define COLOR_LIGHTGREY   0xC618
#define COLOR_DARKGREY    0x39E7
#define COLOR_PURPLE      0x780F
#define COLOR_MAGENTA     0xF81F
#define COLOR_BLUE        0x001F

static display_ui_config_t s_cfg;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint16_t *s_frame_buffer = NULL;
static SemaphoreHandle_t s_ui_mutex = NULL;

static ui_view_t s_current_view = UI_VIEW_DASHBOARD;
static debugger_mode_t s_current_mode = DEBUGGER_MODE_COMPOSITE;
static int s_menu_cursor = 0;
static uint8_t s_charging_progress = 0;
static int64_t s_last_action_time_ms = 0;
static ui_status_data_t s_status_data;
static bool s_need_refresh = true;

static const char *s_menu_items[] = {
    "1. Composite (CDC+Net)",
    "2. Pure Serial (CDC)",
    "3. Pure Network (NCM)",
    "4. Wi-Fi: Reset to AP",
    "5. Enter Download Mode",
    "6. TF: New Session",
    "7. TF: Flush & Eject",
    "8. System Reboot",
};

/* -------------------- 基础绘图函数 -------------------- */

static inline void set_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        // ST7789 常用大端格式 (Big Endian)
        s_frame_buffer[y * LCD_WIDTH + x] = (color >> 8) | (color << 8);
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > LCD_WIDTH) x2 = LCD_WIDTH;
    if (y2 > LCD_HEIGHT) y2 = LCD_HEIGHT;

    uint16_t be_color = (color >> 8) | (color << 8);
    for (int j = y; j < y2; j++) {
        for (int i = x; i < x2; i++) {
            s_frame_buffer[j * LCD_WIDTH + i] = be_color;
        }
    }
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *bitmap = font8x16[c - 32];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits & (0x80 >> col)) != 0;
            set_pixel(x + col, y + row, on ? fg : bg);
        }
    }
}

static void draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        if (x + 8 > LCD_WIDTH) break;
        draw_char(x, y, *str, fg, bg);
        x += 8;
        str++;
    }
}

static void draw_progress_bar(int x, int y, int w, int h, uint8_t percent, uint16_t fg, uint16_t bg, uint16_t border)
{
    // 边框
    fill_rect(x, y, w, 1, border);
    fill_rect(x, y + h - 1, w, 1, border);
    fill_rect(x, y, 1, h, border);
    fill_rect(x + w - 1, y, 1, h, border);

    // 内容区
    int inner_w = w - 2;
    int inner_h = h - 2;
    int fill_w = (inner_w * percent) / 100;
    if (fill_w > inner_w) fill_w = inner_w;

    if (fill_w > 0) {
        fill_rect(x + 1, y + 1, fill_w, inner_h, fg);
    }
    if (inner_w - fill_w > 0) {
        fill_rect(x + 1 + fill_w, y + 1, inner_w - fill_w, inner_h, bg);
    }
}

/* -------------------- 页面视图渲染 -------------------- */

static void render_dashboard(void)
{
    // 1. 顶部状态条 (深灰背景)
    fill_rect(0, 0, LCD_WIDTH, 18, COLOR_DARKGREY);

    char mode_str[16] = "COMPOSITE";
    if (s_current_mode == DEBUGGER_MODE_PURE_SERIAL) strcpy(mode_str, "CDC-ONLY");
    else if (s_current_mode == DEBUGGER_MODE_PURE_NET) strcpy(mode_str, "NET-ONLY");

    const char *wifi_tag = "AP-CFG";
    uint16_t wifi_color = COLOR_YELLOW;
    if (s_status_data.wifi_status == UI_WIFI_STATUS_CONNECTING) {
        wifi_tag = "CONNECTING...";
        wifi_color = COLOR_ORANGE;
    } else if (s_status_data.wifi_status == UI_WIFI_STATUS_ONLINE) {
        wifi_tag = "STA-ONLINE";
        wifi_color = COLOR_GREEN;
    } else if (s_status_data.wifi_status == UI_WIFI_STATUS_FALLBACK) {
        wifi_tag = "FALLBACK->AP";
        wifi_color = COLOR_RED;
    }

    char top_bar[36];
    snprintf(top_bar, sizeof(top_bar), "[%s] %s", mode_str, wifi_tag);
    draw_string(4, 1, top_bar, wifi_color, COLOR_DARKGREY);

    // 2. 主体信息 (黑底)
    fill_rect(0, 18, LCD_WIDTH, LCD_HEIGHT - 18 - 16, COLOR_BLACK);

    char line[42];
    if (s_status_data.wifi_status == UI_WIFI_STATUS_ONLINE) {
        snprintf(line, sizeof(line), "STA-SSID : %.20s", s_status_data.wifi_ssid[0] ? s_status_data.wifi_ssid : "Connected");
        draw_string(6, 24, line, COLOR_CYAN, COLOR_BLACK);
        snprintf(line, sizeof(line), "Host IP  : %s", s_status_data.wifi_ip[0] ? s_status_data.wifi_ip : "192.168.1.x");
        draw_string(6, 42, line, COLOR_WHITE, COLOR_BLACK);
    } else if (s_status_data.wifi_status == UI_WIFI_STATUS_CONNECTING) {
        snprintf(line, sizeof(line), "Connecting: %.20s", s_status_data.wifi_ssid);
        draw_string(6, 24, line, COLOR_YELLOW, COLOR_BLACK);
        draw_string(6, 42, "Waiting for IP assignment...", COLOR_LIGHTGREY, COLOR_BLACK);
    } else if (s_status_data.wifi_status == UI_WIFI_STATUS_FALLBACK) {
        draw_string(6, 24, ">> Connect Failed! <<", COLOR_RED, COLOR_BLACK);
        draw_string(6, 42, "Fallback to AP: GEEK-Debugger", COLOR_ORANGE, COLOR_BLACK);
    } else {
        // AP 模式
        draw_string(6, 24, "AP SSID  : GEEK-Debugger", COLOR_CYAN, COLOR_BLACK);
        draw_string(6, 42, "Web Portal: 192.168.4.1", COLOR_WHITE, COLOR_BLACK);
    }

    snprintf(line, sizeof(line), "Target IP: %s (usb0)", s_status_data.target_ip[0] ? s_status_data.target_ip : "192.168.4.2");
    draw_string(6, 60, line, COLOR_GREEN, COLOR_BLACK);

    // TF 卡与会话记录状态
    if (s_status_data.sd_mounted) {
        snprintf(line, sizeof(line), "TF: %luGB OK  Log:#%03lu (%luK)",
                 (unsigned long)(s_status_data.sd_total_mb / 1024),
                 (unsigned long)s_status_data.sd_session_id,
                 (unsigned long)(s_status_data.sd_file_bytes / 1024));
        draw_string(6, 78, line, COLOR_MAGENTA, COLOR_BLACK);
    } else {
        draw_string(6, 78, "TF Card: NO CARD (Insert to log)", COLOR_DARKGREY, COLOR_BLACK);
    }

    // 流量指示
    snprintf(line, sizeof(line), "RX: %3lu KB/s  TX: %3lu KB/s",
             (unsigned long)(s_status_data.rx_bytes_sec / 1024),
             (unsigned long)(s_status_data.tx_bytes_sec / 1024));
    draw_string(6, 96, line, COLOR_DARKCYAN, COLOR_BLACK);

    // 3. 底部操作提示
    fill_rect(0, LCD_HEIGHT - 16, LCD_WIDTH, 16, COLOR_NAVY);
    draw_string(6, LCD_HEIGHT - 15, ">> Short Press to Enter Menu <<", COLOR_YELLOW, COLOR_NAVY);
}

static void render_menu(void)
{
    // 1. 顶部标题
    fill_rect(0, 0, LCD_WIDTH, 17, COLOR_NAVY);
    draw_string(4, 1, "SELECT (Short:Next / Long:OK)", COLOR_WHITE, COLOR_NAVY);

    // 2. 菜单项列表
    fill_rect(0, 17, LCD_WIDTH, LCD_HEIGHT - 17 - 18, COLOR_BLACK);

    // 最多同屏显示 6 项
    int max_visible = 6;
    int top_index = 0;
    if (s_menu_cursor >= max_visible) {
        top_index = s_menu_cursor - max_visible + 1;
    }

    int start_y = 19;
    int line_h = 16;
    for (int idx = 0; idx < max_visible && (top_index + idx) < DEBUGGER_MENU_MAX; idx++) {
        int i = top_index + idx;
        int item_y = start_y + idx * line_h;

        bool is_selected = (i == s_menu_cursor);
        uint16_t bg_color = is_selected ? COLOR_DARKCYAN : COLOR_BLACK;
        uint16_t fg_color = is_selected ? COLOR_WHITE : COLOR_LIGHTGREY;

        // 选中项背景高亮
        if (is_selected) {
            fill_rect(2, item_y - 1, LCD_WIDTH - 4, 16, bg_color);
        }

        char buf[36];
        char active_mark = (i == (int)s_current_mode) ? '*' : ' ';
        snprintf(buf, sizeof(buf), "%c%s", is_selected ? '>' : ' ', s_menu_items[i]);
        if (active_mark == '*') {
            int len = strlen(buf);
            if (len < 26) {
                buf[len] = ' ';
                buf[len+1] = '*';
                buf[len+2] = '\0';
            }
        }
        draw_string(4, item_y, buf, fg_color, bg_color);
    }

    // 3. 底部长按蓄力指示条
    fill_rect(0, LCD_HEIGHT - 18, LCD_WIDTH, 18, COLOR_BLACK);
    if (s_charging_progress > 0) {
        char prog_str[16];
        snprintf(prog_str, sizeof(prog_str), "Hold OK:%2d%%", s_charging_progress);
        draw_string(4, LCD_HEIGHT - 16, prog_str, COLOR_YELLOW, COLOR_BLACK);
        draw_progress_bar(100, LCD_HEIGHT - 15, 134, 13, s_charging_progress, COLOR_ORANGE, COLOR_DARKGREY, COLOR_WHITE);
    } else {
        fill_rect(0, LCD_HEIGHT - 18, LCD_WIDTH, 18, COLOR_DARKGREY);
        draw_string(6, LCD_HEIGHT - 16, "[Hold >1.5s to Confirm]", COLOR_CYAN, COLOR_DARKGREY);
    }
}

static void render_applying(void)
{
    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK);

    // 绘制弹窗线框
    fill_rect(10, 16, LCD_WIDTH - 20, LCD_HEIGHT - 32, COLOR_DARKCYAN);
    fill_rect(12, 18, LCD_WIDTH - 24, LCD_HEIGHT - 36, COLOR_BLACK);

    if (s_current_mode == DEBUGGER_OPT_DOWNLOAD_MODE) {
        draw_string(24, 28, "DOWNLOAD MODE (ROM)", COLOR_YELLOW, COLOR_BLACK);
        draw_string(20, 50, "Entering Bootloader...", COLOR_WHITE, COLOR_BLACK);
        draw_string(26, 68, "Ready for idf.py flash", COLOR_GREEN, COLOR_BLACK);
        draw_progress_bar(20, 88, LCD_WIDTH - 40, 8, 100, COLOR_ORANGE, COLOR_BLACK, COLOR_WHITE);
    } else if (s_current_mode == DEBUGGER_OPT_SD_NEW_SESSION) {
        draw_string(32, 28, "START NEW SESSION", COLOR_YELLOW, COLOR_BLACK);
        draw_string(24, 50, "Created new log file", COLOR_WHITE, COLOR_BLACK);
        draw_progress_bar(20, 80, LCD_WIDTH - 40, 8, 100, COLOR_MAGENTA, COLOR_BLACK, COLOR_WHITE);
    } else if (s_current_mode == DEBUGGER_OPT_SD_EJECT) {
        draw_string(36, 28, "TF FLUSH & EJECT", COLOR_YELLOW, COLOR_BLACK);
        draw_string(24, 50, "Safe to remove card", COLOR_WHITE, COLOR_BLACK);
        draw_progress_bar(20, 80, LCD_WIDTH - 40, 8, 100, COLOR_ORANGE, COLOR_BLACK, COLOR_WHITE);
    } else if (s_current_mode == DEBUGGER_OPT_REBOOT) {
        draw_string(40, 28, "SYSTEM REBOOTING", COLOR_YELLOW, COLOR_BLACK);
        draw_string(32, 50, "Restarting ESP32-S3...", COLOR_WHITE, COLOR_BLACK);
        draw_progress_bar(20, 80, LCD_WIDTH - 40, 8, 100, COLOR_CYAN, COLOR_BLACK, COLOR_WHITE);
    } else if (s_current_mode == DEBUGGER_OPT_WIFI_TOGGLE) {
        draw_string(40, 28, "RESETTING WI-FI", COLOR_YELLOW, COLOR_BLACK);
        draw_string(26, 50, "Switching to AP Portal", COLOR_WHITE, COLOR_BLACK);
        draw_progress_bar(20, 80, LCD_WIDTH - 40, 8, 100, COLOR_GREEN, COLOR_BLACK, COLOR_WHITE);
    } else {
        draw_string(44, 28, "Applying Mode...", COLOR_YELLOW, COLOR_BLACK);
        draw_string(34, 50, "USB Re-enumerating", COLOR_WHITE, COLOR_BLACK);
        draw_progress_bar(20, 80, LCD_WIDTH - 40, 8, 100, COLOR_GREEN, COLOR_BLACK, COLOR_WHITE);
    }
}

static void flush_screen(void)
{
    if (s_panel_handle && s_frame_buffer) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_frame_buffer);
    }
}

/* -------------------- 任务与事件调度 -------------------- */

static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Display UI task started");

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000LL;

        if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // 超时检测：如果处于菜单页面，且 5 秒无任何操作，自动退回 Dashboard
            if (s_current_view == UI_VIEW_MENU && s_charging_progress == 0) {
                if (now_ms - s_last_action_time_ms > 5000) {
                    s_current_view = UI_VIEW_DASHBOARD;
                    s_need_refresh = true;
                    ESP_LOGI(TAG, "Menu timeout, returning to dashboard");
                }
            }

            if (s_need_refresh) {
                s_need_refresh = false;

                switch (s_current_view) {
                case UI_VIEW_DASHBOARD:
                    render_dashboard();
                    break;
                case UI_VIEW_MENU:
                    render_menu();
                    break;
                case UI_VIEW_APPLYING:
                    render_applying();
                    break;
                }

                flush_screen();
            }

            xSemaphoreGive(s_ui_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // ~50Hz 刷新检测循环
    }

    vTaskDelete(NULL);
}

/* -------------------- 公共接口实现 -------------------- */

void display_ui_on_short_press(void)
{
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_last_action_time_ms = esp_timer_get_time() / 1000LL;
        s_charging_progress = 0;

        if (s_current_view == UI_VIEW_DASHBOARD) {
            // 仪表盘界面下，短按进入菜单模式
            s_current_view = UI_VIEW_MENU;
            s_menu_cursor = (int)s_current_mode; // 默认选中当前正在运行的模式
        } else if (s_current_view == UI_VIEW_MENU) {
            // 菜单界面下，短按向下循环切换选择项
            s_menu_cursor = (s_menu_cursor + 1) % DEBUGGER_MENU_MAX;
        }

        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void display_ui_on_long_press_tick(uint8_t progress)
{
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_last_action_time_ms = esp_timer_get_time() / 1000LL;

        // 如果在 Dashboard 收到长按，直接切换进入菜单并展示进度
        if (s_current_view == UI_VIEW_DASHBOARD) {
            s_current_view = UI_VIEW_MENU;
            s_menu_cursor = (int)s_current_mode;
        }

        s_charging_progress = progress;
        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void display_ui_on_long_press_cancel(void)
{
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_last_action_time_ms = esp_timer_get_time() / 1000LL;
        s_charging_progress = 0;
        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void display_ui_on_long_press_confirm(void)
{
    debugger_mode_t target_mode = DEBUGGER_MODE_COMPOSITE;

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_charging_progress = 100;
        target_mode = (debugger_mode_t)s_menu_cursor;
        s_current_mode = target_mode;
        s_current_view = UI_VIEW_APPLYING;
        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }

    ESP_LOGI(TAG, "Long press CONFIRM triggered! Target mode: %d", target_mode);

    // 回调外部模式切换函数
    if (s_cfg.on_apply) {
        s_cfg.on_apply(target_mode, s_cfg.user_ctx);
    }

    // 保持提示 1.2 秒后退回仪表盘
    vTaskDelay(pdMS_TO_TICKS(1200));

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_charging_progress = 0;
        s_current_view = UI_VIEW_DASHBOARD;
        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void display_ui_update_status(const ui_status_data_t *data)
{
    if (!data) return;
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(&s_status_data, data, sizeof(ui_status_data_t));
        s_need_refresh = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

#define LCD_LEDC_TIMER       LEDC_TIMER_0
#define LCD_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LCD_LEDC_CHANNEL     LEDC_CHANNEL_0
#define LCD_LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define LCD_LEDC_FREQ_HZ     5000

void display_ui_set_backlight(uint8_t percent)
{
    if (s_cfg.pin_bl >= 0) {
        if (percent > 100) percent = 100;
        // 10-bit resolution max duty is 1023
        uint32_t duty = (1023U * (uint32_t)percent) / 100U;
        ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL, duty);
        ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL);
    }
}

esp_err_t display_ui_init(const display_ui_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, config, sizeof(display_ui_config_t));
    s_current_mode = config->initial_mode;
    s_menu_cursor = (int)s_current_mode;

    s_ui_mutex = xSemaphoreCreateMutex();
    if (!s_ui_mutex) return ESP_ERR_NO_MEM;

    // 申请 240x135x2 字节双缓冲 FrameBuffer
    s_frame_buffer = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_frame_buffer) {
        // 若内部 RAM 紧张则从通用堆分配
        s_frame_buffer = (uint16_t *)malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    }
    if (!s_frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate FrameBuffer");
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame_buffer, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));

    // 1. 初始化背光引脚 (LEDC PWM 硬件无级平滑调光)
    if (s_cfg.pin_bl >= 0) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LCD_LEDC_MODE,
            .timer_num        = LCD_LEDC_TIMER,
            .duty_resolution  = LCD_LEDC_DUTY_RES,
            .freq_hz          = LCD_LEDC_FREQ_HZ,
            .clk_cfg          = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&ledc_timer);

        ledc_channel_config_t ledc_channel = {
            .speed_mode     = LCD_LEDC_MODE,
            .channel        = LCD_LEDC_CHANNEL,
            .timer_sel      = LCD_LEDC_TIMER,
            .intr_type      = LEDC_INTR_DISABLE,
            .gpio_num       = s_cfg.pin_bl,
            .duty           = 1023, // 默认 100% 满亮度
            .hpoint         = 0,
        };
        ledc_channel_config(&ledc_channel);
    }

    // 2. 初始化 SPI 总线 (SPI2_HOST)
    spi_bus_config_t buscfg = {
        .sclk_io_num = s_cfg.pin_sclk,
        .mosi_io_num = s_cfg.pin_mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SPI bus: %d", ret);
        return ret;
    }

    // 3. 配置 LCD Panel IO (DC, CS)
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = s_cfg.pin_dc,
        .cs_gpio_num = s_cfg.pin_cs,
        .pclk_hz = 40 * 1000 * 1000, // 40MHz 高速刷新
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %d", ret);
        return ret;
    }

    // 4. 初始化 ST7789 驱动
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = s_cfg.pin_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %d", ret);
        return ret;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true); // IPS 通常需要反色
    esp_lcd_panel_swap_xy(s_panel_handle, true);      // 横屏 (240 x 135)
    esp_lcd_panel_mirror(s_panel_handle, false, true);
    esp_lcd_panel_set_gap(s_panel_handle, LCD_GAP_X, LCD_GAP_Y);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // 默认展示仪表盘
    s_current_view = UI_VIEW_DASHBOARD;
    s_need_refresh = true;

    // 启动 UI 刷新任务
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);

    return ESP_OK;
}
