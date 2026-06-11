# Firmware — ESP32-S3-Touch-AMOLED-1.75

Round 466×466 AMOLED token meter. Arduino + PlatformIO. **Working end to end**:
display + WiFi + HTTPS + JSON + dual arcs + touch + on-device WiFi config.

Project: `firmware/token-meter/`  ·  Host machine: HEFEI (Ubuntu 24.04).
Last updated: 2026-06-10.

---

## 1. Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| Screen | 1.75" **round** AMOLED, 466×466, **CO5300** driver (QSPI) |
| Touch | **CST92xx / CST9217** (I2C), addr `0x5A` |
| MCU | ESP32-S3R8, 8MB PSRAM (OPI), 16MB Flash (QIO), WiFi + BLE5 |
| Extras | AXP2101 PMU, TCA9554 I/O expander, QMI8658 IMU, dual mic |
| USB | native USB-Serial-JTAG → `/dev/ttyACM0` (VID:PID `303a:1001`) |

**Pin map** (`waveshare-ref/.../libraries/Mylibrary/pin_config.h`):

```
CO5300 display, QSPI:  D0=4  D1=5  D2=6  D3=7   SCLK=38  CS=12  RESET=39   466×466
CST92xx touch, I2C:    SDA=15  SCL=14  INT=11  RESET=40
PMU:                   AXP2101
```

Note: for this board the 06_LVGL_Widgets path does **not** need the TCA9554
expander — display RESET is a direct GPIO (39). Touch RESET is GPIO40.

---

## 2. Toolchain (on HEFEI)

PlatformIO Core, installed via the official isolated-venv installer (Ubuntu 24.04
is PEP668 "externally managed", so a venv is required):

```bash
sudo apt-get install -y python3.12-venv            # prerequisite
python3 /tmp/get-platformio.py                     # from platformio-core-installer
~/.platformio/penv/bin/pio --version               # -> PlatformIO Core 6.1.19
```

`pio` is at `~/.platformio/penv/bin/pio` (not on PATH — use the absolute path).

**Serial permission:** the user must be in `dialout` to flash. Done once:
```bash
sudo usermod -aG dialout zhangjy    # permanent (new login sessions have it)
```

---

## 3. Project layout

```
firmware/
├── FIRMWARE.md                 (this file)
├── waveshare-ref/              official demo clone (vendored libs + pins live here)
│   └── examples/Arduino-v3.3.5/libraries/{GFX_Library_for_Arduino, SensorLib,
│       lvgl, lv_conf.h, Mylibrary, ESP32_IO_Expander, XPowersLib, esp-lib-utils}
└── token-meter/                the PlatformIO project
    ├── platformio.ini
    ├── src/main.cpp
    ├── include/
    │   ├── pin_config.h        (copied from Mylibrary)
    │   ├── lv_conf.h           (copied from vendored lv_conf.h — LVGL 8, RGB565)
    │   ├── secrets.h           (WiFi + meter URL/key — gitignored)
    │   └── secrets.h.example
    └── .gitignore              (.pio/, include/secrets.h)
```

The vendored libs are used **in place** via `lib_extra_dirs` (no copy). Registry
libs (`ArduinoJson`, `WiFiManager`) come via `lib_deps`. WiFi/HTTPClient/
WiFiClientSecure are built into the framework.

### platformio.ini essentials

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi      ; 16MB flash QIO + 8MB PSRAM OPI
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv
upload_port = /dev/ttyACM0
monitor_port = /dev/ttyACM0
monitor_speed = 115200
lib_extra_dirs = ${PROJECT_DIR}/../waveshare-ref/examples/Arduino-v3.3.5/libraries
lib_deps = bblanchon/ArduinoJson@^7.2.0
           tzapu/WiFiManager@^2.0.17
build_flags = -DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1
              -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I include
```

### Build / flash / monitor

```bash
cd ~/workspace/DEMOs/TokenGenie/firmware/token-meter
~/.platformio/penv/bin/pio run                 # build
~/.platformio/penv/bin/pio run -t upload       # flash via /dev/ttyACM0
# serial (see §7 for the gotcha):
stty -F /dev/ttyACM0 115200 raw -echo; timeout 9 cat /dev/ttyACM0
```

---

## 4. Display / UI

- Bus `Arduino_ESP32QSPI(CS,SCLK,D0..D3)`; panel
  `Arduino_CO5300(bus, RESET, 0, 466, 466, 6, 0, 0, 0)` — note **col offset 6**.
- LVGL 8.4, RGB565, `LV_COLOR_16_SWAP=0`, flush via `draw16bitRGBBitmap`.
- `rounder_cb` forces even start / odd end coords (CO5300 requirement).
- Two DMA draw buffers of `W*H/10` `lv_color_t` (~87KB total in internal DMA RAM).
- UI: two concentric `lv_arc`s (outer 448px = Codex green `0x00E5A0`, inner 348px =
  Claude purple `0xC77DFF`), center label = cost, bottom label = mode, top label =
  status (`WiFi...` / `updating` / `http NNN` / blank).

## 5. Touch

- IRQ-driven (this is the reliable path — polling `getPoint()` every loop was
  flaky). `attachInterrupt(TP_INT, …, FALLING)` sets a flag; the loop reads
  `touch.getPoint()` once when the flag is set.
- `touch.begin(Wire, 0x5A, SDA, SCL)`, `setMaxCoordinates(466,466)`,
  `setMirrorXY(true,true)`.
- **Single tap → cycle metric (today / month).** Nothing destructive on tap.

## 6. WiFi

Captive-portal company WiFi can't be used by a headless ESP32, so the board uses
a **phone hotspot** (or any network with internet) and reaches the data via the
public Cloudflare tunnel — see `../README.md`.

Connection order (`wifi_bringup`):
1. `WiFi.begin()` — saved NVS creds (from a prior connect / portal).
2. `WiFi.begin(WIFI_SSID, WIFI_PASS)` — hardcoded seed from `secrets.h`.
3. WiFiManager config portal `TokenGenie-Setup` — manual setup.

**Change WiFi without reflashing:** hold a finger on the screen **during boot**
(~1.2s) → wipes saved WiFi and opens the `TokenGenie-Setup` portal. Join that AP
from a phone/laptop (keep the target hotspot on; use a *second* device since a
phone can't host its hotspot and join the portal at once), pick the network, save.

`secrets.h`:
```c
#define WIFI_SSID  "your-wifi"
#define WIFI_PASS  "your-password"
#define METER_URL  "https://your-host.example.com/usage"
#define METER_KEY  "your-access-key"
```

Data fetch: `WiFiClientSecure` + `setInsecure()` + HTTPClient GET every 30s, 6s
timeouts so a stalled request can never freeze the loop. Parse with ArduinoJson.

---

## 7. Gotchas & lessons learned (the hard-won bits)

1. **arduino-esp32 3.x needs the pioarduino platform**, not stock
   `platformio/espressif32` (which ships 2.x). Tag used: `55.03.39`.
2. **GFX fork vs core API drift:** `Arduino_ESP32SPI.cpp` and
   `Arduino_ESP32SPIDMA.cpp` call the old one-arg `spiFrequencyToClockDiv()`;
   the 3.x core made it two-arg. We only use **QSPI**, so those two files were
   renamed `*.cpp.disabled` in the vendored GFX lib. (QSPI is unaffected.)
3. **`BLACK` macro** isn't exported by this GFX fork → use `0x0000` (RGB565).
4. **ArduinoJson `| 0` on a float field returns the default (0).** A JSON number
   with a fraction (e.g. `87.8`) is not `is<int>()`, so `doc[...]["pct"] | 0`
   yielded 0 and the arcs stayed empty while costs (read with `| 0.0f`) were fine.
   Fix: read pct as float and round — `(int)roundf(doc[...]["pct"] | 0.0f)`.
5. **USB-CDC serial drops after WiFi connects / on re-enumeration.** `cat
   /dev/ttyACM0` right after flashing often shows *nothing* — this does NOT mean
   the board is frozen. Run `stty -F /dev/ttyACM0 115200 raw -echo` first and read
   once the board has been up a few seconds. A 5s heartbeat (`[hb] wifi= heap=
   valid=`) is the source of truth for "is the loop alive".
6. **Don't make destructive gestures easy.** A "triple-tap = reset WiFi" gesture
   misfired during normal tap testing and wiped the creds. Moved reconfigure to a
   deliberate **boot-time hold**; runtime taps only cycle the metric.
7. **nvm node isn't on PATH** in non-interactive/systemd shells (host side) — use
   absolute paths or the project-local `node_modules/.bin`.
8. **`pkill -f usage_server.py` matched and killed the SSH shell itself** (the
   script text contains the string). Kill by port instead:
   `lsof -ti tcp:8787 | xargs -r kill`.
9. Ubuntu 24.04 is PEP668 — install PlatformIO via its venv installer, and
   `python3.12-venv` must be apt-installed first.

---

## 8. Status

- [x] Bring-up: display + LVGL dual arcs (Stage 1)
- [x] Live data: WiFi + HTTPS + JSON + dynamic arcs + tap-to-switch (Stage 2)
- [x] On-device WiFi config (WiFiManager) + robust fallback + boot-hold reset (Stage 3)
- [ ] Polish: larger center font, pixel-art center animation (the "easter egg")
- [ ] 5h rolling-window mode (needs host-side per-agent session math — v2)
