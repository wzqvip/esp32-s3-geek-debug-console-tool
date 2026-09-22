#pragma once

#include <stdint.h>
#include <stdbool.h>
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
 * @brief 网络管理器配置
 */
typedef struct {
    const char *ap_ssid;            /*!< AP 热点名称 (默认: GEEK-Debugger) */
    const char *ap_password;        /*!< AP 热点密码 (默认 NULL 为免密) */
    net_wifi_state_cb_t on_state_change;
    void *user_ctx;
} net_bridge_config_t;

/**
 * @brief 初始化网络管理器并启动 Wi-Fi 状态机
 *
 * @param config 配置参数
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t net_bridge_init(const net_bridge_config_t *config);

/**
 * @brief 从配网接口触发连接新 Wi-Fi
 *
 * @param ssid 目标 SSID
 * @param password 目标密码
 * @return esp_err_t 
 */
esp_err_t net_bridge_connect_wifi(const char *ssid, const char *password);

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

#ifdef __cplusplus
}
#endif
