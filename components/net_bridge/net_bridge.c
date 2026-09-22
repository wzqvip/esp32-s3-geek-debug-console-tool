#include "net_bridge.h"
#include "wifi_portal_html.h"

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "NET_BRIDGE";

#define MAX_STA_RETRY 5
#define DEFAULT_AP_SSID "GEEK-Debugger"

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

/* -------------------- 辅助函数 -------------------- */

static void notify_state(net_wifi_state_t state)
{
    s_state = state;
    if (s_cfg.on_state_change) {
        s_cfg.on_state_change(s_state, s_connected_ssid, s_cur_ip, s_cfg.user_ctx);
    }
}

static void parse_json_field(const char *json, const char *key, char *out_val, size_t max_len)
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

/* -------------------- NVS 持久化 -------------------- */

static esp_err_t save_wifi_to_nvs(const char *ssid, const char *pwd)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_str(handle, "ssid", ssid);
    nvs_set_str(handle, "pwd", pwd ? pwd : "");
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t load_wifi_from_nvs(char *ssid, size_t ssid_max, char *pwd, size_t pwd_max)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t s_len = ssid_max;
    err = nvs_get_str(handle, "ssid", ssid, &s_len);
    if (err == ESP_OK) {
        size_t p_len = pwd_max;
        nvs_get_str(handle, "pwd", pwd, &p_len);
    }
    nvs_close(handle);
    return err;
}

/* -------------------- Wi-Fi 事件回调 -------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP Mode Started. SSID: %s, IP: %s",
                     s_cfg.ap_ssid ? s_cfg.ap_ssid : DEFAULT_AP_SSID, s_cur_ip);
            break;

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA Mode Started, connecting to: %s", s_target_ssid);
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
                    ESP_LOGW(TAG, "Retrying connect to %s (%d/%d)...", s_target_ssid, s_retry_num, MAX_STA_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "Failed to connect to %s, fallback to AP Portal!", s_target_ssid);
                    s_retry_num = 0;
                    strcpy(s_cur_ip, "192.168.4.1");
                    s_connected_ssid[0] = '\0';

                    // 切换回 AP+STA 或纯 AP
                    esp_wifi_set_mode(WIFI_MODE_APSTA);
                    notify_state(NET_WIFI_STATE_FALLBACK_AP);

                    // 3 秒后转入稳定的 AP 配网状态
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    notify_state(NET_WIFI_STATE_AP_PORTAL);
                }
            }
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_cur_ip, sizeof(s_cur_ip));
        snprintf(s_connected_ssid, sizeof(s_connected_ssid), "%.31s", s_target_ssid);
        ESP_LOGI(TAG, "Got IP address: %s! Connected to: %s", s_cur_ip, s_connected_ssid);
        notify_state(NET_WIFI_STATE_CONNECTED);
    }
}

/* -------------------- HTTP Web 配网服务 -------------------- */

static esp_err_t http_get_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    const char *st_str = "AP";
    if (s_state == NET_WIFI_STATE_CONNECTING) st_str = "CONNECTING";
    else if (s_state == NET_WIFI_STATE_CONNECTED) st_str = "CONNECTED";
    else if (s_state == NET_WIFI_STATE_FALLBACK_AP) st_str = "FALLBACK";

    snprintf(resp, sizeof(resp), "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\"}",
             st_str, s_cur_ip, s_connected_ssid);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    esp_wifi_scan_start(&scan_config, true); // 阻塞扫描

    uint16_t num_ap = 0;
    esp_wifi_scan_get_ap_num(&num_ap);
    if (num_ap > 15) num_ap = 15;

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * num_ap);
    if (!ap_list) {
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    esp_wifi_scan_get_ap_records(&num_ap, ap_list);

    // 拼接成 JSON 数组
    char buf[1024];
    int offset = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < num_ap; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                           (offset > 1) ? "," : "",
                           (char *)ap_list[i].ssid,
                           ap_list[i].rssi);
        if (offset >= sizeof(buf) - 64) break;
    }
    snprintf(buf + offset, sizeof(buf) - offset, "]");
    free(ap_list);

    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_connect_handler(httpd_req_t *req)
{
    char content[256];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        return httpd_resp_send_500(req);
    }
    content[recv_len] = '\0';

    char ssid[33] = {0};
    char pwd[65] = {0};
    parse_json_field(content, "ssid", ssid, sizeof(ssid));
    parse_json_field(content, "password", pwd, sizeof(pwd));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received Wi-Fi connect request -> SSID: %s", ssid);

    // 保存到 NVS
    save_wifi_to_nvs(ssid, pwd);

    // 触发连接
    net_bridge_connect_wifi(ssid, pwd);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    if (s_http_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_uri_t uri_get_index = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = http_get_index_handler,
        };
        httpd_register_uri_handler(s_http_server, &uri_get_index);

        httpd_uri_t uri_get_status = {
            .uri = "/api/wifi/status",
            .method = HTTP_GET,
            .handler = http_get_status_handler,
        };
        httpd_register_uri_handler(s_http_server, &uri_get_status);

        httpd_uri_t uri_get_scan = {
            .uri = "/api/wifi/scan",
            .method = HTTP_GET,
            .handler = http_get_scan_handler,
        };
        httpd_register_uri_handler(s_http_server, &uri_get_scan);

        httpd_uri_t uri_post_connect = {
            .uri = "/api/wifi/connect",
            .method = HTTP_POST,
            .handler = http_post_connect_handler,
        };
        httpd_register_uri_handler(s_http_server, &uri_post_connect);

        ESP_LOGI(TAG, "HTTP Portal Server started on port %d", config.server_port);
    }
}

/* -------------------- 外部接口 -------------------- */

esp_err_t net_bridge_connect_wifi(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
    if (password) {
        snprintf(s_target_pwd, sizeof(s_target_pwd), "%s", password);
    } else {
        s_target_pwd[0] = '\0';
    }

    s_retry_num = 0;
    notify_state(NET_WIFI_STATE_CONNECTING);

    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%.31s", s_target_ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%.63s", s_target_pwd);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    return ESP_OK;
}

esp_err_t net_bridge_reset_to_ap(void)
{
    // 擦除 NVS 中的 wifi 配置
    nvs_handle_t handle;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

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

    // 配置 AP 参数
    const char *ap_name = s_cfg.ap_ssid ? s_cfg.ap_ssid : DEFAULT_AP_SSID;
    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%.31s", ap_name);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);

    // 默认以 APSTA 模式启动（AP 保证随时可连入配网，STA 用于连接目标路由）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 启动 HTTP 配网页面
    start_http_server();

    // 尝试从 NVS 读取历史保存的 Wi-Fi 配置
    char saved_ssid[33] = {0};
    char saved_pwd[65] = {0};
    if (load_wifi_from_nvs(saved_ssid, sizeof(saved_ssid), saved_pwd, sizeof(saved_pwd)) == ESP_OK && saved_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found saved Wi-Fi '%s', auto-connecting...", saved_ssid);
        net_bridge_connect_wifi(saved_ssid, saved_pwd);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi found. Ready for AP Portal on %s", ap_name);
        notify_state(NET_WIFI_STATE_AP_PORTAL);
    }

    return ESP_OK;
}
