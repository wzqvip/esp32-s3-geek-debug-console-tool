#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi 状态枚举
 */
typedef enum {
    NET_WIFI_STATE_AP_PORTAL,       /*!< AP 模式配网中 (热点就绪，等待用户连接配置) */
    NET_WIFI_STATE_CONNECTING,      /*!< 正在连接目标 Wi-Fi 中... */
    NET_WIFI_STATE_CONNECTED,       /*!< 成功连入目标 Wi-Fi (STA 模式，正常在线) */
    NET_WIFI_STATE_FALLBACK_AP,     /*!< 连接目标 Wi-Fi 失败，已自动回退到 AP 配网模式 */
} net_wifi_state_t;

/**
 * @brief Wi-Fi 状态变化通知回调函数原型
 */
typedef void (*net_wifi_state_cb_t)(net_wifi_state_t state, const char *ssid, const char *ip_str, void *user_ctx);

/**
 * @brief 系统全局持久化配置参数结构体
 */
typedef struct {
    // 1. Wi-Fi STA (连网配置)
    char sta_ssid[33];
    char sta_pwd[65];
    bool sta_static_ip;             /*!< 是否启用静态 IP (false: DHCP, true: Static) */
    char sta_ip[16];                /*!< 静态 IP 地址 (如 192.168.1.200) */
    char sta_netmask[16];           /*!< 子网掩码 (如 255.255.255.0) */
    char sta_gw[16];                /*!< 网关 (如 192.168.1.1) */
    char sta_dns[16];               /*!< DNS 服务器 (如 8.8.8.8) */
    bool sta_auto_reconnect;        /*!< 断网自动重连 */

    // 2. Wi-Fi AP (本机热点配置)
    char ap_ssid[33];               /*!< 热点名称 (默认 GEEK-Debugger) */
    char ap_pwd[65];                /*!< 热点密码 (留空则为开放热点) */
    uint8_t ap_channel;             /*!< 热点信道 (1 - 13) */
    bool ap_hidden;                 /*!< 是否隐藏 SSID */
    uint8_t ap_max_conn;            /*!< 最大允许连接设备数 (1 - 4) */

    // 3. 串口与 USB 模式
    uint8_t usb_default_mode;       /*!< 默认启动模式: 0:Composite, 1:Pure Serial, 2:Pure Net */
    uint32_t serial_baudrate;       /*!< 串口波特率: 9600, 115200, 921600 等 */
    uint8_t serial_databits;        /*!< 数据位: 7, 8 */
    uint8_t serial_parity;          /*!< 校验位: 0:None, 1:Odd, 2:Even */
    uint8_t serial_stopbits;        /*!< 停止位: 1, 2 */

    // 4. TF 卡日志配置
    bool log_enabled;               /*!< 是否自动记录日志 */
    uint16_t log_flush_interval_ms; /*!< 刷盘周期 (ms) */
    uint32_t log_max_file_kb;       /*!< 单文件最大大小 (KB, 0 为不限制) */

    // 5. 屏幕与显示配置
    uint8_t display_brightness;     /*!< 亮度 10% - 100% */
    uint16_t display_timeout_s;     /*!< 熄屏时间 (秒, 0 为常亮) */
    uint8_t display_rotation;       /*!< 方向 (0: 默认, 2: 180度反转) */
} sys_config_t;

/**
 * @brief 网络管理器配置
 */
typedef struct {
    const char *ap_ssid;            /*!< 默认 AP 热点名称 */
    const char *ap_password;        /*!< 默认 AP 热点密码 */
    net_wifi_state_cb_t on_state_change;
    void *user_ctx;
} net_bridge_config_t;

/**
 * @brief Web 终端数据发送回调函数 (Web 端输入命令通过此回调注入串口)
 */
typedef void (*web_terminal_tx_fn_t)(const uint8_t *data, size_t len);

/**
 * @brief 初始化网络管理器并启动 Wi-Fi 状态机与 Web 服务器
 */
esp_err_t net_bridge_init(const net_bridge_config_t *config);

/**
 * @brief 获取当前系统配置结构指针 (只读)
 */
const sys_config_t *net_bridge_get_sys_config(void);

/**
 * @brief 保存系统配置到 NVS
 */
esp_err_t net_bridge_save_sys_config(const sys_config_t *cfg);

/**
 * @brief 重置配置为出厂默认并清除 NVS
 */
esp_err_t net_bridge_factory_reset_config(void);

/**
 * @brief 从配网接口触发连接新 Wi-Fi (可带静态 IP 参数)
 */
esp_err_t net_bridge_connect_wifi_full(const char *ssid, const char *password,
                                       bool static_ip, const char *ip,
                                       const char *netmask, const char *gw, const char *dns);

/**
 * @brief 传统快速连入接口
 */
esp_err_t net_bridge_connect_wifi(const char *ssid, const char *password);

/**
 * @brief 重新配置并应用 AP 热点参数
 */
esp_err_t net_bridge_configure_ap(const char *ssid, const char *password,
                                  uint8_t channel, bool hidden, uint8_t max_conn);

/**
 * @brief 强制重置 Wi-Fi 配置并切回 AP 配网热点
 */
esp_err_t net_bridge_reset_to_ap(void);

/**
 * @brief 获取当前 Wi-Fi 状态
 */
net_wifi_state_t net_bridge_get_state(void);

/**
 * @brief 获取当前有效的 IP 地址字符串
 */
const char *net_bridge_get_ip_str(void);

/**
 * @brief 获取当前连接的 SSID
 */
const char *net_bridge_get_connected_ssid(void);

/**
 * @brief 注册 Web 终端命令发送回调
 */
void net_bridge_register_terminal_tx(web_terminal_tx_fn_t fn);

/**
 * @brief 向 Web 终端环形缓冲区推入最新的串口输出数据 (由串口接收/回显调用)
 */
void net_bridge_terminal_rx_push(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
