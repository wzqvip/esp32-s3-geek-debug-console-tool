#include "sd_logger.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_LOGGER";

#define PIN_DEFAULT_CLK 36
#define PIN_DEFAULT_CMD 35
#define PIN_DEFAULT_D0  37
#define PIN_DEFAULT_D1  33
#define PIN_DEFAULT_D2  38
#define PIN_DEFAULT_D3  34

#define LOG_QUEUE_ITEM_MAX_LEN 128
#define LOG_QUEUE_DEPTH        64

typedef struct {
    bool is_tx; // true: Host->Target (TX), false: Target->Host (RX)
    uint16_t len;
    char data[LOG_QUEUE_ITEM_MAX_LEN];
} log_msg_t;

static sd_logger_status_t s_status = {0};
static sdmmc_card_t *s_card = NULL;
static FILE *s_current_file = NULL;
static QueueHandle_t s_log_queue = NULL;
static SemaphoreHandle_t s_file_mutex = NULL;
static bool s_task_running = false;
static uint32_t s_unflushed_bytes = 0;

static void update_storage_capacity(void)
{
    if (!s_status.card_mounted) return;

    struct statvfs vfs;
    if (statvfs(SD_LOGGER_MOUNT_POINT, &vfs) == 0) {
        uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t free_b = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        s_status.total_mb = (uint32_t)(total / (1024 * 1024));
        s_status.free_mb = (uint32_t)(free_b / (1024 * 1024));
    }
}

static uint32_t scan_max_session_id(void)
{
    uint32_t max_id = 0;
    uint32_t count = 0;

    DIR *dir = opendir(SD_LOGGER_LOGS_DIR);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            uint32_t id = 0;
            if (sscanf(entry->d_name, "session_%lu.log", (unsigned long *)&id) == 1) {
                count++;
                if (id > max_id) {
                    max_id = id;
                }
            }
        }
    }
    closedir(dir);
    s_status.total_sessions_count = count;
    return max_id;
}

static void sd_logger_task(void *pvParameters)
{
    ESP_LOGI(TAG, "SD Logger worker task started");
    s_task_running = true;
    log_msg_t msg;

    while (s_task_running) {
        // 阻塞等待日志消息队列，超时 300ms 自动刷盘
        if (xQueueReceive(s_log_queue, &msg, pdMS_TO_TICKS(300)) == pdTRUE) {
            if (s_status.card_mounted && s_current_file) {
                if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    // 写入方向标识或直接写入流
                    size_t written = fwrite(msg.data, 1, msg.len, s_current_file);
                    s_unflushed_bytes += written;
                    s_status.current_file_bytes += written;

                    if (s_unflushed_bytes >= 512) {
                        fflush(s_current_file);
                        s_unflushed_bytes = 0;
                    }
                    xSemaphoreGive(s_file_mutex);
                }
            }
        } else {
            // 超时未接收到新数据，若有残留缓冲则立即同步
            if (s_status.card_mounted && s_current_file && s_unflushed_bytes > 0) {
                if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    fflush(s_current_file);
                    s_unflushed_bytes = 0;
                    xSemaphoreGive(s_file_mutex);
                }
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t sd_logger_init(const sd_logger_config_t *config)
{
    ESP_LOGI(TAG, "Initializing MicroSD / TF Card subsystem...");

    s_file_mutex = xSemaphoreCreateMutex();
    if (!s_file_mutex) return ESP_ERR_NO_MEM;

    s_log_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_msg_t));
    if (!s_log_queue) return ESP_ERR_NO_MEM;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz 保证极致兼容性与稳定性

    int clk = config ? config->pin_clk : PIN_DEFAULT_CLK;
    int cmd = config ? config->pin_cmd : PIN_DEFAULT_CMD;
    int d0  = config ? config->pin_d0  : PIN_DEFAULT_D0;
    int d1  = config ? config->pin_d1  : PIN_DEFAULT_D1;
    int d2  = config ? config->pin_d2  : PIN_DEFAULT_D2;
    int d3  = config ? config->pin_d3  : PIN_DEFAULT_D3;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = clk;
    slot_config.cmd = cmd;
    slot_config.d0 = d0;
    slot_config.d1 = d1;
    slot_config.d2 = d2;
    slot_config.d3 = d3;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Probing SDMMC 4-bit bus (CLK:%d, CMD:%d, D0:%d, D1:%d, D2:%d, D3:%d)...",
             clk, cmd, d0, d1, d2, d3);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_LOGGER_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        // 4 线模式若失败，尝试 1 线模式降级兼容
        ESP_LOGW(TAG, "4-bit bus mount failed (%s), retrying 1-bit mode...", esp_err_to_name(ret));
        slot_config.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_LOGGER_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    }

    if (ret == ESP_OK) {
        s_status.card_mounted = true;
        sdmmc_card_print_info(stdout, s_card);
        update_storage_capacity();

        ESP_LOGI(TAG, "MicroSD Card Mounted! Capacity: %lu MB Total, %lu MB Free",
                 (unsigned long)s_status.total_mb, (unsigned long)s_status.free_mb);

        // 创建日志目录
        struct stat st;
        if (stat(SD_LOGGER_LOGS_DIR, &st) != 0) {
            mkdir(SD_LOGGER_LOGS_DIR, 0775);
        }

        // 启动后台写入任务 (优先级 3, 分配 4KB 栈)
        xTaskCreate(sd_logger_task, "sd_log_task", 4096, NULL, 3, NULL);

        if (!config || config->auto_new_session) {
            sd_logger_start_new_session();
        }
    } else {
        s_status.card_mounted = false;
        ESP_LOGW(TAG, "No MicroSD Card detected or mount failed (%s). Continuing in cardless mode.",
                 esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t sd_logger_start_new_session(void)
{
    if (!s_status.card_mounted) {
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // 关闭前一个会话文件
    if (s_current_file) {
        fflush(s_current_file);
        fclose(s_current_file);
        s_current_file = NULL;
    }

    uint32_t next_id = scan_max_session_id() + 1;
    s_status.current_session_id = next_id;
    snprintf(s_status.current_filename, sizeof(s_status.current_filename), "session_%03lu.log", (unsigned long)next_id);

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_LOGGER_LOGS_DIR, s_status.current_filename);

    s_current_file = fopen(full_path, "w");
    if (!s_current_file) {
        ESP_LOGE(TAG, "Failed to create session log file: %s", full_path);
        xSemaphoreGive(s_file_mutex);
        return ESP_FAIL;
    }

    // 写入标准会话头元数据
    int64_t uptime_ms = esp_timer_get_time() / 1000LL;
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "======================================================================\n"
        "ESP32-S3-GEEK Wireless Linux Debugger - Session Log\n"
        "Session ID   : %03lu\n"
        "File Name    : %s\n"
        "Start Uptime : %lld ms\n"
        "Baud Rate    : 115200 (CDC-ACM)\n"
        "Description  : CLI Command & Output Stream Capture\n"
        "======================================================================\n\n",
        (unsigned long)next_id, s_status.current_filename, uptime_ms);

    fwrite(header, 1, hlen, s_current_file);
    fflush(s_current_file);
    s_status.current_file_bytes = hlen;
    s_unflushed_bytes = 0;
    s_status.total_sessions_count++;

    update_storage_capacity();
    xSemaphoreGive(s_file_mutex);

    ESP_LOGI(TAG, "New Session started: %s", full_path);
    return ESP_OK;
}

static void enqueue_log_data(bool is_tx, const uint8_t *data, size_t len)
{
    if (!s_status.card_mounted || !s_log_queue || !data || len == 0) return;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > LOG_QUEUE_ITEM_MAX_LEN) {
            chunk = LOG_QUEUE_ITEM_MAX_LEN;
        }

        log_msg_t msg;
        msg.is_tx = is_tx;
        msg.len = (uint16_t)chunk;
        memcpy(msg.data, data + offset, chunk);

        // 非阻塞入队，队列满时丢弃以保护高优先级通信
        if (xQueueSend(s_log_queue, &msg, 0) != pdTRUE) {
            ESP_LOGD(TAG, "Log queue full, dropped %d bytes", (int)chunk);
        }

        offset += chunk;
    }
}

void sd_logger_log_rx(const uint8_t *data, size_t len)
{
    enqueue_log_data(false, data, len);
}

void sd_logger_log_tx(const uint8_t *data, size_t len)
{
    enqueue_log_data(true, data, len);
}

void sd_logger_flush(void)
{
    if (s_status.card_mounted && s_current_file && s_file_mutex) {
        if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            fflush(s_current_file);
            s_unflushed_bytes = 0;
            update_storage_capacity();
            xSemaphoreGive(s_file_mutex);
        }
    }
}

esp_err_t sd_logger_eject(void)
{
    if (!s_status.card_mounted) return ESP_OK;

    sd_logger_flush();

    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_current_file) {
            fclose(s_current_file);
            s_current_file = NULL;
        }
        xSemaphoreGive(s_file_mutex);
    }

    esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
    s_card = NULL;
    s_status.card_mounted = false;
    s_status.current_filename[0] = '\0';
    s_status.current_session_id = 0;

    ESP_LOGW(TAG, "TF Card safely unmounted. Ready for physical removal.");
    return ESP_OK;
}

void sd_logger_get_status(sd_logger_status_t *out_status)
{
    if (out_status) {
        memcpy(out_status, &s_status, sizeof(sd_logger_status_t));
    }
}

esp_err_t sd_logger_list_files_json(char *json_buf, size_t max_len)
{
    if (!json_buf || max_len < 32) return ESP_ERR_INVALID_ARG;

    if (!s_status.card_mounted) {
        snprintf(json_buf, max_len, "{\"mounted\":false,\"files\":[]}");
        return ESP_OK;
    }

    DIR *dir = opendir(SD_LOGGER_LOGS_DIR);
    if (!dir) {
        snprintf(json_buf, max_len, "{\"mounted\":true,\"total_mb\":%lu,\"free_mb\":%lu,\"files\":[]}",
                 (unsigned long)s_status.total_mb, (unsigned long)s_status.free_mb);
        return ESP_OK;
    }

    update_storage_capacity();

    size_t written = snprintf(json_buf, max_len,
        "{\"mounted\":true,\"total_mb\":%lu,\"free_mb\":%lu,\"cur_sess\":\"%s\",\"files\":[",
        (unsigned long)s_status.total_mb, (unsigned long)s_status.free_mb, s_status.current_filename);

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            uint32_t id = 0;
            if (sscanf(entry->d_name, "session_%lu.log", (unsigned long *)&id) == 1) {
                char filepath[300];
                snprintf(filepath, sizeof(filepath), "%s/%s", SD_LOGGER_LOGS_DIR, entry->d_name);
                struct stat st;
                size_t fsize = 0;
                if (stat(filepath, &st) == 0) {
                    fsize = st.st_size;
                }

                char item[128];
                int item_len = snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%lu,\"id\":%lu}",
                                       first ? "" : ",", entry->d_name, (unsigned long)fsize, (unsigned long)id);
                if (written + item_len + 4 < max_len) {
                    strcpy(json_buf + written, item);
                    written += item_len;
                    first = false;
                } else {
                    break;
                }
            }
        }
    }
    closedir(dir);

    if (written + 3 < max_len) {
        strcpy(json_buf + written, "]}");
    }

    return ESP_OK;
}

esp_err_t sd_logger_delete_file(const char *filename)
{
    if (!s_status.card_mounted || !filename || filename[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // 防止目录穿越
    if (strstr(filename, "/") || strstr(filename, "\\") || strstr(filename, "..")) {
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "%s/%s", SD_LOGGER_LOGS_DIR, filename);

    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted log file: %s", filepath);
        scan_max_session_id();
        update_storage_capacity();
        return ESP_OK;
    }
    return ESP_FAIL;
}
