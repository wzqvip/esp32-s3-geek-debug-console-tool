#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOGGER_MOUNT_POINT   "/sdcard"
#define SD_LOGGER_LOGS_DIR      "/sdcard/logs"

/**
 * @brief TF 卡日志器运行时状态
 */
typedef struct {
    bool card_mounted;              /*!< TF 卡是否已挂载 */
    uint32_t total_mb;              /*!< 总容量 (MB) */
    uint32_t free_mb;               /*!< 剩余可用空间 (MB) */
    uint32_t current_session_id;    /*!< 当前会话编号，如 1, 2... */
    char current_filename[32];      /*!< 当前日志文件名，如 "session_001.log" */
    uint32_t current_file_bytes;    /*!< 当前会话文件已写入字节数 */
    uint32_t total_sessions_count;  /*!< 当前存储卡上总历史会话数 */
} sd_logger_status_t;

/**
 * @brief 初始化配置
 */
typedef struct {
    int pin_clk;                    /*!< CLK 引脚 (ESP32-S3-GEEK: GPIO 36) */
    int pin_cmd;                    /*!< CMD 引脚 (ESP32-S3-GEEK: GPIO 35) */
    int pin_d0;                     /*!< D0 引脚 (ESP32-S3-GEEK: GPIO 37) */
    int pin_d1;                     /*!< D1 引脚 (ESP32-S3-GEEK: GPIO 33) */
    int pin_d2;                     /*!< D2 引脚 (ESP32-S3-GEEK: GPIO 38) */
    int pin_d3;                     /*!< D3 引脚 (ESP32-S3-GEEK: GPIO 34) */
    bool auto_new_session;          /*!< 开机是否自动开启新会话 */
} sd_logger_config_t;

/**
 * @brief 初始化 MicroSD / TF 卡控制器并挂载 FATFS
 *
 * @param config 配置参数，传 NULL 则使用默认 GEEK 引脚
 * @return esp_err_t ESP_OK 成功挂载，ESP_ERR_NOT_FOUND 卡未插入或识别失败 (允许热插拔/优雅容错)
 */
esp_err_t sd_logger_init(const sd_logger_config_t *config);

/**
 * @brief 开启一个全新的调试会话，生成新文件 session_XXX.log
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t sd_logger_start_new_session(void);

/**
 * @brief 记录由 Target 目标机发出的数据（RX: Linux 控制台、内核崩溃日志、Shell 输出）
 *
 * @param data 字节缓冲
 * @param len 长度
 */
void sd_logger_log_rx(const uint8_t *data, size_t len);

/**
 * @brief 记录由 Host 主机发往 Target 的数据（TX: 用户敲入的 Shell 调试命令）
 *
 * @param data 字节缓冲
 * @param len 长度
 */
void sd_logger_log_tx(const uint8_t *data, size_t len);

/**
 * @brief 手动刷盘同步当前日志文件缓冲
 */
void sd_logger_flush(void);

/**
 * @brief 安全弹出 / 卸载 TF 卡（刷入所有缓冲，允许安全拔出）
 */
esp_err_t sd_logger_eject(void);

/**
 * @brief 获取当前 TF 卡及日志记录状态
 *
 * @param out_status 输出状态结构体
 */
void sd_logger_get_status(sd_logger_status_t *out_status);

/**
 * @brief 获取 TF 卡上所有历史日志文件列表 (输出为 JSON 字符串，供 Web API 直接调用)
 *
 * @param json_buf 输出缓冲区
 * @param max_len 缓冲区最大长度
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t sd_logger_list_files_json(char *json_buf, size_t max_len);

/**
 * @brief 删除指定会话日志文件
 *
 * @param filename 文件名（如 "session_001.log"）
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t sd_logger_delete_file(const char *filename);

/**
 * @brief 格式化 TF 卡为全新 FATFS 文件系统并重建日志目录与新会话
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t sd_logger_format(void);

#ifdef __cplusplus
}
#endif
