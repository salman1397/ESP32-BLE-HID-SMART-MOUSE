# ESP32 BLE HID Smart Mouse

A fully wireless Bluetooth Low Energy (BLE) HID mouse built on the **DOIT ESP32 DevKit V1**, featuring a joystick-based cursor, an SSD1306 OLED display, live weather mode, a mini breakout game, and an animated sleep screen — all in one compact device.

---

## Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [GPIO Mapping](#gpio-mapping)
- [Parameter Settings](#parameter-settings)
- [Operating Modes](#operating-modes)
- [Joystick Controls](#joystick-controls)
- [Configuration](#configuration)
- [Build & Flash Process](#build--flash-process)
- [Sleep Screen Designs](#sleep-screen-designs)
- [Project Structure](#project-structure)
- [License](#license)

---

## Features

| Feature | Details |
|---------|---------|
| BLE HID Mouse | NimBLE stack, BLE 4.2, HID over GATT |
| Device Name | `Shushant Mouse` |
| Display | 128×64 SSD1306 OLED over I²C |
| Cursor Control | Analog joystick with exponential curve |
| Click | Short press = Left click, 500 ms hold = Right click |
| Scroll | Dedicated scroll mode (smooth / step) |
| Weather | Live weather via OpenWeatherMap API over WiFi |
| Game | Built-in Breakout-style ball game (3 difficulty levels) |
| Sleep Screen | 5 rotating creative tech designs, auto-activates after 30 s idle |
| Battery Level | Reports 100 % to host OS on connect |

---

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| MCU | DOIT ESP32 DevKit V1 (ESP32-D0WD-V3) |
| Display | SSD1306 OLED 128×64 (I²C) |
| Joystick | 2-axis analog + push-button (e.g. KY-023) |
| Power | USB or 3.7 V LiPo via onboard regulator |

### Wiring Diagram

```
ESP32 DevKit V1
┌─────────────────────────────────────────┐
│                                         │
│  GPIO21 ──── OLED SDA                   │
│  GPIO22 ──── OLED SCL                   │
│  3.3V   ──── OLED VCC                   │
│  GND    ──── OLED GND                   │
│                                         │
│  GPIO33 ──── Joystick VRx (X axis)      │
│  GPIO32 ──── Joystick VRy (Y axis)      │
│  GPIO25 ──── Joystick SW (button)       │
│  3.3V   ──── Joystick VCC               │
│  GND    ──── Joystick GND               │
│                                         │
└─────────────────────────────────────────┘
```

---

## GPIO Mapping

| GPIO | ADC Channel | Function | Direction | Notes |
|------|-------------|----------|-----------|-------|
| 21 | — | I²C SDA (OLED) | Bidirectional | 400 kHz, internal pull-up |
| 22 | — | I²C SCL (OLED) | Output | 400 kHz, internal pull-up |
| 33 | ADC1_CH5 | Joystick X axis | Input (analog) | UP / DOWN cursor |
| 32 | ADC1_CH4 | Joystick Y axis | Input (analog) | LEFT / RIGHT cursor |
| 25 | — | Joystick switch | Input (digital) | Internal pull-up, active LOW |

### I²C Bus

| Signal | Pin | Address |
|--------|-----|---------|
| SDA | GPIO21 | — |
| SCL | GPIO22 | — |
| OLED (primary) | — | 0x3C |
| OLED (secondary fallback) | — | 0x3D |

### ADC Configuration

| Axis | Channel | Attenuation | Range | Center (calibrated at boot) |
|------|---------|-------------|-------|-----------------------------|
| X (UP/DOWN) | ADC1_CH5 | 12 dB | 0 – 4095 | ~2761 |
| Y (LEFT/RIGHT) | ADC1_CH4 | 12 dB | 0 – 4095 | ~2640 |

> The center value is auto-calibrated by averaging 48 samples during `joystick_init()` at startup.

---

## Parameter Settings

All configurable parameters are `#define` constants at the top of `src/main.c`.

### I²C / OLED

| Define | Default | Description |
|--------|---------|-------------|
| `I2C_PORT` | `I2C_NUM_0` | I²C peripheral |
| `I2C_SDA_PIN` | `21` | SDA GPIO |
| `I2C_SCL_PIN` | `22` | SCL GPIO |
| `I2C_FREQ_HZ` | `400000` | Bus frequency (400 kHz) |
| `OLED_WIDTH` | `128` | Display width in pixels |
| `OLED_HEIGHT` | `64` | Display height in pixels |
| `OLED_ADDR_PRIMARY` | `0x3C` | Primary I²C address |
| `OLED_ADDR_SECONDARY` | `0x3D` | Fallback I²C address |

### Joystick

| Define | Default | Description |
|--------|---------|-------------|
| `JOY_X_PIN` | `ADC1_CHANNEL_5` | X axis ADC channel (GPIO33) |
| `JOY_Y_PIN` | `ADC1_CHANNEL_4` | Y axis ADC channel (GPIO32) |
| `JOY_SW_GPIO` | `GPIO_NUM_25` | Button GPIO |
| `JOY_CENTER_DEFAULT` | `2048` | Fallback center if calibration fails |
| `JOY_DEADZONE` | `300` | ADC counts ignored around center |

### Button Timing

| Define | Default | Description |
|--------|---------|-------------|
| `SW_DEBOUNCE_MS` | `50` | Debounce window (ms) |
| `SW_HOLD_MS` | `500` | Threshold for mid-press (right-click) |
| `SW_LONG_MS` | `3000` | Threshold for long-press (open menu) |

### UI / Loop

| Define | Default | Description |
|--------|---------|-------------|
| `LOOP_MS` | `20` | Main loop tick (50 Hz) |
| `MENU_STEP_MS` | `220` | Minimum ms between menu scroll steps |
| `DOUBLE_CLICK_MS` | `300` | Max gap between clicks to count as double |
| `SLEEP_TIMEOUT_MS` | `30000` | Idle ms before sleep screen activates |

### Mouse Speed

Cursor speed is controlled by `max_step` in the `joy_axis_to_delta()` calls inside `handle_run_logic()`:

| Axis | Current max_step | Max pixels/tick at 50 Hz |
|------|-----------------|--------------------------|
| X (horizontal) | 15 | 15 px |
| Y (vertical) | 15 | 15 px |

Increase `max_step` to go faster, decrease to go slower.

### WiFi / Weather

| Define | Default | Description |
|--------|---------|-------------|
| `WIFI_SSID_CFG` | `"YOUR_WIFI_SSID"` | WiFi network name |
| `WIFI_PASS_CFG` | `"YOUR_WIFI_PASSWORD"` | WiFi password |
| `WEATHER_API_KEY` | `"YOUR_API_KEY"` | OpenWeatherMap API key |
| `WEATHER_CONNECT_MS` | `15000` | WiFi connect timeout (ms) |
| `CITY_COUNT` | `4` | Number of cities in the weather list |

> **Note:** Register for a free API key at [openweathermap.org](https://openweathermap.org/api).

### Cities

Modify `s_city_names[]` and `s_city_queries[]` in `src/main.c` to change the weather city list:

```c
static const char *s_city_names[]   = {"Bengaluru", "Kolkata", "Mumbai", "Delhi"};
static const char *s_city_queries[] = {
    "Bengaluru,IN", "Kolkata,IN", "Mumbai,IN", "Delhi,IN"
};
```

### BLE HID

| Setting | Value |
|---------|-------|
| Device name | `Shushant Mouse` |
| Appearance | Mouse (0x03C2) |
| Vendor ID | 0x16C0 |
| Product ID | 0x27DB |
| HID Version | 1.0.0 |
| BLE stack | NimBLE |
| Transport | BLE only |
| Battery level reported | 100 % |

---

## Operating Modes

Switch between modes via the **Hold 3 s → long press** menu.

| Mode | Description |
|------|-------------|
| **Normal Cursor** | Standard HID mouse. Short press = left click, 500 ms = right click. |
| **Right + Cursor** | Cursor moves; short press toggles right button hold; 500 ms toggles middle button hold. |
| **Left + Cursor** | Cursor moves; short press toggles left button hold; 500 ms toggles middle button hold. |
| **Scroll Mode** | Joystick scrolls vertically (wheel) and horizontally (pan). Short press toggles smooth/step style. |
| **Game** | Breakout-style paddle game. Choose difficulty (Easy / Mid / Hard). Short press restarts. |
| **Weather** | Connects to WiFi and fetches live weather. Select city with joystick, confirm with short press. |

---

## Joystick Controls

| Action | Result |
|--------|--------|
| Move joystick | Move cursor (or scroll in Scroll mode) |
| Short press (< 500 ms) | Left click / action confirm |
| Medium press (500 ms – 3 s) | Right click / secondary action |
| Long press (≥ 3 s) | Open/close mode menu |
| In menu — tilt joystick | Navigate up / down |
| In menu — short press | Select highlighted mode |
| In Weather Show — tilt left/right | Switch to next/previous city and re-fetch |
| Any input in Sleep | Wake up to normal mode |

---

## Configuration

### 1. Update WiFi credentials

Open `src/main.c` and change:

```c
#define WIFI_SSID_CFG   "YOUR_WIFI_SSID"
#define WIFI_PASS_CFG   "YOUR_WIFI_PASSWORD"
```

### 2. Set your OpenWeatherMap API key

```c
#define WEATHER_API_KEY "YOUR_API_KEY"
```

### 3. (Optional) Change city list

```c
static const char *s_city_names[]   = {"City1", "City2", "City3", "City4"};
static const char *s_city_queries[] = {"City1,CC", "City2,CC", "City3,CC", "City4,CC"};
```
Replace `CC` with the two-letter ISO country code.

### 4. (Optional) Change BLE device name

```c
.device_name = "Shushant Mouse",
```

---

## Build & Flash Process

### Prerequisites

| Tool | Version | Link |
|------|---------|------|
| PlatformIO Core | ≥ 6.1 | [platformio.org](https://platformio.org) |
| VS Code + PlatformIO extension | latest | [marketplace](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) |
| ESP-IDF (via PlatformIO) | 5.5.0 | auto-installed |
| USB driver (CH340/CP2102) | latest | board-dependent |

### Steps

```bash
# 1. Clone the repository
git clone https://github.com/salman151397/ESP32-BLE-HID-SMART-MOUSE.git
cd ESP32-BLE-HID-SMART-MOUSE

# 2. Open in VS Code with PlatformIO installed
code .

# 3. Edit WiFi/API credentials in src/main.c (see Configuration above)

# 4. Build
pio run

# 5. Flash (ensure ESP32 is connected via USB)
pio run --target upload

# 6. Monitor serial output (optional)
pio device monitor --baud 115200
```

### PlatformIO Configuration (`platformio.ini`)

```ini
[env:esp32doit-devkit-v1]
platform  = espressif32
board     = esp32doit-devkit-v1
framework = espidf
board_build.partitions = partitions_singleapp_large.csv
```

> **Partition:** A 1.5 MB single-app large partition is required. The firmware is ~1.14 MB.  
> The file `partitions_singleapp_large.csv` is included in the repository root.

### sdkconfig Key Settings

The file `sdkconfig.esp32doit-devkit-v1` contains the ESP-IDF config. Key settings:

| Config Key | Value | Purpose |
|-----------|-------|---------|
| `CONFIG_BT_NIMBLE_HID_SERVICE` | `y` | Enable NimBLE HID service |
| `CONFIG_BT_NIMBLE_SVC_HID_MAX_INSTANCES` | `2` | HID instances |
| `CONFIG_ESP_WIFI_ENABLED` | `y` | Enable WiFi (for weather mode) |
| `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS` | `y` | HTTPS support |
| `CONFIG_LWIP_ENABLE` | `y` | TCP/IP stack |
| `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` | `y` | 1.5 MB partition |
| WiFi IRAM opts | all disabled | Prevent IRAM overflow |

---

## Sleep Screen Designs

After **30 seconds of idle**, the display switches to sleep mode showing one of 5 rotating creative designs. The design changes every **15 seconds**:

| # | Design | Description |
|---|--------|-------------|
| 0 | **Nested Boxes** | Concentric rectangles with diagonal corner accents and `SHUSHANT MOUSE v3` |
| 1 | **Circuit Board** | PCB-style horizontal traces, vertical bridges, and filled vias with `PCB DESIGN` |
| 2 | **Oscilloscope** | Dual sine waves on calibrated axes with tick marks and `~ SIGNAL ~` |
| 3 | **Crosshair Target** | Full-screen crosshair with 3 concentric circles and `ESP32 HID` |
| 4 | **Binary Rain** | Rows of binary digits with `BLE MOUSE v3.0` centre banner |

Any joystick movement or button press wakes the device instantly.

---

## Project Structure

```
ESP32-BLE-HID-SMART-MOUSE/
├── src/
│   ├── main.c              # All firmware logic (~1300 lines)
│   ├── esp_hid_gap.c       # BLE GAP / advertising helper
│   └── esp_hid_gap.h
├── include/
│   └── README
├── lib/
│   └── README
├── test/
│   └── README
├── platformio.ini          # PlatformIO build config
├── partitions_singleapp_large.csv   # 1.5 MB partition table
├── sdkconfig.esp32doit-devkit-v1    # ESP-IDF sdkconfig
├── CMakeLists.txt
└── README.md               # This file
```

---

## License

MIT License — free to use, modify, and distribute.  
Built with ❤️ by **Shushant** | Firmware by **InfinityX Innovations**