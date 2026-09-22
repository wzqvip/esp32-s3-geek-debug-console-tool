#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键事件类型枚举
 */
typedef enum {
    BUTTON_EVENT_DOWN,                 /*!< 按键按下 (消抖后) */
    BUTTON_EVENT_SHORT_PRESS,          /*!< 短按触发 (50ms ~ 600ms 抬起时产生，用于菜单项切换) */
    BUTTON_EVENT_LONG_PRESS_TICK,      /*!< 长按蓄力中 (600ms ~ 1500ms 期间周期性产生，携带进度值) */
    BUTTON_EVENT_LONG_PRESS_CANCEL,    /*!< 蓄力未满提前抬起，取消长按 */
    BUTTON_EVENT_LONG_PRESS_CONFIRM,   /*!< 长按确认 (按住达到 1500ms 立即产生，用于确认选定项) */
    BUTTON_EVENT_UP,                   /*!< 按键完全松开 */
} button_event_t;

/**
 * @brief 按键事件回调数据载荷
 */
typedef struct {
    button_event_t event;
    uint32_t press_duration_ms;        /*!< 当前按下的持续毫秒数 */
    uint8_t progress_percent;          /*!< 长按蓄力进度 (0 ~ 100) */
} button_event_data_t;

/**
 * @brief 按键事件回调函数原型
 */
typedef void (*button_event_cb_t)(const button_event_data_t *event_data, void *user_ctx);

/**
 * @brief 按键控制器初始化配置
 */
typedef struct {
    int gpio_num;                      /*!< GPIO 引脚号 (ESP32-S3-GEEK BOOT 按键为 GPIO 0) */
    bool active_low;                   /*!< 是否低电平有效 (板载按键通常为 true) */
    uint32_t debounce_ms;              /*!< 消抖时间，推荐 20~30ms */
    uint32_t short_press_max_ms;       /*!< 短按最大判定时间，推荐 600ms */
    uint32_t long_press_threshold_ms;  /*!< 长按确认判定阈值，推荐 1500ms */
    button_event_cb_t callback;        /*!< 事件回调 */
    void *user_ctx;                    /*!< 用户上下文指针 */
} button_config_t;

/**
 * @brief 初始化按键控制器并启动后台检测任务
 *
 * @param config 按键配置结构体
 * @return esp_err_t ESP_OK 成功，其它为错误码
 */
esp_err_t button_ctrl_init(const button_config_t *config);

/**
 * @brief 注销按键控制器
 *
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t button_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif
