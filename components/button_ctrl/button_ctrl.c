#include "button_ctrl.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "BUTTON_CTRL";

typedef enum {
    SM_IDLE,
    SM_DEBOUNCE_PRESS,
    SM_PRESSED,
    SM_LONG_PRESS_CONFIRMED,
    SM_DEBOUNCE_RELEASE,
} btn_state_t;

static button_config_t s_config;
static TaskHandle_t s_task_handle = NULL;
static bool s_running = false;

static inline bool read_raw_pin(void)
{
    int level = gpio_get_level(s_config.gpio_num);
    return s_config.active_low ? (level == 0) : (level == 1);
}

static inline void emit_event(button_event_t evt, uint32_t duration_ms, uint8_t progress)
{
    if (s_config.callback) {
        button_event_data_t data = {
            .event = evt,
            .press_duration_ms = duration_ms,
            .progress_percent = progress,
        };
        s_config.callback(&data, s_config.user_ctx);
    }
}

static void button_task(void *pvParameters)
{
    const TickType_t poll_period = pdMS_TO_TICKS(10); // 10ms 采样周期
    btn_state_t state = SM_IDLE;
    uint32_t press_start_tick = 0;
    uint32_t state_entry_tick = 0;
    bool charging_started = false;

    ESP_LOGI(TAG, "Button monitoring task started on GPIO %d", s_config.gpio_num);

    while (s_running) {
        bool is_pressed = read_raw_pin();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        switch (state) {
        case SM_IDLE:
            if (is_pressed) {
                state_entry_tick = now_ms;
                state = SM_DEBOUNCE_PRESS;
            }
            break;

        case SM_DEBOUNCE_PRESS:
            if (is_pressed) {
                if (now_ms - state_entry_tick >= s_config.debounce_ms) {
                    // 消抖通过，正式进入按下状态
                    press_start_tick = now_ms;
                    charging_started = false;
                    state = SM_PRESSED;
                    emit_event(BUTTON_EVENT_DOWN, 0, 0);
                }
            } else {
                state = SM_IDLE; // 毛刺抖动，恢复空闲
            }
            break;

        case SM_PRESSED:
            if (is_pressed) {
                uint32_t duration = now_ms - press_start_tick;
                // 判断是否进入长按蓄力期 (介于 short_press_max_ms 和 long_press_threshold_ms 之间)
                if (duration >= s_config.short_press_max_ms && duration < s_config.long_press_threshold_ms) {
                    charging_started = true;
                    uint32_t charge_span = s_config.long_press_threshold_ms - s_config.short_press_max_ms;
                    uint32_t charge_cur = duration - s_config.short_press_max_ms;
                    uint8_t progress = (uint8_t)((charge_cur * 100) / charge_span);
                    emit_event(BUTTON_EVENT_LONG_PRESS_TICK, duration, progress);
                } else if (duration >= s_config.long_press_threshold_ms) {
                    // 长按蓄力满，立即触发长按确认！
                    emit_event(BUTTON_EVENT_LONG_PRESS_CONFIRM, duration, 100);
                    state = SM_LONG_PRESS_CONFIRMED;
                }
            } else {
                // 在释放时做判定
                uint32_t duration = now_ms - press_start_tick;
                if (duration < s_config.short_press_max_ms) {
                    // 满足短按窗口，触发短按事件
                    emit_event(BUTTON_EVENT_SHORT_PRESS, duration, 0);
                } else if (charging_started) {
                    // 蓄力途中松开，发送取消事件
                    emit_event(BUTTON_EVENT_LONG_PRESS_CANCEL, duration, 0);
                }
                emit_event(BUTTON_EVENT_UP, duration, 0);
                state_entry_tick = now_ms;
                state = SM_DEBOUNCE_RELEASE;
            }
            break;

        case SM_LONG_PRESS_CONFIRMED:
            if (!is_pressed) {
                // 长按确认之后用户松手
                uint32_t duration = now_ms - press_start_tick;
                emit_event(BUTTON_EVENT_UP, duration, 100);
                state_entry_tick = now_ms;
                state = SM_DEBOUNCE_RELEASE;
            }
            break;

        case SM_DEBOUNCE_RELEASE:
            if (!is_pressed) {
                if (now_ms - state_entry_tick >= s_config.debounce_ms) {
                    state = SM_IDLE;
                }
            } else {
                // 释放过程中再次出现抖动
                state_entry_tick = now_ms;
            }
            break;
        }

        vTaskDelay(poll_period);
    }

    vTaskDelete(NULL);
}

esp_err_t button_ctrl_init(const button_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(button_config_t));

    if (s_config.debounce_ms == 0) s_config.debounce_ms = 20;
    if (s_config.short_press_max_ms == 0) s_config.short_press_max_ms = 600;
    if (s_config.long_press_threshold_ms == 0) s_config.long_press_threshold_ms = 1500;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_config.gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_config.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_config.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d", s_config.gpio_num);
        return err;
    }

    s_running = true;
    BaseType_t ret = xTaskCreate(button_task, "btn_task", 2048, NULL, 10, &s_task_handle);
    if (ret != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t button_ctrl_deinit(void)
{
    s_running = false;
    if (s_task_handle) {
        // 等待任务退出
        vTaskDelay(pdMS_TO_TICKS(50));
        s_task_handle = NULL;
    }
    return ESP_OK;
}
