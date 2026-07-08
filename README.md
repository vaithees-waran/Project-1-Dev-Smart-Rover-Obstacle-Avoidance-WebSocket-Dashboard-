# LinkX Smart Rover — WiFi Obstacle-Avoidance Dashboard

## 1. Project Title
**LinkX Smart Rover: AP-Mode Obstacle-Avoidance WebSocket Dashboard (ESP32-S3)**

## 2. Objective
Build a WiFi-controlled rover on the Gbro/LinkX ESP32-S3 board that can be driven from a mobile-responsive web dashboard, while an onboard ultrasonic sensor autonomously prevents collisions — either by auto-stopping in manual mode or by fully self-driving in "Auto-Avoid" mode.

## 3. Real-World Applications
- STEM / robotics education kits
- Warehouse or indoor delivery-bot prototyping
- Remote inspection rover for tight/hazardous spaces
- Base platform for security patrol bots
- Base platform for any of the other 43 project ideas that need drive + sensing

## 4. Hardware Required & Pin Mapping

| Component | Pin(s) | Notes |
|---|---|---|
| ESP32-S3 (LinkX board) | — | Built-in |
| Dual DC motor driver (built-in) | IN1=11, IN2=10, PWMA=12, IN3=46, IN4=13, PWMB=3 | Already wired on-board |
| RGB LED (built-in) | R=45, G=48, B=47 | Status indicator |
| Buzzer (built-in) | 14 | Alert sounds |
| HC-SR04 ultrasonic | TRIG=35, ECHO=36 | 5V logic — use a voltage divider or level shifter on ECHO if your module is 5V-only |
| Power | 2S/3S Li-ion or 4xAA via VIN, or USB-C for bench testing | Motors need a supply that can source their stall current |

## 5. Wiring Table

| HC-SR04 Pin | ESP32-S3 Pin |
|---|---|
| VCC | 5V |
| GND | GND |
| TRIG | GPIO35 |
| ECHO | GPIO36 (through 1kΩ/2kΩ divider if module outputs 5V) |

All motor/RGB/buzzer wiring is already done on-board — no extra wiring needed for those.

## 6. Dashboard Layout
Single-page mobile-first dashboard (`/data/index.html`) with four cards:
1. **Connection status** — live dot (grey=connecting, green=connected, red pulsing=obstacle)
2. **Obstacle distance** — big numeric readout + animated bar gauge
3. **Drive control** — 5-button D-pad (forward/back/left/right/stop), press-and-hold semantics
4. **Safety threshold slider** (5–60 cm) and **Auto-Avoid** mode toggle
5. **Event log** — live-scrolling event feed + "Download CSV" link to the LittleFS log

## 7–8. UI + AP/STA mode
See `data/index.html` for the full HTML/CSS/JS (dark theme, WebSocket client with auto-reconnect). The firmware brings up **AP mode** (`LinkX-Rover` / `12345678`) unconditionally at `192.168.4.1`, and will additionally join a home network in **STA mode** if you fill in `STA_SSID`/`STA_PASSWORD` at the top of the sketch — both run simultaneously (`WIFI_AP_STA`).

## 9–10. Firmware + WebSocket updates
See `rover_dashboard.ino`. Uses `ESPAsyncWebServer` + `AsyncWebSocket` to push JSON state (`distance`, `obstacle`, `motion`, `auto`) to all connected clients 5x/second, and pushes discrete `event` messages instantly when something notable happens (obstacle detected, threshold changed, mode changed, safety stop).

## 11. Sensor Calibration
The HC-SR04 needs no factory calibration, but you should tune two things after assembly:
- **Threshold**: use the dashboard slider live while walking an object toward the rover; set it a few cm past your chassis's actual stopping distance.
- **Timing constant**: `ECHO_TIMEOUT_US` (25ms) covers ~4m range; shorten it if you want faster loop timing in a small room.

## 12. Data Logging (LittleFS)
Every notable event (`Obstacle detected…`, `Threshold set to…`, `Auto-Avoid ENABLED`, `Safety auto-stop…`) is appended to `/log.csv` as `millis,event`. The file auto-rotates to `/log_old.csv` past ~200KB to avoid filling flash. Download anytime via the dashboard's "Download CSV" link (served at `/log.csv`).

## 13. OTA Firmware Update
`ArduinoOTA` is enabled with hostname `linkx-rover`. After the first USB flash, subsequent updates can be pushed from Arduino IDE (`Tools > Port > linkx-rover at 192.168.x.x`) or `platformio run -t upload --upload-port linkx-rover.local`, as long as your dev machine is on the same STA network as the rover (or connected to its AP).

## 14. OLED/LCD Integration
Not required for this project, but if you want a physical mirror of the dashboard, wire an SSD1306 OLED to SDA=GPIO8/SCL=GPIO9 and print `distanceCm`/`currentMotion` in the sensor-poll block — see Project #41 in the idea list for the full pattern.

## 15. RGB LED Status Logic
| State | Color |
|---|---|
| Boot / OTA in progress | Purple |
| Idle, no obstacle | Dim green |
| Moving, no obstacle | Blue |
| Obstacle detected | Red |

## 16. Buzzer Alert Logic
| Event | Pattern |
|---|---|
| Command received | 15ms click |
| Boot complete | 2 short beeps |
| Obstacle detected | 3 beeps (80ms on/off) |

## 17. Touch Button Functionality
Not wired into this build (kept free for a future "manual override" or "auto-avoid toggle" — Touch1/Touch2 on GPIO1/GPIO2 are untouched and available if you want to add a physical Auto-Avoid toggle later).

## 18. Automation Rules
- **Obstacle rule**: distance ≤ threshold → stop, and if Auto-Avoid is on, reverse 300ms + turn 300ms, then resume scanning.
- **Dead-man rule**: if a manual drive command isn't refreshed within 800ms, motors auto-stop (protects against dropped WiFi packets leaving the rover driving blind).
- **Log rotation rule**: log file rotates at 200KB.

## 19. Power Optimization
- PWM frequency kept at 5kHz/8-bit (efficient for small DC motors, inaudible whine avoided)
- Sensor polled at 10Hz (not continuously) — enough for a slow rover, saves CPU/power
- WiFi TX power can be reduced with `WiFi.setTxPower(WIFI_POWER_11dBm)` if range isn't critical and battery life is
- BLE stack is never initialized in this sketch (saves ~10-15mA vs having it running idle)

## 20. Error Handling & Recovery
- Ultrasonic `pulseIn` has a hard timeout (25ms) — a disconnected/faulty sensor reads as "clear" (max distance) rather than hanging the loop
- Malformed WebSocket JSON is caught and ignored (`deserializeJson` error check)
- LittleFS mount failure is caught and logged to Serial; server still starts (dashboard just won't have logging)
- WebSocket client disconnects are cleaned up every loop (`ws.cleanupClients()`) to prevent memory leaks
- STA connection attempt has an 8-second timeout and falls back to AP-only rather than blocking forever

## 21. Testing Procedure
1. Flash firmware, upload `/data` to LittleFS
2. Open Serial Monitor @115200 — confirm `[AP] IP: 192.168.4.1`
3. Connect phone to `LinkX-Rover` WiFi, browse to `192.168.4.1`
4. Confirm dashboard shows "Connected" and a live distance reading
5. Drive forward — confirm motors respond and RGB turns blue
6. Place hand within threshold distance — confirm auto-stop, red RGB, triple beep, log entry appears
7. Toggle Auto-Avoid — confirm rover drives forward and reverses/turns on its own near an obstacle
8. Download the CSV log and confirm entries are present
9. Push an OTA update and confirm it applies without re-flashing over USB

## 22. Future Improvements
- Add a second ultrasonic (or a rotating servo-mounted one) for wider-angle obstacle mapping
- Add MPU6050 for heading-lock/straight-line correction
- Add a simple SLAM/occupancy grid using turn+distance history from `/log.csv`
- Add user auth (simple token) on the WebSocket before allowing drive commands
- Add mobile app wrapper (WebView) with haptic feedback for obstacle events

## 23. Project Folder Structure
```
rover_project/
├── rover_dashboard.ino     # Main firmware (this is the Arduino sketch)
├── data/
│   └── index.html           # Dashboard UI, uploaded to LittleFS
└── README.md                 # This document
```

## 24. Library Installation Guide
In Arduino IDE, **Sketch > Include Library > Manage Libraries**, install:
- `ESP Async WebServer` (by lacamera / ESP32Async, or the me-no-dev original)
- `AsyncTCP` (by me-no-dev / dvarrel — ESP32-compatible fork)
- `ArduinoJson` (by Benoit Blanchon, v6.x)

`LittleFS`, `WiFi`, and `ArduinoOTA` ship with the ESP32 board package (install via **Boards Manager → esp32 by Espressif Systems** if not already present).

For uploading `/data` to LittleFS in Arduino IDE, install the **ESP32 Sketch Data Upload** tool: https://github.com/lorol/arduino-esp32fs-plugin (or use **PlatformIO**, which handles this natively with `pio run --target uploadfs`).

## 25. Demo Workflow (Power-On to Operation)
1. Power the board (USB-C or battery via VIN)
2. RGB flashes purple briefly (boot), then settles to dim green (idle) with 2 confirmation beeps
3. AP `LinkX-Rover` becomes visible within ~2 seconds
4. Connect phone, open `192.168.4.1` — dashboard loads, WebSocket connects (green dot)
5. Press-and-hold Forward — rover drives, RGB turns blue
6. Approach an obstacle — rover auto-stops at the configured threshold, RGB flashes red, buzzer triple-beeps, event logged
7. Flip to Auto-Avoid — release manual control, rover patrols forward and self-corrects around obstacles indefinitely
8. Review/download the event log anytime from the dashboard
