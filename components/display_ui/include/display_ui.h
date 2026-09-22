#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 调试器工作模式定义
 */
typedef enum {
    DEBUGGER_MODE_COMPOSITE = 0,    /*!< 复合模式: CDC-ACM串口 + CDC-NCM网卡 (默认推荐) */
    DEBUGGER_MODE_PURE_SERIAL,      /*!< 纯串口模式: 仅枚举 CDC-ACM 控制台 */
    DEBUGGER_MODE_PURE_NET,         /*!< 纯网卡模式: 仅枚举 CDC-NCM 虚拟以太网 */
    DEBUGGER_OPT_WIFI_TOGGLE,       /*!< Wi-Fi 重置为 AP 配网热点 */
    DEBUGGER_OPT_DOWNLOAD_MODE,     /*!< 进入 ROM 固件下载模式 (DFU / Bootloader) */
    DEBUGGER_OPT_SD_NEW_SESSION,    /*!< TF卡: 开启新 Session 日志文件 */
    DEBUGGER_OPT_SD_EJECT,          /*!< TF卡: 刷盘并安全弹出 */
    DEBUGGER_OPT_SD_FORMAT,         /*!< TF卡: 格式化存储卡 */
    DEBUGGER_OPT_SCREEN_ROTATE,     /*!< 切换屏幕方向 (0° / 180°) */
    DEBUGGER_OPT_BACKLIGHT_CYCLE,   /*!< 循环切换背光亮度 (25% -> 50% -> 75% -> 100%) */
    DEBUGGER_OPT_FACTORY_RESET,     /*!< 恢复出厂设置并擦除 NVS */
    DEBUGGER_OPT_REBOOT,            /*!< 系统重启 */
    DEBUGGER_MENU_MAX
} debugger_mode_t;

/**
 * @brief UI 界面模式
 */
typedef enum {
    UI_VIEW_DASHBOARD,              /*!< 仪表盘监控界面 */
    UI_VIEW_MENU,                   /*!< 菜单选择界面 (短按切换) */
    UI_VIEW_APPLYING,               /*!< 正在应用配置提示 (长按确认后) */
    UI_VIEW_INFO,                   /*!< 详情信息界面 (单击切页，长按返回) */
} ui_view_t;

/**
 * @brief UI 界面上的 Wi-Fi 状态枚举
 */
typedef enum {
    UI_WIFI_STATUS_AP = 0,          /*!< AP 配网热点就绪 */
    UI_WIFI_STATUS_CONNECTING,      /*!< 正在连接目标路由中 */
    UI_WIFI_STATUS_ONLINE,          /*!< 已连入局域网在线 */
    UI_WIFI_STATUS_FALLBACK,        /*!< 连接失败，回退至 AP 配网 */
} ui_wifi_status_t;

/**
 * @brief 运行时状态数据，供仪表盘与 Info 页面显示
 */
typedef struct {
    debugger_mode_t current_mode;
    ui_wifi_status_t wifi_status;   /*!< 当前 Wi-Fi 状态 */
    char wifi_ssid[33];             /*!< 当前连接的 SSID 或 AP 名称 */
    char wifi_ip[20];               /*!< 例如 "192.168.4.1" 或 "192.168.1.x" */
    char target_ip[20];             /*!< 例如 "192.168.4.2" */
    uint32_t baud_rate;             /*!< 例如 115200 */
    bool serial_connected;          /*!< 目标机串口是否打开 */
    bool net_link_up;               /*!< 虚拟网卡是否 Link Up */
    uint32_t rx_bytes_sec;          /*!< 接收速率 (Bytes/s) */
    uint32_t tx_bytes_sec;          /*!< 发送速率 (Bytes/s) */
    bool sd_mounted;                /*!< TF 卡是否已挂载 */
    uint32_t sd_total_mb;           /*!< TF 卡容量 (MB) */
    uint32_t sd_free_mb;            /*!< TF 卡剩余空间 (MB) */
    uint32_t sd_session_id;         /*!< 当前会话 ID */
    uint32_t sd_file_bytes;         /*!< 当前日志写入大小 (Bytes) */
    uint32_t free_heap_kb;          /*!< 空闲内存 (KB) */
    uint32_t min_heap_kb;           /*!< 最小历史空闲内存 (KB) */
    uint32_t uptime_sec;            /*!< 运行时间 (秒) */
    char sta_gw[20];                /*!< 网关 IP */
    char ap_ssid[33];               /*!< AP SSID */
    char ap_ip[20];                 /*!< AP IP */
    uint8_t wifi_channel;           /*!< Wi-Fi 信道 */
    uint8_t mac_addr[6];            /*!< STA/AP MAC 地址 */
} ui_status_data_t;

/**
 * @brief 模式确认生效回调函数原型
 */
typedef void (*ui_mode_apply_cb_t)(debugger_mode_t selected_mode, void *user_ctx);

/**
 * @brief UI 控制器初始化参数
 */
typedef struct {
    int pin_mosi;                   /*!< ESP32-S3-GEEK: GPIO 11 */
    int pin_sclk;                   /*!< ESP32-S3-GEEK: GPIO 12 */
    int pin_cs;                     /*!< ESP32-S3-GEEK: GPIO 10 */
    int pin_dc;                     /*!< ESP32-S3-GEEK: GPIO 8 */
    int pin_rst;                    /*!< ESP32-S3-GEEK: GPIO 9 */
    int pin_bl;                     /*!< ESP32-S3-GEEK: GPIO 7 */
    debugger_mode_t initial_mode;   /*!< 开机默认模式 */
    ui_mode_apply_cb_t on_apply;    /*!< 长按确认时的回调 */
    void *user_ctx;                 /*!< 用户上下文 */
} display_ui_config_t;

/**
 * @brief 初始化 ST7789 屏幕并启动 UI 渲染任务
 *
 * @param config 配置参数
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t display_ui_init(const display_ui_config_t *config);

/**
 * @brief 响应按键短按事件 (若在 Dashboard 则唤起菜单；若在 Menu 则循环选择下一项)
 */
void display_ui_on_short_press(void);

/**
 * @brief 响应长按蓄力进度更新 (0 ~ 100)
 *
 * @param progress 蓄力百分比
 */
void display_ui_on_long_press_tick(uint8_t progress);

/**
 * @brief 响应长按取消 (用户未蓄满即松手)
 */
void display_ui_on_long_press_cancel(void);

/**
 * @brief 响应长按确认事件 (执行当前选中的菜单项)
 */
void display_ui_on_long_press_confirm(void);

/**
 * @brief 更新运行时仪表盘状态数据
 *
 * @param data 状态数据
 */
void display_ui_update_status(const ui_status_data_t *data);

/**
 * @brief 设置屏幕背光亮度
 *
 * @param percent 亮度百分比 (0 ~ 100)
 */
void display_ui_set_backlight(uint8_t percent);

/**
 * @brief 设置屏幕横向旋转方向
 *
 * @param inverted true 为翻转 180°，false 为默认 0°
 */
void display_ui_set_rotation(bool inverted);

/**
 * @brief 获取当前屏幕是否处于 180° 翻转模式
 */
bool display_ui_get_rotation(void);

/**
 * @brief 切换屏幕横向旋转方向 (0° <-> 180°)
 */
void display_ui_toggle_rotation(void);

#ifdef __cplusplus
}
#endif
