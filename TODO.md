# ESP32-S3-GEEK 无线调试器开发进度追踪清单 (TODO.md)

> 本文档实时追踪项目的架构实现进度。每个子模块包含具体开发任务、完成状态及验证情况。

---

## 总体进度概览

| 阶段 | 模块 / 功能 | 状态 | 进度 | 说明 |
| :--- | :--- | :---: | :---: | :--- |
| **Phase 1** | **硬件底层、按键状态机与 1.14" LCD UI** | ✅ **已完成** | 100% | 驱动适配、20ms 消抖、短按循环、长按进度条蓄力与确认已实机烧录验证通过 |
| **Phase 2** | **TinyUSB 复合设备与动态重枚举** | ✅ **已完成** | 100% | CDC-ACM (COM41) + CDC-NCM (以太网 24) 复合设备在线，实测 12Mbps 链路与串口回环 |
| **Phase 3** | **Wi-Fi Web配网、回退保护与下载模式** | ✅ **已完成** | 100% | AP热点(GEEK-Debugger 88%信号)、Web配网后台、自动回退机制、设置菜单内一键进入ROM下载模式实机测试通过 |
| **Phase 4** | **嵌入式 WebShell 与无线控制台** | 🟡 **进行中** | 20% | WebSocket 串口双向透传 (xterm.js)、网页控制台与网络数据帧转发 |

---

## 阶段一：硬件底层、按键状态机与 LCD UI (Phase 1) - ✅ [已完成]
- [x] **硬件原理图与引脚核对**
  - [x] 确立 ST7789 SPI2 引脚：MOSI: 11, SCLK: 12, CS: 10, DC: 8, RST: 9, BL: 7
  - [x] 确立 BOOT 按键引脚：GPIO 0（内部上拉，低电平触发）
  - [x] 确立 USB-OTG 引脚：D-: 19, D+: 20
- [x] **按键事件驱动 (`components/button_ctrl`)**
  - [x] 20ms 硬件消抖，过滤杂讯毛刺
  - [x] 50ms ~ 600ms 短按识别（抬起触发，用于菜单项向下循环切换）
  - [x] 600ms ~ 1500ms 长按蓄力（周期性发送 0%~100% 进度给 UI）
  - [x] >1500ms 长按确认（无须松手即刻触发选定项生效）
  - [x] 蓄力中途松手取消保护
- [x] **ST7789 显示驱动与双缓冲渲染 (`components/display_ui`)**
  - [x] 基于 ESP-IDF v6.1 `esp_lcd` 原生驱动 ST7789
  - [x] 240×135 内部 SRAM 双缓冲 FrameBuffer，40MHz SPI 零闪烁刷新
  - [x] 仪表盘监控界面 (Dashboard)：展示运行模式、IP 地址、串口波特率、数据速率
  - [x] 菜单选择界面 (Menu)：显示候选项目，高亮条指示当前游标，支持 6 项滚动适配
  - [x] 动态蓄力进度条：长按时在底栏绘制 `Hold OK: xx% [====>   ]`
  - [x] 执行弹窗提示 (Applying)：长按确认后弹出专用提示窗口（包括专属下载模式弹窗）
  - [x] 菜单 5 秒超时无操作自动返回仪表盘
- [x] **实机编译与烧录验证**
  - [x] 成功使用 ESP-IDF v6.1 编译通过
  - [x] 成功通过 `COM40` 烧录进 ESP32-S3-GEEK 并在板载屏幕上点亮运行

---

## 阶段二：TinyUSB 复合设备与动态重枚举 (Phase 2) - ✅ [已完成]
- [x] **TinyUSB 协议栈与组件集成**
  - [x] 在 `main/idf_component.yml` 引入 `espressif/esp_tinyusb`
  - [x] 端点分配（EP1/EP2 用于 CDC-ACM，EP3/EP4 用于 CDC-NCM）
- [x] **USB 复合设备管理器 (`components/usb_manager`)**
  - [x] 实现 CDC-ACM 虚拟串口通道（支持数据收发与终端 Echo 回环）
  - [x] 实现 CDC-NCM 虚拟以太网通道（广播 MAC 地址与链路 Link Up 状态）
  - [x] 支持复合设备模式（Composite: CDC-ACM + CDC-NCM 同时在线）
  - [x] 支持纯串口模式（Pure CDC-ACM）与纯网卡模式（Pure CDC-NCM）
- [x] **动态模式切换 (Mode Switch) 与重新枚举**
  - [x] 实现 `tud_disconnect()` 断开 USB 总线
  - [x] 重新载入对应模式外设并调用 `tud_connect()` 触发重枚举
  - [x] 增加 `usb_manager_disconnect()` 供进入 Bootloader 或硬重启前平滑断开
- [x] **宿主机实机验证 (Windows 现场实测 100% 通过)**
  - [x] 当前 Windows 机器设备管理器枚举出真正复合设备：`USB\VID_303A&PID_4001`
  - [x] 当前 Windows 机器枚举出新虚拟串口：`USB 串行设备 (COM41)`
  - [x] 串口通信收发测试：发送 `HELLO DOWNLOAD MODE TEST` 成功收到 `[ESP32-S3-GEEK Echo]`
  - [x] 当前 Windows 机器生成新网络适配器：`以太网 24 (Espressif Systems USB net)`，状态 `Up`，速率 `12 Mbps`，MAC `1A-8B-0E-CC-95-6C`

---

## 阶段三：Wi-Fi Web配网、连接管理与下载模式 (Phase 3) - ✅ [已完成]
- [x] **Wi-Fi 状态机与连接管理 (`components/net_bridge`)**
  - [x] AP 热点配网模式（广播 `GEEK-Debugger`，IP `192.168.4.1`，现场扫描信号 88%）
  - [x] STA 客户端模式（读取 NVS 保存的 SSID/Password 连接局域网）
  - [x] **连接失败自动 Fallback 回退机制**（超时/密码错误 5 次后自动切回 AP 热点并提示）
  - [x] NVS 配置持久化存储与重置配置功能
- [x] **ST7789 屏幕状态同步显示**
  - [x] 状态栏动态显示：`[WIFI: AP-CFG]` / `[WIFI: CONNECTING]` / `[WIFI: ONLINE]` / `[WIFI: FALLBACK]`
  - [x] 仪表盘显示当前连接的 SSID 与动态分配到的真实 IP
- [x] **现代轻量化 Web 配网页面**
  - [x] 自动扫描并列出周围 2.4G Wi-Fi 热点
  - [x] 提交连接接口与实时连接状态反馈
- [x] **设置菜单新增下载模式 (Download Mode)**
  - [x] 菜单新增 `5. Enter Download Mode` 选项
  - [x] 短按切换到下载模式，长按 >1.5s 确认
  - [x] 触发时屏幕渲染专属黄色橙色线框：`DOWNLOAD MODE (ROM) / Entering Bootloader... / Ready for idf.py flash`
  - [x] 调用 `tud_disconnect()` 清理 USB，写入 `RTC_CNTL_OPTION1_REG` 强制进入 ROM Bootloader
  - [x] 宿主机免按按键免拔插直接识别为 `COM40`，无缝供 `idf.py flash` 烧录

---

## 阶段四：嵌入式 WebShell 与无线控制台 (Phase 4) - 🟡 [当前推进]
- [ ] **嵌入式 HTTP & WebSocket 终端服务 (`components/web_console`)**
  - [ ] 基于 `esp_http_server` 搭建轻量 Web 服务与 REST API
  - [ ] WebSocket 串口双向透传通道与 xterm.js 嵌入式终端
  - [ ] 网页端模式切换控制面板与开发板状态实时推送
- [ ] **lwIP 虚拟以太网桥接与 DHCP**
  - [ ] 为 Target 端 USB 虚拟网卡分配 IP（192.168.4.2）
  - [ ] 打通 USB 虚拟网卡与 Wi-Fi 的数据帧交换与 IP 转发 (NAPT)
