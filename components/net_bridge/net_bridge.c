#include "net_bridge.h"
#include "wifi_portal_html.h"
#include "sd_logger.h"
#include "display_ui.h"
#include "usb_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "NET_BRIDGE";

#define MAX_STA_RETRY 5
#define DEFAULT_AP_SSID "GEEK-Debugger"
#define NVS_CFG_NAMESPACE "sys_cfg"
#define NVS_BLOB_KEY "config_blob"

static net_bridge_config_t s_cfg;
static net_wifi_state_t s_state = NET_WIFI_STATE_AP_PORTAL;

static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;
static httpd_handle_t s_http_server = NULL;

static int s_retry_num = 0;
static char s_cur_ip[20] = "192.168.4.1";
static char s_target_ssid[33] = {0};
static char s_target_pwd[65] = {0};
static char s_connected_ssid[33] = {0};

static sys_config_t s_sys_cfg;

// Web Terminal 环形缓冲区 (4KB)
#define TERM_BUF_SIZE 4096
static char s_term_buf[TERM_BUF_SIZE];
static uint32_t s_term_head = 0;
static uint32_t s_term_total_bytes = 0;
static SemaphoreHandle_t s_term_mutex = NULL;
static web_terminal_tx_fn_t s_term_tx_cb = NULL;

/* -------------------- 默认配置与 NVS -------------------- */

static void init_default_sys_config(sys_config_t *cfg)
{
    memset(cfg, 0, sizeof(sys_config_t));
    strcpy(cfg->sta_ssid, "");
    strcpy(cfg->sta_pwd, "");
    cfg->sta_static_ip = false;
    strcpy(cfg->sta_ip, "192.168.1.200");
    strcpy(cfg->sta_netmask, "255.255.255.0");
    strcpy(cfg->sta_gw, "192.168.1.1");
    strcpy(cfg->sta_dns, "8.8.8.8");
    cfg->sta_auto_reconnect = true;

    strcpy(cfg->ap_ssid, DEFAULT_AP_SSID);
    strcpy(cfg->ap_pwd, "");
    cfg->ap_channel = 6;
    cfg->ap_hidden = false;
    cfg->ap_max_conn = 4;

    cfg->usb_default_mode = 0; // Composite
    cfg->serial_baudrate = 115200;
    cfg->serial_databits = 8;
    cfg->serial_parity = 0;
    cfg->serial_stopbits = 1;

    cfg->log_enabled = true;
    cfg->log_flush_interval_ms = 300;
    cfg->log_max_file_kb = 0;

    cfg->display_brightness = 100;
    cfg->display_timeout_s = 0;
    cfg->display_rotation = 0;
}

esp_err_t net_bridge_save_sys_config(const sys_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, NVS_BLOB_KEY, cfg, sizeof(sys_config_t));
    if (err == ESP_OK) {
        nvs_commit(handle);
        memcpy(&s_sys_cfg, cfg, sizeof(sys_config_t));
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_sys_config(sys_config_t *cfg)
{
    init_default_sys_config(cfg);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t req_len = sizeof(sys_config_t);
    err = nvs_get_blob(handle, NVS_BLOB_KEY, cfg, &req_len);
    nvs_close(handle);
    return err;
}

esp_err_t net_bridge_factory_reset_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_CFG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    init_default_sys_config(&s_sys_cfg);
    return ESP_OK;
}

const sys_config_t *net_bridge_get_sys_config(void)
{
    return &s_sys_cfg;
}

/* -------------------- 辅助解析函数 -------------------- */

static void notify_state(net_wifi_state_t state)
{
    s_state = state;
    if (s_cfg.on_state_change) {
        s_cfg.on_state_change(s_state, s_connected_ssid, s_cur_ip, s_cfg.user_ctx);
    }
}

static void parse_json_str(const char *json, const char *key, char *out_val, size_t max_len)
{
    out_val[0] = '\0';
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    char *p = strstr(json, pattern);
    if (!p) return;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\"') {
        p++;
        size_t idx = 0;
        while (*p && *p != '\"' && idx < max_len - 1) {
            out_val[idx++] = *p++;
        }
        out_val[idx] = '\0';
    }
}

static int parse_json_int(const char *json, const char *key, int default_val)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static bool parse_json_bool(const char *json, const char *key, bool default_val)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

/* -------------------- Web Terminal 环形缓冲区实现 -------------------- */

void net_bridge_register_terminal_tx(web_terminal_tx_fn_t fn)
{
    s_term_tx_cb = fn;
}

void net_bridge_terminal_rx_push(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !s_term_mutex) return;

    if (xSemaphoreTake(s_term_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (size_t i = 0; i < len; i++) {
            s_term_buf[s_term_head] = (char)data[i];
            s_term_head = (s_term_head + 1) % TERM_BUF_SIZE;
        }
        s_term_total_bytes += len;
        xSemaphoreGive(s_term_mutex);
    }
}

/* -------------------- Wi-Fi 静态 IP 配置 -------------------- */

static void apply_sta_ip_settings(void)
{
    if (!s_netif_sta) return;

    if (s_sys_cfg.sta_static_ip) {
        esp_netif_dhcpc_stop(s_netif_sta);
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        esp_netif_str_to_ip4(s_sys_cfg.sta_ip, &ip_info.ip);
        esp_netif_str_to_ip4(s_sys_cfg.sta_gw, &ip_info.gw);
        esp_netif_str_to_ip4(s_sys_cfg.sta_netmask, &ip_info.netmask);
        esp_netif_set_ip_info(s_netif_sta, &ip_info);

        if (s_sys_cfg.sta_dns[0] != '\0') {
            esp_netif_dns_info_t dns_info;
            esp_netif_str_to_ip4(s_sys_cfg.sta_dns, (esp_ip4_addr_t *)&dns_info.ip.u_addr.ip4);
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns_info);
        }
        ESP_LOGI(TAG, "Static IP Applied: IP=%s GW=%s MASK=%s", s_sys_cfg.sta_ip, s_sys_cfg.sta_gw, s_sys_cfg.sta_netmask);
    } else {
        esp_netif_dhcpc_start(s_netif_sta);
        ESP_LOGI(TAG, "DHCP Mode Enabled for STA");
    }
}

/* -------------------- Wi-Fi 事件回调 -------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP Mode Started. SSID: %s, IP: %s",
                     s_sys_cfg.ap_ssid[0] ? s_sys_cfg.ap_ssid : DEFAULT_AP_SSID, s_cur_ip);
            break;

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA Mode Started, connecting to: %s", s_target_ssid);
            apply_sta_ip_settings();
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA Connected to AP! Waiting for IP...");
            s_retry_num = 0;
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_state == NET_WIFI_STATE_CONNECTING) {
                if (s_retry_num < MAX_STA_RETRY) {
                    s_retry_num++;
                    ESP_LOGW(TAG, "STA connect failed, retry %d/%d...", s_retry_num, MAX_STA_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "STA connect retries exhausted. Fallback to AP Portal!");
                    strcpy(s_cur_ip, "192.168.4.1");
                    s_connected_ssid[0] = '\0';
                    notify_state(NET_WIFI_STATE_FALLBACK_AP);
                }
            } else if (s_state == NET_WIFI_STATE_CONNECTED) {
                ESP_LOGW(TAG, "STA disconnected from AP!");
                if (s_sys_cfg.sta_auto_reconnect) {
                    ESP_LOGI(TAG, "Auto-reconnecting...");
                    esp_wifi_connect();
                } else {
                    strcpy(s_cur_ip, "192.168.4.1");
                    s_connected_ssid[0] = '\0';
                    notify_state(NET_WIFI_STATE_FALLBACK_AP);
                }
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_cur_ip, sizeof(s_cur_ip), IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_connected_ssid, sizeof(s_connected_ssid), "%s", s_target_ssid);
            ESP_LOGI(TAG, "STA Got IP: %s (Connected to %s)", s_cur_ip, s_connected_ssid);
            notify_state(NET_WIFI_STATE_CONNECTED);
        }
    }
}

/* -------------------- HTTP REST API 处理函数 -------------------- */

static esp_err_t http_get_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    const char *state_str = "AP_PORTAL";
    if (s_state == NET_WIFI_STATE_CONNECTED) state_str = "CONNECTED";
    else if (s_state == NET_WIFI_STATE_CONNECTING) state_str = "CONNECTING";
    else if (s_state == NET_WIFI_STATE_FALLBACK_AP) state_str = "FALLBACK";

    uint8_t mac_sta[6] = {0};
    uint8_t mac_ap[6] = {0};
    esp_read_mac(mac_sta, ESP_MAC_WIFI_STA);
    esp_read_mac(mac_ap, ESP_MAC_WIFI_SOFTAP);

    char resp[300];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\","
             "\"sta_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"ap_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             state_str, s_cur_ip, s_connected_ssid,
             mac_sta[0], mac_sta[1], mac_sta[2], mac_sta[3], mac_sta[4], mac_sta[5],
             mac_ap[0], mac_ap[1], mac_ap[2], mac_ap[3], mac_ap[4], mac_ap[5]);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_system_info_handler(httpd_req_t *req)
{
    uint8_t mac_sta[6] = {0};
    uint8_t mac_ap[6] = {0};
    esp_read_mac(mac_sta, ESP_MAC_WIFI_STA);
    esp_read_mac(mac_ap, ESP_MAC_WIFI_SOFTAP);

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    int64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    int up_h = (int)(uptime_s / 3600);
    int up_m = (int)((uptime_s % 3600) / 60);
    int up_sec = (int)(uptime_s % 60);

    char uptime_str[32];
    snprintf(uptime_str, sizeof(uptime_str), "%d小时%d分%d秒", up_h, up_m, up_sec);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"cpu\":\"ESP32-S3 (240MHz LX7)\",\"flash_size_mb\":16,"
             "\"free_heap\":%lu,\"min_free_heap\":%lu,"
             "\"uptime_s\":%lld,\"uptime_str\":\"%s\","
             "\"sta_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"ap_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             (unsigned long)free_heap, (unsigned long)min_free_heap,
             uptime_s, uptime_str,
             mac_sta[0], mac_sta[1], mac_sta[2], mac_sta[3], mac_sta[4], mac_sta[5],
             mac_ap[0], mac_ap[1], mac_ap[2], mac_ap[3], mac_ap[4], mac_ap[5]);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_config_handler(httpd_req_t *req)
{
    char resp[600];
    snprintf(resp, sizeof(resp),
             "{\"sta_ssid\":\"%s\",\"sta_static_ip\":%s,"
             "\"sta_ip\":\"%s\",\"sta_netmask\":\"%s\",\"sta_gw\":\"%s\",\"sta_dns\":\"%s\","
             "\"ap_ssid\":\"%s\",\"ap_channel\":%d,\"ap_hidden\":%s,\"ap_max_conn\":%d,"
             "\"usb_default_mode\":%d,\"serial_baudrate\":%lu,\"serial_parity\":%d,"
             "\"display_brightness\":%d,\"display_timeout_s\":%d,\"display_rotation\":%d,"
             "\"log_enabled\":%s,\"log_flush_interval_ms\":%d,\"log_max_file_kb\":%lu}",
             s_sys_cfg.sta_ssid, s_sys_cfg.sta_static_ip ? "true" : "false",
             s_sys_cfg.sta_ip, s_sys_cfg.sta_netmask, s_sys_cfg.sta_gw, s_sys_cfg.sta_dns,
             s_sys_cfg.ap_ssid, (int)s_sys_cfg.ap_channel, s_sys_cfg.ap_hidden ? "true" : "false", (int)s_sys_cfg.ap_max_conn,
             (int)s_sys_cfg.usb_default_mode, (unsigned long)s_sys_cfg.serial_baudrate, (int)s_sys_cfg.serial_parity,
             (int)s_sys_cfg.display_brightness, (int)s_sys_cfg.display_timeout_s, (int)s_sys_cfg.display_rotation,
             s_sys_cfg.log_enabled ? "true" : "false", (int)s_sys_cfg.log_flush_interval_ms, (unsigned long)s_sys_cfg.log_max_file_kb);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 16) ap_count = 16;

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records && ap_count > 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (ap_count > 0) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    for (int i = 0; i < ap_count; i++) {
        char item[128];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 (i > 0) ? "," : "",
                 (char *)ap_records[i].ssid,
                 ap_records[i].rssi,
                 ap_records[i].authmode);
        httpd_resp_send_chunk(req, item, strlen(item));
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);

    if (ap_records) free(ap_records);
    return ESP_OK;
}

static esp_err_t http_post_config_wifi_sta_handler(httpd_req_t *req)
{
    char buf[350];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[33] = {0};
    char pwd[65] = {0};
    parse_json_str(buf, "ssid", ssid, sizeof(ssid));
    parse_json_str(buf, "password", pwd, sizeof(pwd));

    bool is_static = parse_json_bool(buf, "static_ip", false);
    char ip[16] = {0};
    char mask[16] = {0};
    char gw[16] = {0};
    char dns[16] = {0};

    parse_json_str(buf, "ip", ip, sizeof(ip));
    parse_json_str(buf, "netmask", mask, sizeof(mask));
    parse_json_str(buf, "gw", gw, sizeof(gw));
    parse_json_str(buf, "dns", dns, sizeof(dns));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"error\":\"SSID empty\"}", HTTPD_RESP_USE_STRLEN);
    }

    snprintf(s_sys_cfg.sta_ssid, sizeof(s_sys_cfg.sta_ssid), "%s", ssid);
    snprintf(s_sys_cfg.sta_pwd, sizeof(s_sys_cfg.sta_pwd), "%s", pwd);
    s_sys_cfg.sta_static_ip = is_static;
    if (ip[0]) snprintf(s_sys_cfg.sta_ip, sizeof(s_sys_cfg.sta_ip), "%s", ip);
    if (mask[0]) snprintf(s_sys_cfg.sta_netmask, sizeof(s_sys_cfg.sta_netmask), "%s", mask);
    if (gw[0]) snprintf(s_sys_cfg.sta_gw, sizeof(s_sys_cfg.sta_gw), "%s", gw);
    if (dns[0]) snprintf(s_sys_cfg.sta_dns, sizeof(s_sys_cfg.sta_dns), "%s", dns);

    net_bridge_save_sys_config(&s_sys_cfg);

    net_bridge_connect_wifi_full(ssid, pwd, is_static, ip, mask, gw, dns);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_config_wifi_ap_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[33] = {0};
    char pwd[65] = {0};
    parse_json_str(buf, "ssid", ssid, sizeof(ssid));
    parse_json_str(buf, "password", pwd, sizeof(pwd));
    int channel = parse_json_int(buf, "channel", 6);
    bool hidden = parse_json_bool(buf, "hidden", false);
    int max_conn = parse_json_int(buf, "max_conn", 4);

    if (ssid[0] != '\0') {
        snprintf(s_sys_cfg.ap_ssid, sizeof(s_sys_cfg.ap_ssid), "%s", ssid);
    }
    snprintf(s_sys_cfg.ap_pwd, sizeof(s_sys_cfg.ap_pwd), "%s", pwd);
    s_sys_cfg.ap_channel = (uint8_t)channel;
    s_sys_cfg.ap_hidden = hidden;
    s_sys_cfg.ap_max_conn = (uint8_t)max_conn;

    net_bridge_save_sys_config(&s_sys_cfg);
    net_bridge_configure_ap(s_sys_cfg.ap_ssid, s_sys_cfg.ap_pwd, s_sys_cfg.ap_channel, s_sys_cfg.ap_hidden, s_sys_cfg.ap_max_conn);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_config_display_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    int bl = parse_json_int(buf, "brightness", -1);
    if (bl >= 0 && bl <= 100) {
        s_sys_cfg.display_brightness = (uint8_t)bl;
        display_ui_set_backlight((uint8_t)bl);
    }
    int timeout = parse_json_int(buf, "timeout_s", -1);
    if (timeout >= 0) s_sys_cfg.display_timeout_s = (uint16_t)timeout;
    int rot = parse_json_int(buf, "rotation", -1);
    if (rot >= 0) {
        s_sys_cfg.display_rotation = (uint8_t)rot;
        display_ui_set_rotation(rot == 2);
    }

    net_bridge_save_sys_config(&s_sys_cfg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_config_serial_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    int baud = parse_json_int(buf, "baudrate", 0);
    if (baud > 0) s_sys_cfg.serial_baudrate = (uint32_t)baud;
    int usb_mode = parse_json_int(buf, "usb_mode", -1);
    if (usb_mode >= 0) s_sys_cfg.usb_default_mode = (uint8_t)usb_mode;
    int parity = parse_json_int(buf, "parity", -1);
    if (parity >= 0) s_sys_cfg.serial_parity = (uint8_t)parity;

    net_bridge_save_sys_config(&s_sys_cfg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_config_logger_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    s_sys_cfg.log_enabled = parse_json_bool(buf, "enabled", true);
    int flush_ms = parse_json_int(buf, "flush_ms", 300);
    s_sys_cfg.log_flush_interval_ms = (uint16_t)flush_ms;
    int max_kb = parse_json_int(buf, "max_file_kb", 0);
    s_sys_cfg.log_max_file_kb = (uint32_t)max_kb;

    net_bridge_save_sys_config(&s_sys_cfg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

/* -------------------- 终端数据收发 -------------------- */

static esp_err_t http_post_terminal_tx_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char text[256] = {0};
    parse_json_str(buf, "text", text, sizeof(text));
    if (text[0] != '\0' && s_term_tx_cb) {
        s_term_tx_cb((const uint8_t *)text, strlen(text));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_terminal_rx_handler(httpd_req_t *req)
{
    char query[32] = {0};
    uint32_t client_offset = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "offset", val, sizeof(val)) == ESP_OK) {
            client_offset = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    char *send_buf = malloc(1024);
    if (!send_buf) return ESP_FAIL;

    size_t copied = 0;
    uint32_t current_total = 0;

    if (xSemaphoreTake(s_term_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_total = s_term_total_bytes;
        if (current_total > client_offset) {
            uint32_t diff = current_total - client_offset;
            if (diff > 800) diff = 800; // 单次最大返回 800 字节，防止过长
            uint32_t start_pos = (s_term_head + TERM_BUF_SIZE - (current_total - client_offset)) % TERM_BUF_SIZE;
            for (uint32_t i = 0; i < diff; i++) {
                char ch = s_term_buf[(start_pos + i) % TERM_BUF_SIZE];
                // 过滤掉不可见字符除 \n \r \t 之外
                if (ch == '\\') {
                    send_buf[copied++] = '\\'; send_buf[copied++] = '\\';
                } else if (ch == '\"') {
                    send_buf[copied++] = '\\'; send_buf[copied++] = '\"';
                } else if (ch == '\n') {
                    send_buf[copied++] = '\\'; send_buf[copied++] = 'n';
                } else if (ch == '\r') {
                    send_buf[copied++] = '\\'; send_buf[copied++] = 'r';
                } else if (ch == '\t') {
                    send_buf[copied++] = '\\'; send_buf[copied++] = 't';
                } else if (ch >= 32 && ch <= 126) {
                    send_buf[copied++] = ch;
                }
            }
        }
        xSemaphoreGive(s_term_mutex);
    }
    send_buf[copied] = '\0';

    char resp[1200];
    snprintf(resp, sizeof(resp), "{\"offset\":%lu,\"data\":\"%s\"}", (unsigned long)current_total, send_buf);
    free(send_buf);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* -------------------- 系统动作处理 -------------------- */

static void delayed_reboot_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t http_post_system_reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"msg\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void delayed_download_mode_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static esp_err_t http_post_system_download_mode_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"msg\":\"Entering ROM Download Mode...\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(delayed_download_mode_task, "dl_mode_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t http_post_system_factory_reset_handler(httpd_req_t *req)
{
    net_bridge_factory_reset_config();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"msg\":\"Factory Reset Done. Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* -------------------- TF 卡日志接口 -------------------- */

static esp_err_t http_get_logs_list_handler(httpd_req_t *req)
{
    sd_logger_status_t st;
    sd_logger_get_status(&st);

    httpd_resp_set_type(req, "application/json");

    if (!st.card_mounted) {
        return httpd_resp_send(req, "{\"mounted\":false,\"files\":[]}", HTTPD_RESP_USE_STRLEN);
    }

    char *files_json = malloc(2048);
    if (!files_json) {
        return httpd_resp_send_500(req);
    }
    sd_logger_list_files_json(files_json, 2048);

    char cur_sess_str[32] = {0};
    if (st.current_session_id > 0) {
        snprintf(cur_sess_str, sizeof(cur_sess_str), "session_%03lu.log", (unsigned long)st.current_session_id);
    }

    char *resp = malloc(2500);
    if (!resp) {
        free(files_json);
        return httpd_resp_send_500(req);
    }
    snprintf(resp, 2500,
             "{\"mounted\":true,\"total_mb\":%lu,\"free_mb\":%lu,\"cur_sess\":\"%s\",\"files\":%s}",
             (unsigned long)st.total_mb, (unsigned long)st.free_mb, cur_sess_str, files_json);

    esp_err_t ret = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(files_json);
    free(resp);
    return ret;
}

static esp_err_t http_get_logs_view_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char filename[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    if (filename[0] == '\0') {
        return httpd_resp_send_404(req);
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "/sdcard/logs/%s", filename);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        return httpd_resp_send_404(req);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char chunk[512];
    size_t read_bytes = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_get_logs_download_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char filename[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    if (filename[0] == '\0') {
        return httpd_resp_send_404(req);
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "/sdcard/logs/%s", filename);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return httpd_resp_send_404(req);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char chunk[1024];
    size_t read_bytes = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_post_logs_new_session_handler(httpd_req_t *req)
{
    esp_err_t err = sd_logger_start_new_session();
    char resp[128];
    if (err == ESP_OK) {
        sd_logger_status_t st;
        sd_logger_get_status(&st);
        snprintf(resp, sizeof(resp), "{\"success\":true,\"file\":\"session_%03lu.log\"}", (unsigned long)st.current_session_id);
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":%d}", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_logs_delete_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char filename[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }
    if (filename[0] != '\0') {
        sd_logger_delete_file(filename);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_logs_format_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Web API: Formatting TF Card (FATFS)...");
    esp_err_t err = sd_logger_format();
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"success\":true,\"message\":\"TF Card formatted successfully\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":%d}", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* -------------------- USB HID 模拟键鼠 Web 接口 -------------------- */

static esp_err_t http_post_hid_keyboard_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char action[32] = {0};
    parse_json_str(buf, "action", action, sizeof(action));

    if (strcmp(action, "type") == 0) {
        char text[384] = {0};
        parse_json_str(buf, "text", text, sizeof(text));
        if (text[0] != '\0') {
            usb_hid_type_string(text);
        }
    } else if (strcmp(action, "shortcut") == 0) {
        char key[32] = {0};
        parse_json_str(buf, "key", key, sizeof(key));
        if (strcmp(key, "enter") == 0) {
            usb_hid_send_keystroke(0, 0x28);
        } else if (strcmp(key, "esc") == 0) {
            usb_hid_send_keystroke(0, 0x29);
        } else if (strcmp(key, "backspace") == 0) {
            usb_hid_send_keystroke(0, 0x2A);
        } else if (strcmp(key, "tab") == 0) {
            usb_hid_send_keystroke(0, 0x2B);
        } else if (strcmp(key, "space") == 0) {
            usb_hid_send_keystroke(0, 0x2C);
        } else if (strcmp(key, "win") == 0 || strcmp(key, "gui") == 0) {
            usb_hid_send_keystroke(0x08, 0xE3); // GUI / Win key
        } else if (strcmp(key, "ctrl_c") == 0) {
            usb_hid_send_keystroke(0x01, 0x06); // Ctrl + C
        } else if (strcmp(key, "ctrl_v") == 0) {
            usb_hid_send_keystroke(0x01, 0x19); // Ctrl + V
        } else if (strcmp(key, "ctrl_a") == 0) {
            usb_hid_send_keystroke(0x01, 0x04); // Ctrl + A
        } else if (strcmp(key, "ctrl_z") == 0) {
            usb_hid_send_keystroke(0x01, 0x1D); // Ctrl + Z
        } else if (strcmp(key, "ctrl_alt_del") == 0) {
            usb_hid_send_keystroke(0x01 | 0x04, 0x4C); // Ctrl + Alt + Delete
        } else if (strcmp(key, "up") == 0) {
            usb_hid_send_keystroke(0, 0x52);
        } else if (strcmp(key, "down") == 0) {
            usb_hid_send_keystroke(0, 0x51);
        } else if (strcmp(key, "left") == 0) {
            usb_hid_send_keystroke(0, 0x50);
        } else if (strcmp(key, "right") == 0) {
            usb_hid_send_keystroke(0, 0x4F);
        }
    } else if (strcmp(action, "press") == 0) {
        int mod = parse_json_int(buf, "modifier", 0);
        int kc = parse_json_int(buf, "keycode", 0);
        if (kc > 0) {
            usb_hid_send_keystroke((uint8_t)mod, (uint8_t)kc);
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_hid_mouse_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char click[16] = {0};
    parse_json_str(buf, "click", click, sizeof(click));

    if (click[0] != '\0') {
        if (strcmp(click, "left") == 0) {
            usb_hid_mouse_click(0x01);
        } else if (strcmp(click, "right") == 0) {
            usb_hid_mouse_click(0x02);
        } else if (strcmp(click, "middle") == 0) {
            usb_hid_mouse_click(0x04);
        } else if (strcmp(click, "double") == 0) {
            usb_hid_mouse_click(0x01);
            vTaskDelay(pdMS_TO_TICKS(50));
            usb_hid_mouse_click(0x01);
        }
    } else {
        int dx = parse_json_int(buf, "dx", 0);
        int dy = parse_json_int(buf, "dy", 0);
        int btn = parse_json_int(buf, "buttons", 0);
        int wheel = parse_json_int(buf, "wheel", 0);
        usb_hid_send_mouse((uint8_t)btn, (int8_t)dx, (int8_t)dy, (int8_t)wheel, 0);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

/* -------------------- 启动 Web 服务器 -------------------- */

static void start_http_server(void)
{
    if (s_http_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30;
    config.stack_size = 9216;

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_uri_t uri_get_index = { .uri = "/", .method = HTTP_GET, .handler = http_get_index_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_index);

        httpd_uri_t uri_get_status = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = http_get_status_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_status);

        httpd_uri_t uri_get_sys_info = { .uri = "/api/system/info", .method = HTTP_GET, .handler = http_get_system_info_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_sys_info);

        httpd_uri_t uri_get_config = { .uri = "/api/config", .method = HTTP_GET, .handler = http_get_config_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_config);

        httpd_uri_t uri_get_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = http_get_scan_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_scan);

        httpd_uri_t uri_post_cfg_wifi_sta = { .uri = "/api/config/wifi_sta", .method = HTTP_POST, .handler = http_post_config_wifi_sta_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_cfg_wifi_sta);

        httpd_uri_t uri_post_cfg_wifi_ap = { .uri = "/api/config/wifi_ap", .method = HTTP_POST, .handler = http_post_config_wifi_ap_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_cfg_wifi_ap);

        httpd_uri_t uri_post_cfg_display = { .uri = "/api/config/display", .method = HTTP_POST, .handler = http_post_config_display_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_cfg_display);

        httpd_uri_t uri_post_cfg_serial = { .uri = "/api/config/serial", .method = HTTP_POST, .handler = http_post_config_serial_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_cfg_serial);

        httpd_uri_t uri_post_cfg_logger = { .uri = "/api/config/logger", .method = HTTP_POST, .handler = http_post_config_logger_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_cfg_logger);

        httpd_uri_t uri_post_term_tx = { .uri = "/api/terminal/tx", .method = HTTP_POST, .handler = http_post_terminal_tx_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_term_tx);

        httpd_uri_t uri_get_term_rx = { .uri = "/api/terminal/rx", .method = HTTP_GET, .handler = http_get_terminal_rx_handler };
        httpd_register_uri_handler(s_http_server, &uri_get_term_rx);

        httpd_uri_t uri_post_sys_reboot = { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = http_post_system_reboot_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_sys_reboot);

        httpd_uri_t uri_post_sys_dl = { .uri = "/api/system/download_mode", .method = HTTP_POST, .handler = http_post_system_download_mode_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_sys_dl);

        httpd_uri_t uri_post_sys_rst = { .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = http_post_system_factory_reset_handler };
        httpd_register_uri_handler(s_http_server, &uri_post_sys_rst);

        // TF 卡日志接口
        httpd_uri_t uri_logs_list = { .uri = "/api/logs/list", .method = HTTP_GET, .handler = http_get_logs_list_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_list);

        httpd_uri_t uri_logs_view = { .uri = "/api/logs/view", .method = HTTP_GET, .handler = http_get_logs_view_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_view);

        httpd_uri_t uri_logs_download = { .uri = "/api/logs/download", .method = HTTP_GET, .handler = http_get_logs_download_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_download);

        httpd_uri_t uri_logs_new = { .uri = "/api/logs/new_session", .method = HTTP_POST, .handler = http_post_logs_new_session_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_new);

        httpd_uri_t uri_logs_del = { .uri = "/api/logs/delete", .method = HTTP_POST, .handler = http_post_logs_delete_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_del);

        httpd_uri_t uri_logs_format = { .uri = "/api/logs/format", .method = HTTP_POST, .handler = http_post_logs_format_handler };
        httpd_register_uri_handler(s_http_server, &uri_logs_format);

        // USB HID 键鼠控制接口
        httpd_uri_t uri_hid_kb = { .uri = "/api/hid/keyboard", .method = HTTP_POST, .handler = http_post_hid_keyboard_handler };
        httpd_register_uri_handler(s_http_server, &uri_hid_kb);

        httpd_uri_t uri_hid_mouse = { .uri = "/api/hid/mouse", .method = HTTP_POST, .handler = http_post_hid_mouse_handler };
        httpd_register_uri_handler(s_http_server, &uri_hid_mouse);

        ESP_LOGI(TAG, "Full-Featured Web Console Server started on port %d", config.server_port);
    }
}

/* -------------------- 外部公共接口 -------------------- */

esp_err_t net_bridge_configure_ap(const char *ssid, const char *password,
                                  uint8_t channel, bool hidden, uint8_t max_conn)
{
    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%.31s", ssid ? ssid : DEFAULT_AP_SSID);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    if (password && strlen(password) >= 8) {
        snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%.63s", password);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_config.ap.channel = (channel >= 1 && channel <= 13) ? channel : 6;
    ap_config.ap.ssid_hidden = hidden ? 1 : 0;
    ap_config.ap.max_connection = (max_conn >= 1 && max_conn <= 4) ? max_conn : 4;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    ESP_LOGI(TAG, "AP Reconfigured: SSID=%s, Channel=%d, Hidden=%d",
             ap_config.ap.ssid, ap_config.ap.channel, ap_config.ap.ssid_hidden);
    return err;
}

esp_err_t net_bridge_connect_wifi_full(const char *ssid, const char *password,
                                       bool static_ip, const char *ip,
                                       const char *netmask, const char *gw, const char *dns)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
    if (password) {
        snprintf(s_target_pwd, sizeof(s_target_pwd), "%s", password);
    } else {
        s_target_pwd[0] = '\0';
    }

    s_sys_cfg.sta_static_ip = static_ip;
    if (ip && ip[0]) snprintf(s_sys_cfg.sta_ip, sizeof(s_sys_cfg.sta_ip), "%s", ip);
    if (netmask && netmask[0]) snprintf(s_sys_cfg.sta_netmask, sizeof(s_sys_cfg.sta_netmask), "%s", netmask);
    if (gw && gw[0]) snprintf(s_sys_cfg.sta_gw, sizeof(s_sys_cfg.sta_gw), "%s", gw);
    if (dns && dns[0]) snprintf(s_sys_cfg.sta_dns, sizeof(s_sys_cfg.sta_dns), "%s", dns);

    s_retry_num = 0;
    notify_state(NET_WIFI_STATE_CONNECTING);

    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%.31s", s_target_ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%.63s", s_target_pwd);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    apply_sta_ip_settings();
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    return ESP_OK;
}

esp_err_t net_bridge_connect_wifi(const char *ssid, const char *password)
{
    return net_bridge_connect_wifi_full(ssid, password, false, NULL, NULL, NULL, NULL);
}

esp_err_t net_bridge_reset_to_ap(void)
{
    s_sys_cfg.sta_ssid[0] = '\0';
    s_sys_cfg.sta_pwd[0] = '\0';
    net_bridge_save_sys_config(&s_sys_cfg);

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    strcpy(s_cur_ip, "192.168.4.1");
    s_connected_ssid[0] = '\0';
    notify_state(NET_WIFI_STATE_AP_PORTAL);
    return ESP_OK;
}

net_wifi_state_t net_bridge_get_state(void)
{
    return s_state;
}

const char *net_bridge_get_ip_str(void)
{
    return s_cur_ip;
}

const char *net_bridge_get_connected_ssid(void)
{
    return s_connected_ssid;
}

esp_err_t net_bridge_init(const net_bridge_config_t *config)
{
    if (config) {
        memcpy(&s_cfg, config, sizeof(net_bridge_config_t));
    }

    s_term_mutex = xSemaphoreCreateMutex();

    // 读取 NVS 持久化系统配置
    load_sys_config(&s_sys_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建 AP 和 STA 网络接口
    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 默认以 APSTA 模式启动
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 配置 AP 参数
    net_bridge_configure_ap(s_sys_cfg.ap_ssid, s_sys_cfg.ap_pwd,
                            s_sys_cfg.ap_channel, s_sys_cfg.ap_hidden, s_sys_cfg.ap_max_conn);

    ESP_ERROR_CHECK(esp_wifi_start());

    // 启动 HTTP 控制中心
    start_http_server();

    // 自动连接已保存的 STA Wi-Fi
    if (s_sys_cfg.sta_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found saved Wi-Fi '%s', auto-connecting...", s_sys_cfg.sta_ssid);
        net_bridge_connect_wifi_full(s_sys_cfg.sta_ssid, s_sys_cfg.sta_pwd,
                                     s_sys_cfg.sta_static_ip, s_sys_cfg.sta_ip,
                                     s_sys_cfg.sta_netmask, s_sys_cfg.sta_gw, s_sys_cfg.sta_dns);
    } else {
        ESP_LOGI(TAG, "Ready for AP Portal on %s", s_sys_cfg.ap_ssid);
        notify_state(NET_WIFI_STATE_AP_PORTAL);
    }

    return ESP_OK;
}
