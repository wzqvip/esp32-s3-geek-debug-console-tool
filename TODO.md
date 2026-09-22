# ESP32-S3-GEEK 无线调试器开发进度追踪清单 (TODO.md)

> 本文档实时追踪项目的架构实现进度。每个子模块包含具体开发任务、完成状态及验证情况。

---

## 总体进度概览

| 阶段 | 模块 / 功能 | 状态 | 进度 | 说明 |
| :--- | :--- | :---: | :---: | :--- |
| **Phase 1** | **硬件底层、按键状态机与 1.14" LCD UI** | ✅ **已完成** | 100% | 驱动适配、20ms 消抖、短按循环、长按进度条蓄力与确认已实机烧录验证通过 |
| **Phase 2** | **TinyUSB 复合设备与动态重枚举** | ✅ **已完成** | 100% | CDC-ACM (COM41) + CDC-NCM (以太网 24) 复合设备在线，实测 12Mbps 链路与串口回环 |
| **Phase 3** | **Wi-Fi Web配网、回退保护与下载模式** | ✅ **已完成** | 100% | AP热点(GEEK-Debugger 88%信号)、Web配网后台、自动回退机制、设置菜单内一键进入ROM下载模式实机测试通过 |
| **Phase 4** | **TF 卡全量会话日志与 Web 文件管理器** | ✅ **已完成** | 100% | 4线原生SDMMC(CLK:36,CMD:35,D0~3:37/33/38/34)、多Session自动分文件、双向命令捕获、Web端日志查看/下载/删除、4MB大分区支持 |
| **Phase 5** | **全功能 Web 管理控制台与交互式终端** | ✅ **已完成** | 100% | 5合1极客暗黑风Web控制中心：全量Wi-Fi配置(DHCP/静态IP/AP参数)、Web实时交互终端(发送/接收流)、串口参数调节、屏幕LEDC PWM调光(10%~100%)、系统诊断与一键ROM下载模式 |
| **Phase 6** | **lwIP 虚拟以太网桥接与 NAPT 转发** | 🟡 **准备中** | 0% | 为 Target 端 USB 虚拟网卡分配 IP（192.168.4.2）并打通局域网与外网数据帧转发 |


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
  - [x] 仪表盘监控界面 (Dashboard)：展示运行模式、Wi-Fi 状态、IP 地址、**TF 卡容量与使用百分比 (`TF: 16GB  Log:#001 (2%)`)**、实时网络速率
  - [x] **二级层级菜单系统 (2-Level Sub-Menu)**：
    - 一级主菜单：USB 模式、TF 卡操作、Wi-Fi 设置、系统工具、返回仪表盘
    - 二级子菜单：详细操作项，支持 `< Back to Main Menu` 返回
    - 单键操作体验：短按循环切换光标，长按 >1.5s 蓄力进入子菜单或执行动作
  - [x] 动态蓄力进度条：长按时在底栏绘制 `Hold OK: xx% [====>   ]`
  - [x] 执行弹窗提示 (Applying)：长按确认后弹出专用提示窗口（包括 TF 格式化专属红框警告弹窗）
  - [x] 菜单 8 秒超时无操作自动返回仪表盘
- [x] **实机编译与烧录验证**
  - [x] 成功使用 ESP-IDF v6.1 编译通过
  - [x] 成功通过 `COM40/COM41` 烧录进 ESP32-S3-GEEK 并在板载屏幕上点亮运行
  - [x] 支持 1200-Baud 串口触碰无感自动重启进入 ROM Bootloader 烧录模式

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

## 阶段四：TF 卡全量会话日志与 Web 文件管理器 (Phase 4) - ✅ [已完成]
- [x] **TF 卡底层驱动与 FATFS 文件系统 (`components/sd_logger`)**
  - [x] 基于 ESP32-S3 原生 SDMMC Slot 1 高速 4 线驱动（CLK: 36, CMD: 35, D0: 37, D1: 33, D2: 38, D3: 34）
  - [x] 智能自适应总线：优先 4-bit 挂载，异常时自动降级 1-bit，无卡插入时安全容错不阻塞系统
  - [x] 挂载 `/sdcard` 并自动创建 `/sdcard/logs` 目录
  - [x] 获取并更新存储卡容量（总空间、剩余可用空间）
- [x] **独立会话文件管理 (Session File Management)**
  - [x] 自动扫描历史日志文件编号，递增创建 `session_001.log`, `session_002.log`...
  - [x] 写入标准化 Session 元数据文件头（会话ID、开机毫秒、波特率、流格式）
  - [x] 异步非阻塞写入队列（Ring Buffer），高波特率串口数据零丢包
  - [x] 300ms 定期自动 `fflush()` 同步刷盘，拔电不丢数据
- [x] **全量双向命令行交互记录**
  - [x] 捕获 Host 敲入的命令输入：`[TX -> Host]`
  - [x] 捕获 Target Linux 控制台与内核日志返回：`[RX <- Target]`
- [x] **Web 端全功能日志管理器 (Web Log Explorer)**
  - [x] 门户集成双选项卡（Wi-Fi 设置 / TF 卡会话日志）
  - [x] `GET /api/logs/list`：动态获取文件清单、卡剩余空间
  - [x] `GET /api/logs/view`：网页端弹窗直接预览日志文本内容
  - [x] `GET /api/logs/download`：一键下载 `.log` 原始文件至电脑
  - [x] `POST /api/logs/new_session`：网页端一键开启新会话
  - [x] `POST /api/logs/delete`：清理废弃日志文件
  - [x] `POST /api/logs/format`：Web 端一键格式化 TF 卡 (FATFS)，带二次弹窗防误触保护
- [x] **TF 卡格式化 (FATFS Reformatting)**
  - [x] 底层集成 `esp_vfs_fat_sdcard_format()` 原生物理格式化
  - [x] 格式化完成后自动重建 `/sdcard/logs` 目录并重置为 `session_001.log`
- [x] **ST7789 屏幕 UI 联动**
  - [x] 仪表盘动态展示：`TF: 16GB  Log:#001 (2%)`，卡容量紧随 `TF:` 之后，百分比格式统一
  - [x] 二级菜单集成：`2. TF Card Ops >` 包含 `1. New Session`, `2. Flush & Eject`, `3. Format Card (FATFS)`
- [x] **Flash 分区表扩展**
  - [x] 针对 ESP32-S3-GEEK 16MB Flash 定制 `partitions.csv`
  - [x] 将 App 运行分区从 1MB 扩大至 4MB（空闲率 75%），为后续高级网络协议与功能预留充裕空间

---

## 阶段五：全功能 Web 管理控制台与交互式终端 (Phase 5) - ✅ [已完成]
- [x] **现代暗黑风 5 合 1 Web 管理门户 (`components/net_bridge/wifi_portal_html.h`)**
  - [x] 选项卡一【📶 网络】：STA Wi-Fi 信号扫描、DHCP / 静态 IP 切换（可配置 IP、网关、掩码、DNS）、AP 热点自定义（SSID、密码、信道 1-13、隐藏 SSID、最大连接数）、实时网络状态（MAC、RSSI、IP）
  - [x] 选项卡二【💻 终端】：Web 实时交互式控制台，支持指令下发、`Ctrl+C` 信号注入、`Enter` 快捷执行与清屏，通过 4KB 环形队列无阻塞流式接收串口回显
  - [x] 选项卡三【💾 TF日志】：查看 TF 卡全量日志列表、实时卡空间监控、在线文本 Modal 查看、一键打包下载 `.log`、删除日志、创建新 Session
  - [x] 选项卡四【⚙️ 参数】：屏幕背光硬件 **LEDC PWM 调光**（10% ~ 100% 实时无级调节）、屏幕休眠超时（常亮/30s/1m/5m）、屏幕旋转方向；串口波特率调节（9600 至 2000000）、数据位与校验位；TF 卡日志自动记录开关与刷盘周期调节
  - [x] 选项卡五【⚡ 诊断】：ESP32-S3 CPU 状态、16MB Flash 用量、Free Heap / Min Heap 内存监测、开机时长 (Uptime)、一键设备重启、一键进入 ROM 下载模式、一键恢复出厂设置并擦除 NVS
- [x] **后台 RESTful API 体系 (`components/net_bridge/net_bridge.c`)**
  - [x] `GET /api/system/info` / `GET /api/config`
  - [x] `POST /api/config/wifi_sta` / `POST /api/config/wifi_ap`
  - [x] `POST /api/config/display` / `POST /api/config/serial` / `POST /api/config/logger`
  - [x] `POST /api/terminal/tx` / `GET /api/terminal/rx`
  - [x] `POST /api/system/reboot` / `POST /api/system/download_mode` / `POST /api/system/factory_reset`
- [x] **NVS 统一配置持久化与硬件联动**
  - [x] 存储命名空间 `sys_cfg`，开机自启加载已保存的 Wi-Fi、串口、屏幕亮度与超时参数
  - [x] 屏幕背光升级为 ESP-IDF v6.1 `esp_driver_ledc` 原生硬件 PWM 驱动

---

## 阶段六：lwIP 虚拟以太网桥接与 NAPT 转发 (Phase 6) - 🟡 [后续规划]
- [ ] **Target 端 USB 虚拟网卡 IP 分配与 DHCP 服务**
  - [ ] 启用 lwIP DHCP Server 为 Target 分配 `192.168.4.2`
- [ ] **Wi-Fi 与 USB CDC-NCM 数据帧双向桥接**
  - [ ] 打通 USB 虚拟以太网帧与 Wi-Fi STA 的桥接传输
  - [ ] 支持 NAPT (网络地址端口转换)，让 Linux 目标机通过 GEEK 连入互联网进行救援与包下载


