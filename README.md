# ESP32-S3-GEEK Wireless Linux Debugger & Console Tool

A plug-and-play wireless debugger, rescue dongle, and interactive remote console tailored for the [Waveshare ESP32-S3-GEEK](https://docs.waveshare.com/ESP32-S3-GEEK) development board.

Plugged into a Target Linux machine's USB-A port, this dongle instantly creates dual communication channels—a **CDC-ACM Virtual Serial Console** and a **CDC-NCM High-Speed Virtual Ethernet Adapter**—while bridging them wirelessly to a Host laptop via an on-board Wi-Fi AP web portal and embedded console.

Made by Gemini 3.8 Flash Medium with me.

---

## 🌟 Key Features

### 1. Dual-Channel USB Composite Device

- **Simultaneous Online Interfaces**: Powered by TinyUSB composite stack, Target Linux identifies both `/dev/ttyACM0` (Serial Console) and `usb0` (CDC-NCM Virtual Network Card) at the same time.
- **Dynamic Mode Re-enumeration**: On-the-fly switching between **Composite Mode**, **Pure Serial Mode**, and **Pure Network Mode** using clean software USB detachment (`tud_disconnect() -> re-enumerate -> tud_connect()`).
- **High-Speed Link**: Verified 12 Mbps Full-Speed USB Ethernet with native Windows/Linux driver support (no driver installation needed).

### 2. Single-Button Hierarchical Sub-Menu System

- **20 ms Hardware Debounce**: Eliminates contact bounce and electrical glitches.
- **Short Press (< 600 ms)**: Cycles through menu options or advances to next item.
- **Long Press (> 1500 ms) with Live Charge Bar**: Dynamic progress bar (`Hold OK: 0% -> 100%`) visually fills at the bottom of the screen. Releasing early cancels safely; holding past 1.5s confirms and enters sub-menus or executes actions immediately.
- **Two-Level Sub-Menu Architecture**:
  - `MAIN MENU`: `1. USB Mode >`, `2. TF Card Ops >`, `3. Wi-Fi Config >`, `4. System Tools >`, `< Return Dashboard`
  - Sub-menus include dedicated operations and a `< Back to Main Menu` return option.
- **Auto-Dismissing Timeout**: Returns to Dashboard after 8 seconds of inactivity.

### 3. 1.14" IPS LCD Real-Time Dashboard (ST7789)

- **High Performance**: Native ESP-IDF `esp_lcd` hardware driver running at 40 MHz SPI with double-buffered internal SRAM FrameBuffer (zero flicker, 50 Hz refresh).
- **Comprehensive Monitoring**: Displays live operating mode, assigned Wi-Fi IP, target IP, serial baud rate, and real-time RX/TX network throughput.
- **Consistent TF Card & Session Formatting**: Displays card capacity directly following `TF:` with consistent percentage usage (e.g., `TF: 16GB  Log:#001 (2%)`).

### 4. Wi-Fi AP Web Portal & Automatic Fallback

- **Out-of-the-Box AP**: Broadcasts open AP `GEEK-Debugger` (default IP: `192.168.4.1`) on 2.4 GHz 802.11n.
- **Responsive Dark-Theme Web Portal**: Embedded zero-dependency web interface providing real-time Wi-Fi network scanning, SSID/Password configuration, and connection status reporting.
- **Automatic Fallback Protection**: If the target router is unreachable or password authentication fails after 5 retries, the system automatically falls back to AP configuration mode and notifies the user via the LCD.

### 5. One-Click Software Download Mode & 1200-Baud Touch

- **No Physical Button Holding or Cable Re-plugging**: Due to the enclosed case of the ESP32-S3-GEEK, entering download mode manually is cumbersome.
- **Menu-Driven DFU**: Selecting `System Tools -> 2. Enter Download Mode` softly detaches USB, sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT` in the RTC controller, and restarts straight into the ESP32-S3 ROM bootloader.
- **1200-Baud Touch Auto-Reset**: Opening the CDC-ACM port at 1200 baud triggers an automatic reboot into ROM bootloader download mode for seamless tool flashing.

### 6. MicroSD (TF) Card Full CLI Session Logging & FATFS Formatter

- **Native 4-bit SDMMC Driver**: High-speed communication with FATFS mounted at `/sdcard/logs/` using native ESP32-S3 SDMMC Slot 1 with 1-bit auto-fallback.
- **Automatic Multi-Session File Isolation**: Each reboot or manual session trigger creates an isolated file (`session_001.log`, `session_002.log`...) with standard session headers.
- **FATFS Card Formatting**: Built-in physical format tool accessible from both the LCD menu (`TF Card Ops -> 3. Format Card (FATFS)`) and the Web Portal (`POST /api/logs/format`), automatically wiping and rebuilding a fresh `/sdcard/logs` directory.
- **Bi-Directional Command Capture**: Logs both Host input `[TX -> Host]` and Target Linux shell/kernel panic output `[RX <- Target]` through an async non-blocking queue.
- **Interactive Web Log Explorer**: Access `http://192.168.4.1` in your browser to view logs directly in an in-browser modal, download `.log` files to your PC, or delete old logs.

### 7. 5-in-1 Cyber-Dark Web Management Console & Web Terminal

- **Full Wi-Fi Control (STA & AP)**: Wi-Fi signal scanner with RSSI bars, DHCP or Static IP (custom IP, Mask, Gateway, DNS), auto-reconnect, and custom AP settings (SSID, WPA2 password, channel 1-13, hidden SSID).
- **Interactive Web Serial Terminal**: Built-in browser terminal console! Send commands directly to Target Linux, inject `Ctrl+C` interrupt signals, execute with `Enter`, clear screen, and view real-time streaming target responses without needing PuTTY.
- **Hardware PWM LCD Dimming**: Smooth 10% - 100% brightness control driven by ESP32-S3 hardware LEDC timer and PWM channel, with configurable screen sleep timeouts (Never, 30s, 1m, 5m).
- **Device Diagnostics & Actions**: View live CPU load, 16MB Flash usage, Free Heap memory monitoring, Uptime, and trigger one-click Reboot, ROM Download Mode, or Factory Reset from anywhere on the local network.

---

## 🛠️ Hardware Specifications & Pinout

The project is customized specifically for the **Waveshare ESP32-S3-GEEK** (ESP32-S3R2, 16MB Flash, 2MB Quad PSRAM):

| Peripheral       | Component          |  GPIO Pin   | Function / Description               |
| :--------------- | :----------------- | :---------: | :----------------------------------- |
| **LCD**          | ST7789 IPS 1.14"   | **GPIO 11** | SPI2 MOSI                            |
|                  | (240 × 135 RGB)    | **GPIO 12** | SPI2 SCLK                            |
|                  |                    | **GPIO 10** | SPI2 Chip Select (CS)                |
|                  |                    | **GPIO 8**  | Data / Command (DC)                  |
|                  |                    | **GPIO 9**  | Hardware Reset (RST)                 |
|                  |                    | **GPIO 7**  | Backlight Control (BL)               |
| **Button**       | BOOT Key           | **GPIO 0**  | Active Low, internal pull-up enabled |
| **USB**          | Native USB OTG     | **GPIO 19** | USB D- (Data Minus)                  |
|                  |                    | **GPIO 20** | USB D+ (Data Plus)                   |
| **MicroSD / TF** | 4-bit SDMMC Slot 1 | **GPIO 36** | SDMMC CLK                            |
|                  | FATFS on `/sdcard` | **GPIO 35** | SDMMC CMD                            |
|                  |                    | **GPIO 37** | SDMMC Data 0 (D0)                    |
|                  |                    | **GPIO 33** | SDMMC Data 1 (D1)                    |
|                  |                    | **GPIO 38** | SDMMC Data 2 (D2)                    |
|                  |                    | **GPIO 34** | SDMMC Data 3 (D3)                    |

---

## 📂 Project Architecture

```text
esp32-s3-geek debug-console-tool/
├── CMakeLists.txt                         # Root CMake build configuration
├── sdkconfig.defaults                     # S3 target, 16MB flash, PSRAM, and TinyUSB defaults
├── dependencies.lock                      # Component dependency version lockfile
├── flash.ps1                              # Quick flash & monitor helper script (PowerShell)
├── README.md                              # Main documentation (English)
├── TODO.md                                # Real-time phase & milestone tracking checklist
├── ESP32_S3_GEEK_WIRELESS_DEBUGGER_DESIGN.md # In-depth technical architecture document
│
├── main/                                  # Application layer
│   ├── CMakeLists.txt
│   ├── idf_component.yml                  # IDF Component Manager manifest (esp_tinyusb dependency)
│   └── main.c                             # Main entry point & component orchestration
│
└── components/                            # Modular, reusable subsystem components
    ├── button_ctrl/                       # Non-blocking button event state machine
    │   ├── include/button_ctrl.h
    │   └── button_ctrl.c
    ├── display_ui/                        # ST7789 driver, FrameBuffer renderer, and Menu UI
    │   ├── include/display_ui.h
    │   ├── display_ui.c
    │   └── font8x16.h
    ├── usb_manager/                       # TinyUSB composite driver (CDC-ACM + CDC-NCM)
    │   ├── include/usb_manager.h
    │   └── usb_manager.c
    └── net_bridge/                        # Wi-Fi AP/STA manager, HTTP captive portal, auto-fallback
        ├── include/net_bridge.h
        ├── net_bridge.c
        └── wifi_portal_html.h
```

> **Note on `managed_components/`**:
> The `managed_components/` directory contains vendored third-party dependencies (such as `espressif/esp_tinyusb`) that are **automatically fetched and checked** by the ESP-IDF Component Manager based on `main/idf_component.yml`. Therefore, `managed_components/` is **intentionally excluded from Git tracking** (`.gitignore`) to keep the repository lightweight and clean.

---

## 🚀 Getting Started

### Prerequisites

- **ESP-IDF**: Version `v5.3` or `v6.1` installed (e.g. at `C:\esp\v6.1\esp-idf`).
- **Python**: Version 3.10+ in the ESP-IDF virtual environment.
- **Hardware**: Waveshare ESP32-S3-GEEK connected to your host via USB.

### Build and Flash

1. **Activate ESP-IDF Environment**:

   ```powershell
   # PowerShell
   . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
   ```

2. **Configure Target (First Time Only)**:

   ```bash
   idf.py set-target esp32s3
   ```

3. **Build the Firmware**:

   ```bash
   idf.py build
   ```

   _The ESP-IDF Component Manager will automatically download required components (like `esp_tinyusb`) into `managed_components/`._

4. **Flash and Monitor**:
   ```bash
   # Replace COM40 with your board's bootloader or serial port
   idf.py -p COM40 flash monitor
   ```

---

## 📖 Operating Guide

### Single-Button UI Navigation

- **Wake / Open Menu**: Short press the board's BOOT button while on the Dashboard view.
- **Navigate Options**: Short press to cycle the selection downward through the 6 options:
  1. `1. Composite (CDC+Net)`: Enables both CDC-ACM serial and CDC-NCM Ethernet simultaneously.
  2. `2. Pure Serial (CDC)`: Dedicated high-throughput serial console.
  3. `3. Pure Network (NCM)`: Dedicated virtual Ethernet card.
  4. `4. Wi-Fi: Reset to AP`: Erases saved Wi-Fi credentials in NVS and activates the configuration AP.
  5. `5. Enter Download Mode`: Softly detaches USB and reboots into ROM Bootloader (`COM40`) for easy reflashing.
  6. `6. System Reboot`: Software restart of the ESP32-S3.
- **Confirm & Apply**: Press and hold the BOOT button for > 1.5 seconds until the `Hold OK: 100%` progress bar completes.

### Configuring Wi-Fi via Web Portal

1. Connect your smartphone, tablet, or laptop to the Wi-Fi AP:
   - **SSID**: `GEEK-Debugger`
   - **Password**: None (Open network)
2. Open your web browser and navigate to:
   ```text
   http://192.168.4.1
   ```
3. Click **Scan Networks**, select your local 2.4 GHz Wi-Fi SSID, enter your password, and click **Connect**.
4. The board will connect to the network, and the LCD dashboard will update to show `[WIFI: ONLINE]` along with the dynamically acquired IP address.
5. If connection fails, the board automatically falls back to `[WIFI: FALLBACK->AP]` so you are never locked out.

---

## 🧪 Hardware Verification Status

- ✅ **CDC-ACM Serial (`COM41`)**: Loopback echo verified (`[ESP32-S3-GEEK Echo]` verified).
- ✅ **CDC-NCM Ethernet (`以太网 24`)**: Link Up at 12 Mbps, MAC `1A-8B-0E-CC-95-6C`.
- ✅ **LCD Display**: ST7789 IPS active, double-buffered FrameBuffer rendering at 50 Hz.
- ✅ **Wi-Fi AP (`GEEK-Debugger`)**: 88% signal strength verified via host scan on Channel 1.
- ✅ **Download Mode**: Successfully transitions from TinyUSB user mode to hardware ROM bootloader (`COM40`) without requiring physical button presses.

---

## 📄 License

This project is licensed under the Apache 2.0 License.
