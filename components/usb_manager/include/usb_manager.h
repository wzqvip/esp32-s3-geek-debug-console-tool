#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB 工作模式枚举
 */
typedef enum {
    USB_MODE_COMPOSITE = 0,     /*!< 复合模式: CDC-ACM 串口 + CDC-NCM 虚拟网卡 (推荐) */
    USB_MODE_PURE_SERIAL,       /*!< 纯串口模式: 仅 CDC-ACM 控制台 */
    USB_MODE_PURE_NET,          /*!< 纯网卡模式: 仅 CDC-NCM 虚拟以太网 */
} usb_mode_t;

/**
 * @brief 串口数据接收回调
 */
typedef void (*usb_serial_rx_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

/**
 * @brief 虚拟网卡数据帧接收回调 (以太网 MAC 帧)
 */
typedef void (*usb_net_rx_cb_t)(const void *buffer, uint16_t len, void *user_ctx);

/**
 * @brief USB 管理器配置结构体
 */
typedef struct {
    usb_mode_t initial_mode;
    usb_serial_rx_cb_t on_serial_rx;
    void *serial_user_ctx;
    usb_net_rx_cb_t on_net_rx;
    void *net_user_ctx;
} usb_manager_config_t;

/**
 * @brief 初始化 USB 管理器并安装 TinyUSB 驱动
 *
 * @param config 配置参数
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_manager_init(const usb_manager_config_t *config);

/**
 * @brief 运行时动态切换 USB 模式 (执行 tud_disconnect -> 重配 -> tud_connect 重枚举)
 *
 * @param new_mode 目标模式
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_manager_switch_mode(usb_mode_t new_mode);

/**
 * @brief 向 CDC-ACM 虚拟串口发送数据
 *
 * @param data 数据缓冲区指针
 * @param len 发送字节数
 * @return size_t 实际写入字节数
 */
size_t usb_serial_write(const uint8_t *data, size_t len);

/**
 * @brief 向 CDC-NCM 虚拟网卡发送以太网帧
 *
 * @param buffer 数据帧缓冲
 * @param len 数据帧长度
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_net_send(const void *buffer, uint16_t len);

/**
 * @brief 设置虚拟网卡链路连接状态 (Link Up / Link Down)
 *
 * @param link_up true 为 Link Up，false 为 Link Down
 */
void usb_net_set_link_state(bool link_up);

/**
 * @brief 查询当前实际工作的 USB 模式
 */
usb_mode_t usb_manager_get_current_mode(void);

/**
 * @brief 断开 USB 连接 (调用 tud_disconnect，用于安全重启或进入下载模式)
 */
void usb_manager_disconnect(void);

/* -------------------- USB HID 模拟键鼠接口 -------------------- */

/**
 * @brief 查询 USB HID 是否已连接就绪
 */
bool usb_hid_is_ready(void);

/**
 * @brief 发送完整键盘状态报告
 *
 * @param modifier 修饰键掩码 (KEYBOARD_MODIFIER_*)
 * @param keycodes 最多 6 个同时按下的按键 HID 键码 (0 表示无按键)
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_hid_send_keyboard(uint8_t modifier, const uint8_t keycodes[6]);

/**
 * @brief 发送单个按键打击 (自动按下并延迟释放)
 *
 * @param modifier 修饰键掩码
 * @param keycode 目标按键 HID 键码
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_hid_send_keystroke(uint8_t modifier, uint8_t keycode);

/**
 * @brief 模拟打字输入 ASCII 字符串 (自动转换为按键与 Shift 组合)
 *
 * @param str 要打字的字符串
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_hid_type_string(const char *str);

/**
 * @brief 发送鼠标相对移动、滚轮与按键状态
 *
 * @param buttons 鼠标按键掩码 (BIT0: 左键, BIT1: 右键, BIT2: 中键)
 * @param dx X 轴相对位移 (-127 ~ 127)
 * @param dy Y 轴相对位移 (-127 ~ 127)
 * @param wheel 垂直滚轮位移 (-127 ~ 127)
 * @param pan 水平滚轮位移 (-127 ~ 127)
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_hid_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan);

/**
 * @brief 触发一次鼠标按键点击 (按下并释放)
 *
 * @param button 鼠标按键掩码
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t usb_hid_mouse_click(uint8_t button);

#ifdef __cplusplus
}
#endif
