# SmartHaus-Sensors

Advanced IoT Security & Monitoring for smart homes — ESP8266 (NodeMCU) master + Arduino Mega slave.

This repository contains firmware, examples, and reference files for a small home automation/security solution combining:

- ESP8266 NodeMCU (master): Fingerprint access control, float water sensor, WiFi-to-Firebase sync, I2C master, buzzer
- Arduino Mega (slave): Relay control, SIM800L SMS alerts, I2C slave

---

## Table of Contents

- [Features](#features)
- [Hardware](#hardware)
- [Wiring / Pinout](#wiring--pinout)
  - [ESP8266 NodeMCU (Master)](#esp8266-nodemcu-master)
  - [Fingerprint Sensor (UART)](#fingerprint-sensor-uart)
  - [Arduino Mega (Slave)](#arduino-mega-slave)
  - [SIM800L GSM Module (Mega)](#sim800l-gsm-module-mega)
  - [I2C Connections](#i2c-connections)
- [Software Setup](#software-setup)
- [Firebase Realtime Database Structure](#firebase-realtime-database-structure)
- [Flashing / Build (PlatformIO)](#flashing--build-platformio)
- [Troubleshooting](#troubleshooting)
- [Security & Git](#security--git)
- [License](#license)

---

## Features

- Fingerprint-based door unlock with user names stored on the device
- Failed attempt counting and lockout after 3 failures (with buzzer)
- Water level monitoring with alerts
- Real-time synchronization to Firebase Realtime Database
- I2C master/slave communication between ESP8266 (master) and Mega (slave)
- SMS alerts from Mega using SIM800L

---

## Hardware

Minimum parts list:

- ESP8266 NodeMCU (or similar) x1
- Adafruit/GT-521F32 style fingerprint sensor (UART) x1
- Arduino Mega 2560 (for relays and SIM800L) x1
- SIM800L GSM module x1 (requires stable ~4V supply)
- 8-channel relay module (or custom relay board)
- Float water sensor (simple mechanical float switch)
- Active buzzer (active low in code)
- I2C wiring (SDA, SCL) and common ground between devices
- Power supplies as required (Mega, NodeMCU, SIM800L)

---

## Wiring / Pinout

Below are the exact pins used by the firmware in this repo. When wiring, match the signal names (GPIO numbers) rather than NodeMCU silk labels to avoid confusion.

### ESP8266 NodeMCU (Master)

- `3.3V` -> 3.3V power (fingerprint sensor, NodeMCU powered from stable 3.3V / USB)
- `GND` -> Common ground
- `D1 (GPIO5)` -> I2C SCL (Wire.begin(D2, D1) in code uses `D2` as SDA, `D1` as SCL)
- `D2 (GPIO4)` -> I2C SDA
- `D7 (GPIO13)` -> `BUZZER_PIN` (active low; HIGH = off)
- `D6 (GPIO12)` -> Float sensor pin `FLOAT_PIN` (input with `INPUT_PULLUP`, LOW = water present)

Notes in code:
- `Wire.begin(D2, D1);` — initializes I2C master using SDA=D2 (GPIO4) and SCL=D1 (GPIO5).
- `#define FLOAT_PIN 12` — float uses GPIO12 (NodeMCU D6).
- `#define BUZZER_PIN 13` — buzzer uses GPIO13 (NodeMCU D7).

### Fingerprint Sensor (UART)

The fingerprint management example (`examples/manageFingerprint.cpp`) expects the fingerprint sensor on SoftwareSerial:

- `Fingerprint TX -> NodeMCU RX` (RX pin in `SoftwareSerial mySerial(14, 12);` is first arg = RX = D5/GPIO14)
- `Fingerprint RX -> NodeMCU TX` (TX pin in `SoftwareSerial mySerial(14, 12);` is second arg = TX = D6/GPIO12)

So wiring (Sensor -> NodeMCU):
- `Sensor TX` -> `D5 (GPIO14)` (NodeMCU RX for SoftwareSerial)
- `Sensor RX` -> `D6 (GPIO12)` (NodeMCU TX for SoftwareSerial)
- `Sensor VCC` -> `3.3V` (verify sensor spec; some sensors require 3.6V–5V tolerant)
- `Sensor GND` -> `GND`

> Important: The fingerprint sensor and NodeMCU must share a common ground. Check your sensor's voltage requirements — many sensors run at 5V while NodeMCU is 3.3V; the code here expects a 3.3V-compatible interface.

### Arduino Mega (Slave)

- `I2C SDA` -> ESP8266 `SDA` (NodeMCU `D2` / GPIO4)
- `I2C SCL` -> ESP8266 `SCL` (NodeMCU `D1` / GPIO5)
- `GND` -> Common ground with ESP8266

Mega hardware serial for SIM800L (as used in examples):
- `SIM800L TX` -> `Mega RX1 (pin 19)`
- `SIM800L RX` -> `Mega TX1 (pin 18)`

Relay outputs wired to Mega digital pins (exact mapping used in `examples/mega_slave_i2c/mega_slave.ino`):

- `RELAY_BASE_PIN = 22` — Relay ID 1 → pin 22, ID 2 → pin 23, ID 3 → pin 24, and so on (ID N → pin `22 + (N-1)`).
- `MAX_RELAYS = 16` (configurable upper bound in code) — ensure your relay board has enough channels.
- `RELAY_ACTIVE_LOW = true` — relays are driven active by LOW in the current wiring (code uses inverted logic when writing pins).

Other dedicated pins on the Mega (from `examples/mega_slave_i2c/mega_slave.ino`):

- `UNLOCK_RELAY_PIN = 53` — dedicated relay used for the door unlock mechanism (default ON at startup; `unlock` command sets it OFF).
- `WATER_RELAY_PIN = 52` — dedicated water relay controlled by water sensor logic.
- `WATER_SENSOR_PIN = 48` — float/water sensor input (code assumes `HIGH` = wet, `LOW` = dry).

When the ESP8266 sends `"id:state"` (e.g., `"1:1"`), the Mega will map `id` → pin as described and apply the state. Special string commands handled by the Mega include `"lock"`, `"unlock"`, `"alert"`, `"waterempty"`, and `"waterpresent"`.

### SIM800L GSM Module (Mega)

- `VCC` -> stable ~4.0V power supply capable of ~2A peaks (recommended) — do NOT power SIM800L from the Mega 5V regulator directly
- `GND` -> Common ground with Mega and ESP
- `TX` -> Mega `RX1` (pin 19)
- `RX` -> Mega `TX1` (pin 18)

Power note: The SIM800L requires a stable 3.8–4.2V supply that can handle current spikes when the radio transmits. Use a dedicated LiPo or proper regulator and include decoupling capacitors.

### I2C Connections

- ESP8266 (Master) `SDA=D2(GPIO4)` <-> Mega `SDA` (pins 20)
- ESP8266 (Master) `SCL=D1(GPIO5)` <-> Mega `SCL` (pins 21)
- Both devices must share common ground.

> The ESP8266 acts as I2C master and the Mega is expected to run an `Wire.onReceive(...)` handler to parse command strings like `"alert"`, `"lock"`, `"unlock"`, or relay control strings like `"1:1"`.

---

## Software Setup

1. Clone the repo:

```bash
git clone https://github.com/rul3zero/SmartHaus-Sensors.git
cd SmartHaus-Sensors
```

2. Create a `secrets.h` in `include/` (this file is ignored by `.gitignore`) and add your WiFi and Firebase credentials. Example `secrets.h` minimal contents:

```cpp
// secrets.h - DO NOT COMMIT
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define API_KEY "your-firebase-api-key"
#define USER_EMAIL "your-firebase-user@example.com"
#define USER_PASSWORD "your-firebase-password"
#define DATABASE_URL "https://your-database.firebaseio.com"
```

3. Open the project with PlatformIO (VS Code) or use the CLI to build:

```powershell
# From repository root
platformio run
platformio run --target upload
```

4. Upload firmware to devices:
- `ESP8266` — upload the `src/` firmware
- `Arduino Mega` — upload the `examples/mega_slave_i2c/mega_slave.ino` firmware (open `examples/mega_slave_i2c/` for the sketch)

---

## Firebase Realtime Database Structure

The project expects this minimal structure under `/devices` and `/smart_controls`:

```
/devices/fingerprint_door_001/
  last_updated: "YYYY-MM-DD HH:MM:SS"
  failed_attempts: 0
  logs:
    YYYY-MM-DD:
      HH:MM:SS:
        status: "success" | "failed"
        user: "username" | "unknown"

/devices/water_level_001/
  water_level: true | false
  status: "water_present" | "water_empty"
  tank_status: "normal" | "alert"

/smart_controls/relays/{1..8}/state
/smart_controls/relays/door/isLocked
```

The code will write logs as:

`/devices/fingerprint_door_001/logs/2025-09-15/01:35:31/status = "success"`
and
`/devices/fingerprint_door_001/logs/2025-09-15/01:35:31/user = "Jayce"`

---

## Flashing / Build (PlatformIO)

- Open this folder in VS Code with PlatformIO extension and select the correct environment and board.
- Run `PlatformIO: Build` then `PlatformIO: Upload` for each target device.

Example CLI (Windows PowerShell):

```powershell
# Build
C:\Users\<you>\.platformio\penv\Scripts\platformio.exe run
# Upload
C:\Users\<you>\.platformio\penv\Scripts\platformio.exe run --target upload
```

---

## Troubleshooting

- Fingerprint sensor not responding:
  - Confirm TX/RX are cross-connected (sensor TX -> NodeMCU RX)
  - Ensure sensor voltage is compatible with NodeMCU (3.3V recommended)
  - Verify `SoftwareSerial` pins match `manageFingerprint.cpp` (`mySerial(14, 12)`)

- SIM800L SMS fails intermittently:
  - Ensure SIM800L has a stable 4V power supply that can provide ~2A peaks
  - Avoid powering SIM800L from the Mega 5V regulator
  - Allow >1 second windows where Mega isn't blocked by I2C interrupts while sending AT commands

- Firebase operations failing:
  - Check `secrets.h` for correct `DATABASE_URL` and API credentials
  - Verify device clock if timestamping is required — NTP is used in `setup()`

---
