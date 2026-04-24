# CDRLC Study Room Availability Monitor
### Final Project Documentation

---

## Team Members

| Name | NetID | Email |
|------|-------|-------|
| *(Your Name Here)* | *(NetID)* | *(email@illinois.edu)* |
| *(Team Member 2)* | *(NetID)* | *(email@illinois.edu)* |
| *(Team Member 3)* | *(NetID)* | *(email@illinois.edu)* |
| *(Team Member 4)* | *(NetID)* | *(email@illinois.edu)* |

**Project Name:** CDRLC Study Room Availability Monitor

---

## Abstract

> *(HARD LIMIT: 100 words or fewer)*

The CDRLC Study Room Availability Monitor is a real-time room status display system for a university library. Four networked Arduino boards work together to show live Google Calendar availability on a 4" color TFT touch screen and bi-color LED panels. Users can browse available rooms across multiple dates and time slots, and initiate a booking by tapping the screen to generate a QR code that opens the room's Google Calendar page. A Python server hosted on a NAS uses Tailscale Funnel for secure HTTPS access without port forwarding.

---

## 1. Overall Description of Project Idea

The CDRLC Study Room Availability Monitor is designed to solve a common problem in university libraries: students cannot easily tell at a glance whether a study room is available without checking their phones or walking up to each door. The goal of this project is to install a physical, always-on kiosk display near the study room hallway in the CDRLC (Campus Data Research and Learning Center, or equivalent library study room section) that shows real-time room availability for all eight rooms in the wing.

The system fetches availability data from Google Calendar's public scheduling API every five minutes and presents it in an easy-to-read color-coded format: green rows indicate a room is free for the selected time slot, while grey rows mean it is already booked. A large 4-inch TFT touch screen serves as the primary display interface, allowing users to tap a free room and instantly receive a QR code that opens the corresponding Google Calendar appointment scheduling page on their phone — enabling them to complete a booking in seconds without ever needing to log in through the kiosk device itself.

In addition to the main screen, eight bi-color (red/green) LED panels mounted on two separate "wing" boards provide a hardware-level at-a-glance view visible from further away. The system also features ambient light sensing (auto-brightness), proximity-based power saving (LEDs dim when no one is nearby), live temperature and humidity readings, and a buzzer alert that chimes when a previously booked room becomes available.

The system is designed with extensibility in mind: the NAS-hosted Python server exposes a clean HTTP API that can be consumed by any future client, and a mobile fallback page (`/b/<CODE>`) lets users see the current booking status from any phone without the kiosk.

---

## 2. Final Project Design — Use of Multiple Arduinos

The project is built around four Arduino microcontroller boards, each assigned a distinct role. This multi-board architecture was required by the project specification and also reflects a natural separation of concerns: different hardware peripherals have conflicting pin or timing demands that make a single-board design impractical.

**Arduino 1 — Master Node (Arduino R4 WiFi)**
This is the system brain. The R4 WiFi was chosen because it is the only board in the Arduino ecosystem with built-in 2.4 GHz WiFi (via the u-blox S110 module) and enough memory (256 KB flash, 32 KB SRAM) to run the TFT rendering, QR code generation, HTTP client, and I2C master logic simultaneously. It hosts the 4" TFT touch screen on its SPI bus, manages button inputs, drives the buzzer, queries the NAS server over HTTPS, and acts as I2C master to coordinate all other boards.

**Arduino 2 — Environment Sensor Node (Arduino UNO R3)**
This board's sole job is to read the DHT11 temperature and humidity sensor and serve that data over I2C on demand. The DHT11 requires precise microsecond-level timing to read, and isolating it on a dedicated board prevents the timing-sensitive sensor reading from interfering with the Master's SPI, WiFi, or display tasks. It acts as an I2C slave at address `0x08` and responds to a `requestFrom` call with a 4-byte packet (temperature integer, temperature fractional tenths, humidity integer, XOR checksum).

**Arduino 3 — Wing A LED Node (Arduino UNO R3)**
Controls five bi-color LED indicators for rooms 2432, 2434, 2436, 2438, and 2440. It listens on I2C address `0x09` for a 3-byte packet from the Master and updates LED colors accordingly. A photoresistor (LDR) on analog pin A0 allows this board to automatically adjust LED brightness based on ambient room lighting, keeping the display readable day and night.

**Arduino 4 — Wing B LED Node (Arduino UNO R3)**
Controls three bi-color LED indicators for rooms 2426, 2428, and 2430, and uses an HC-SR04 ultrasonic distance sensor to detect whether a person is standing nearby. When no one is within 150 cm, the LEDs dim to approximately 10% brightness to reduce power consumption and light pollution; they snap to full brightness the moment someone approaches.

All four boards share a single I2C bus (SDA on A4, SCL on A5), allowing the Master to address each slave independently. The Master is the only board with WiFi access; the slave boards have no network knowledge and only respond to I2C commands.

---

## 3. Final Plan for Use and Communication Between Multiple Arduinos

All four boards communicate over a shared two-wire I2C bus. The Arduino R4 WiFi (Master) acts as the bus master and initiates all communication; the three UNO boards are slaves that only respond when addressed.

### I2C Address Map

| Address | Board | Role |
|---------|-------|------|
| Master  | Arduino 1 (R4 WiFi) | Bus master — no address assigned |
| `0x08`  | Arduino 2 (Env Node) | DHT11 data source — read by Master with `requestFrom` |
| `0x09`  | Arduino 3 (Wing A) | LED controller — receives status byte from Master |
| `0x0A`  | Arduino 4 (Wing B) | LED controller — receives status byte from Master |

### Master → Wing A / Wing B (Write — 3 bytes)

Each time the Master receives new availability data from the server, it packs the status of each room into a single byte and transmits it to each Wing board using a 3-byte I2C write:

```
Byte 0: control byte = 0x01 (identifies this as a status update packet)
Byte 1: status byte  = bitmask of booked rooms (bit N = 1 means room N is booked)
Byte 2: checksum     = Byte 0 XOR Byte 1
```

Wing A uses bits 0–4 for rooms 2432–2440; Wing B uses bits 0–2 for rooms 2426–2430. The Wing boards verify the checksum before updating their LEDs to prevent corrupt packets from causing incorrect displays.

### Master → Arduino 2 (Read — 4 bytes via `requestFrom`)

The Master calls `Wire.requestFrom(0x08, 4)` every 30 seconds to retrieve the latest temperature and humidity reading. Arduino 2's I2C interrupt handler (`onRequest`) fires and transmits four bytes:

```
Byte 0: temperature integer part (e.g., 22 for 22.5 °C)
Byte 1: temperature fractional tenths (e.g., 5 for 22.5 °C)
Byte 2: relative humidity integer (e.g., 65 for 65 %)
Byte 3: checksum = Byte 0 XOR Byte 1 XOR Byte 2
```

The Master discards the reading if the checksum does not match.

### Server Communication (Master only)

The Master is the only board with network access. It uses `WiFiSSLClient` and `ArduinoHttpClient` to make HTTPS GET requests to the NAS-hosted Python server via Tailscale Funnel. The key endpoints used are:

- `/dates` — fetches a comma-separated list of upcoming bookable weekdays
- `/slot?date=YYYYMMDD&time=HHMM` — fetches an 8-character availability string (`0`=free, `1`=booked)
- `/refresh` — triggers an immediate re-poll of Google Calendar on the server
- `/newbook?date=YYYYMMDD&time=HHMM` — creates a booking session and returns a 6-character code for the mobile fallback page

All data flow is strictly one-directional for the slave boards: they receive from the Master and never initiate communication.

---

## 4. Final Project Design — Expected Inputs / Outputs

### Inputs

| Input Device | Board | Description |
|---|---|---|
| TFT Touchscreen (XPT2046) | Arduino 1 | User taps room rows to trigger QR display; taps QR screen to return |
| Push Button 1 (D4) | Arduino 1 | Cycles to the next available time slot (09:30 → 10:30 → … → 16:30 → wrap) |
| Push Button 2 (D5) | Arduino 1 | Forces an immediate re-fetch from the server |
| Push Button 3 (D6) | Arduino 1 | Cycles to the next available bookable date |
| DHT11 Sensor (D2) | Arduino 2 | Reads ambient temperature (°C) and relative humidity (%) |
| LDR Photoresistor (A0) | Arduino 3 | Reads ambient light level to adjust Wing A LED brightness |
| HC-SR04 Ultrasonic (D9/D10) | Arduino 4 | Detects proximity (≤150 cm = someone nearby) to adjust Wing B brightness |
| Google Calendar API | NAS Server | Provides real-time room booking availability data (polled every 5 minutes) |

### Outputs

| Output Device | Board | Description |
|---|---|---|
| 4" TFT LCD (480×320) | Arduino 1 | Normal view: 8 color-coded room rows with date/time/temperature header; QR view: full-screen scannable QR code |
| Passive Buzzer (D8) | Arduino 1 | Two-tone chime (880 Hz + 1047 Hz) when a booked room transitions to free |
| 5× Bi-color LEDs | Arduino 3 | Green = room free, Red = room booked; brightness auto-adjusted by LDR |
| 3× Bi-color LEDs | Arduino 4 | Same green/red logic; proximity-dimmed to 10% when no one is nearby |
| Mobile Web Page (`/b/<CODE>`) | NAS Server | Phone-accessible fallback page listing all rooms with direct Google Calendar booking links |
| Serial Monitor (debug) | All boards | Diagnostic output for development and troubleshooting |

### TFT Normal Screen Layout

```
┌─────────────────────────────────────────────────────┐
│ CDRLC Study Rooms          [21.5C  62%]  ← header  │
│ Mon 04/28  09:30           3 free                   │
├─────────────────────────────────────────────────────┤
│ Room 2432  ██████████████████████  [ Book > ]       │  ← dark green
│ Room 2434  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  [  Taken  ]       │  ← dark grey
│ Room 2436  ██████████████████████  [ Book > ]       │
│ Room 2438  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  [  Taken  ]       │
│ Room 2440  ██████████████████████  [ Book > ]       │
│ Room 2426  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  [  Taken  ]       │
│ Room 2428  ██████████████████████  [ Book > ]       │
│ Room 2430  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  [  Taken  ]       │
└─────────────────────────────────────────────────────┘
```

### TFT QR Screen Layout

```
┌─────────────────────────────────────────────────────┐
│           Room 2432  –  Scan to Book                │  ← header
│                                                     │
│              ████████████████                       │
│              █  ██  █  █  █ █                       │
│              █  ██  █  █  █ █    ← QR code          │
│              ████████████████       (scale 4×)      │
│                                                     │
│              Tap anywhere to return                 │
└─────────────────────────────────────────────────────┘
```

---

## 5. Description of Original Work

The original contribution of this project lies in the integration of multiple independently functional subsystems into a cohesive, always-on, hardware kiosk that bridges the physical world (LEDs, touch screen, sensors) with cloud-based calendar data, while keeping the booking workflow entirely on the user's personal phone.

Several aspects of the implementation required novel problem-solving that goes beyond adapting existing tutorials:

**On-device QR code generation on a microcontroller.** The Arduino R4 WiFi generates the full QR code bitmap entirely in SRAM using the `qrcode` library (ricmoo), without any server round-trip for the QR image. This required careful sizing: Google Calendar appointment URLs are approximately 143 characters long, which requires QR version 9 with ECC_LOW (maximum capacity: 154 bytes). The module size at version 9 is 53×53, which at scale factor 4 produces a 212×212 pixel image that fits comfortably on the 480×320 screen. The flash-stored URL array (`const char* const GCAL_URLS[8]`) avoids consuming SRAM, which is critical given the R4 WiFi's limited 32 KB.

**Distributed I2C sensor architecture with checksum validation.** Rather than polling the DHT11 directly on the Master (which would require blocking microsecond delays incompatible with SPI/WiFi timing), we offloaded the sensor to a dedicated Arduino and defined a simple 4-byte I2C protocol with XOR checksums. This pattern is reusable for any future sensor expansion.

**Secure public HTTPS access without a static IP or port forwarding.** The server is exposed via Tailscale Funnel, which provides TLS termination and a stable public HTTPS endpoint even behind NAT, carrier-grade NAT, or dynamic IP. The Arduino connects using `WiFiSSLClient` with no certificate pinning required (Tailscale's certificate is signed by a public CA). This is a practical and replicable pattern for any Arduino IoT project that needs cloud connectivity.

**Proximity-aware LED power management.** Wing B's HC-SR04 integration was designed to reduce power consumption and avoid light pollution when no one is near the display. The threshold (150 cm) and dim level (10%, or `analogWrite` value 25) were chosen empirically so the LEDs remain readable as status indicators while being unobtrusive during off-hours.

**Non-blocking concurrency on a single-threaded microcontroller.** The Master's `loop()` function handles button debouncing, touch polling, buzzer sequencing, periodic server fetching, and DHT11 requests concurrently using a state-machine and `millis()`-based timers, avoiding any `delay()` calls that would freeze the display or miss inputs.

---

## 6. Step-by-Step Build Guide

### Phase 1 — Hardware Assembly

#### 6.1 Gather Components

Refer to the Final Materials List at the end of this document. Acquire all components before starting assembly.

#### 6.2 Wire Arduino 1 (Master Node — Arduino R4 WiFi)

Connect the 4" TFT screen and touch controller to the R4 WiFi's SPI bus:

| Component | Component Pin | Arduino R4 WiFi Pin |
|-----------|--------------|---------------------|
| TFT | SDI / MOSI | D11 |
| TFT | SCK | D13 |
| TFT | SDO / MISO | D12 |
| TFT | CS | D10 |
| TFT | DC / RS | A0 (= digital pin 14) |
| TFT | RST | D9 |
| TFT | LED (backlight) | 3.3V |
| Touch (XPT2046) | T_CS | A1 (= digital pin 15) |
| Passive buzzer | + (signal) | D8 |
| Passive buzzer | − | GND |
| Button 1 | one leg | D4 |
| Button 1 | other leg | GND |
| Button 2 | one leg | D5 |
| Button 2 | other leg | GND |
| Button 3 | one leg | D6 |
| Button 3 | other leg | GND |
| I2C SDA | — | A4 |
| I2C SCL | — | A5 |

> **Note:** The R4 WiFi uses internal pull-up resistors for the buttons (`INPUT_PULLUP`), so no external resistors are needed on the button lines. The TFT backlight LED can also be connected to a PWM pin if adjustable brightness is desired.

#### 6.3 Wire Arduino 2 (Environment Node — UNO R3)

| Component | Pin | Arduino UNO Pin |
|-----------|-----|-----------------|
| DHT11 | VCC | 5V |
| DHT11 | GND | GND |
| DHT11 | DATA | D2 |
| Pull-up resistor | 10 kΩ between DATA and 5V | — |
| I2C SDA | — | A4 |
| I2C SCL | — | A5 |

#### 6.4 Wire Arduino 3 (Wing A — UNO R3)

Each bi-color LED is a common-cathode type. Connect the common pin through a 220 Ω resistor to GND.

| Room | Green LED pin | Red LED pin |
|------|---------------|-------------|
| 2432 | D3 (PWM) | D2 |
| 2434 | D5 (PWM) | D4 |
| 2436 | D6 (PWM) | D7 |
| 2438 | D9 (PWM) | D8 |
| 2440 | D10 (PWM) | D11 |

LDR circuit: one leg → 5V; other leg → A0 AND through 10 kΩ → GND.

I2C: A4 = SDA, A5 = SCL.

#### 6.5 Wire Arduino 4 (Wing B — UNO R3)

| Room | Green LED pin | Red LED pin |
|------|---------------|-------------|
| 2426 | D3 (PWM) | D2 |
| 2428 | D5 (PWM) | D4 |
| 2430 | D6 (PWM) | D7 |

HC-SR04: VCC → 5V, GND → GND, Trig → D9, Echo → D10.

I2C: A4 = SDA, A5 = SCL.

#### 6.6 Connect the I2C Bus

Join the A4 (SDA) and A5 (SCL) pins of all four Arduinos together on a shared bus. If wires are longer than approximately 30 cm, add 4.7 kΩ pull-up resistors from SDA to 5V and SCL to 5V. All boards share a common GND rail.

---

### Phase 2 — Software Setup

#### 6.7 Install Arduino Libraries

Open **Arduino IDE → Tools → Manage Libraries** and install each of the following:

| Library | Author | Used by |
|---------|--------|---------|
| TFT_eSPI | Bodmer | master_node |
| XPT2046_Touchscreen | Paul Stoffregen | master_node |
| qrcode | ricmoo | master_node |
| NTPClient | Fabrice Weinberg | master_node |
| ArduinoHttpClient | Arduino | master_node |
| DHT sensor library | Adafruit | ui_node |
| Adafruit Unified Sensor | Adafruit | ui_node (auto-dependency) |

#### 6.8 Configure `User_Setup.h`

TFT_eSPI looks for a `User_Setup.h` file in the sketch directory. Place the provided `User_Setup.h` in `master_node/` alongside `master_node.ino`. This file tells TFT_eSPI which driver IC (ST7796S) and which SPI pins to use. **Without this file, the library will use incorrect default pin assignments and the screen will not display anything.**

Key settings:
```cpp
#define ST7796_DRIVER
#define TFT_CS    10    // D10
#define TFT_DC    14    // A0
#define TFT_RST    9    // D9
#define TFT_MOSI  11    // D11
#define TFT_SCLK  13    // D13
#define TFT_MISO  12    // D12
#define TOUCH_CS  15    // A1
#define SPI_FREQUENCY        27000000
#define SPI_TOUCH_FREQUENCY   2500000
```

#### 6.9 Configure WiFi Credentials and Server Address

In `master_node/master_node.ino`, update these three lines near the top of the file:

```cpp
const char* SSID     = "YourSSID";                        // your network name
const char* PASSWORD = "YourPassword";                    // your network password
const char* SERVER_IP = "your-nas.tail1234ab.ts.net";     // your Tailscale hostname
```

#### 6.10 Update Google Calendar URLs

In `master_node/master_node.ino`, replace the URLs in the `GCAL_URLS` array with the actual appointment scheduling URLs for your rooms:

1. Log in to Google Calendar and navigate to each room's calendar
2. Open the Appointment Schedule feature and copy the public booking URL
3. Paste each URL into the corresponding slot in `GCAL_URLS[8]`

Each URL must be **154 bytes or shorter** to fit within QR version 9 ECC_LOW capacity.

#### 6.11 Upload Sketches to Each Board

Connect each Arduino separately via USB and upload the corresponding sketch:

| Sketch | Target Board |
|--------|-------------|
| `master_node/master_node.ino` | Arduino R4 WiFi |
| `ui_node/ui_node.ino` | Arduino UNO R3 (Env Node) |
| `wing_a.ino` | Arduino UNO R3 (Wing A) |
| `wing_b.ino` | Arduino UNO R3 (Wing B) |

Upload each board individually **before** connecting the shared I2C bus. Use the Serial Monitor at 9600 baud to verify each board reports correct startup messages.

---

### Phase 3 — Server Deployment

#### 6.12 Prepare the NAS

Copy the following server files to a directory on your NAS (e.g., `/volume1/docker/cdrlc/`):

```
room_server.py
Dockerfile
docker-compose.yml
nginx.conf
```

#### 6.13 Set Up Tailscale

Tailscale provides a stable public HTTPS URL (Funnel) without requiring port forwarding or a static IP.

1. Install Tailscale on the NAS (as a native package or Docker container)
2. Log in and note your NAS's Tailscale DNS name (e.g., `my-nas.tail1234ab.ts.net`)
3. Enable Tailscale Funnel in the admin console for your device
4. Run `tailscale funnel 8080` in the Tailscale terminal to forward public HTTPS port 443 → local port 8080

#### 6.14 Start the Docker Stack

Using the NAS Docker web UI or an SSH terminal:

```bash
cd /volume1/docker/cdrlc
docker compose up -d
```

Verify the containers started:
```bash
docker compose ps
curl http://localhost:8080/dates
curl http://localhost:8080/status
```

#### 6.15 Test Public Access

```bash
curl https://your-nas.tail1234ab.ts.net/dates
```

This should return a comma-separated list of bookable dates.

---

### Phase 4 — Calibration and Testing

#### 6.16 Touch Screen Calibration

The raw ADC values from the XPT2046 touch controller vary between individual panels. After first upload:

1. In Arduino IDE, open `File → Examples → TFT_eSPI → Generic → Touch_calibrate`
2. Upload this example to the R4 WiFi
3. Follow on-screen instructions (tap four crosshairs at screen corners)
4. Read the four values printed to Serial: `TS_MINX`, `TS_MAXX`, `TS_MINY`, `TS_MAXY`
5. Update these four `#define` lines in `master_node.ino`:
   ```cpp
   #define TS_MINX  ????
   #define TS_MAXX  ????
   #define TS_MINY  ????
   #define TS_MAXY  ????
   ```
6. Re-upload `master_node.ino` with the calibration values

#### 6.17 End-to-End System Test

With all four boards powered and the I2C bus connected:

1. Verify the TFT shows the CDRLC splash screen on power-up
2. Verify WiFi connection and the room availability list appears
3. Verify temperature/humidity appears in the header
4. Verify LEDs on Wing A and Wing B reflect the correct green/red status
5. Cover the LDR on Wing A — verify brightness changes
6. Move toward Wing B within 150 cm — verify LEDs brighten
7. Tap a green room row on the TFT — verify QR code appears
8. Scan the QR with a phone — verify the Google Calendar booking page opens
9. Tap anywhere on the QR screen — verify the room list returns
10. Press BTN1 (slot cycle), BTN3 (date cycle), BTN2 (manual refresh) — verify each works

---

## 7. User Guide

### 7.1 Normal Operation

On power-up, the display shows a splash screen, connects to WiFi, and then loads the room availability list for tomorrow's first available slot. No user interaction is needed for the system to operate — it auto-refreshes every five minutes.

#### What the Screen Shows

The TFT display is divided into a header bar and eight room rows:

- **Header (top, dark blue):** Shows the title "CDRLC Study Rooms", the current temperature and humidity from the DHT11 sensor in the top-right corner, the selected date and time slot on the left, and the number of currently free rooms on the right.

- **Room rows (8 rows below header):** Each row corresponds to one study room. A **dark green row** with a white "Book >" button means the room is available for the displayed time slot. A **dark grey row** with a muted "Taken" label means the room is already booked.

#### What the LEDs Show

- **Green LED lit:** The room is available for the currently displayed time slot.
- **Red LED lit:** The room is booked.
- **Dim LEDs (Wing B):** No one has been detected nearby in the past 300 ms. LEDs automatically brighten when someone approaches within 150 cm.

#### What the Buzzer Does

A two-tone chime (ascending: 880 Hz then 1047 Hz) sounds whenever the system detects that a room has transitioned from booked to available. This can happen after an auto-refresh or a manual refresh.

---

### 7.2 Browsing Rooms

**To change the time slot:** Press **Button 1** (labeled "SLOT" or leftmost button on the Master board). The system cycles through 8 one-hour slots: 09:30, 10:30, 11:30, 12:30, 13:30, 14:30, 15:30, 16:30. After 16:30, it wraps back to 09:30. Each press fetches new data from the server.

**To change the date:** Press **Button 3** (rightmost button on the Master board). The system cycles through the next available bookable weekdays (up to 5 days ahead). Each press fetches new data.

**To force a refresh:** Press **Button 2** (middle button). The header briefly shows "Refreshing…" and the system calls the server's `/refresh` endpoint to trigger an immediate Google Calendar re-poll, then waits 3 seconds and reloads the display.

---

### 7.3 Booking a Room (QR Code Flow)

1. Identify a **green** room row on the TFT display.
2. **Tap the row** anywhere on the screen within that row's height.
3. The TFT switches to a full-screen QR code displaying the room number and the instruction "Scan to Book."
4. **Open the camera app** on your phone and point it at the QR code.
5. A link notification appears — tap it to open Google Calendar's appointment scheduling page for that specific room.
6. On Google Calendar, select your desired time slot and complete the booking. No kiosk login is required; you book directly through your own Google account.
7. **Tap anywhere** on the TFT screen to return to the room availability list.

> **Note:** The QR code encodes a direct Google Calendar appointment scheduling URL. The booking is completed entirely on Google's website. The kiosk system does not store any personal data, email addresses, or booking confirmations.

---

### 7.4 Mobile Fallback Page

If the TFT kiosk is unavailable, the system provides a mobile web page accessible from any phone on any network. Navigate to:

```
https://your-nas.tail1234ab.ts.net/b/<CODE>
```

Where `<CODE>` is a 6-character session code generated by the server. This page lists all 8 rooms with their current booking status and direct links to each room's Google Calendar scheduling page.

---

### 7.5 Input/Output Device Summary

| Device | Type | Location | User Action / Information Provided |
|--------|------|----------|-------------------------------------|
| TFT Touch Screen (480×320) | Input + Output | Arduino 1 | Tap rows to view QR; displays room list, QR codes, date/time, temperature |
| Button 1 | Input | Arduino 1 (D4) | Press to cycle to next time slot |
| Button 2 | Input | Arduino 1 (D5) | Press to force a data refresh from server |
| Button 3 | Input | Arduino 1 (D6) | Press to cycle to next date |
| Passive Buzzer | Output | Arduino 1 (D8) | Chimes when a room becomes available |
| DHT11 Sensor | Input (ambient) | Arduino 2 (D2) | Measures room temperature and humidity (shown on TFT header) |
| 5× Bi-color LEDs | Output | Arduino 3 | Green = free, Red = booked; auto-brightness via LDR |
| LDR | Input (ambient) | Arduino 3 (A0) | Senses ambient light; adjusts Wing A LED brightness automatically |
| 3× Bi-color LEDs | Output | Arduino 4 | Green = free, Red = booked; dims when no one nearby |
| HC-SR04 | Input (proximity) | Arduino 4 (D9/D10) | Detects person within 150 cm; dims Wing B LEDs when unoccupied |

---

## Design Decisions — Ideas Considered and Rejected

This section documents the significant design ideas explored during development, the reasoning behind each decision, and why some approaches were ultimately rejected in favor of the final design. Understanding these trade-offs is essential context for anyone seeking to replicate or extend the system.

---

### Idea 1: Generate QR Codes on the Server and Send Bitmap to OLED

**What we considered:** The original display plan used a 128×64 px SSD1306 OLED as the secondary display and a 16×2 LCD as the primary room list display. The idea was to have the server generate QR bitmaps (via the Python `qrcode` library) and serve them to the Arduino as raw pixel data, which would then be rendered on the OLED.

**Why it was rejected:** The OLED's 128×64 pixel resolution is severely insufficient for a QR code. A QR code at even version 1 (smallest) is 21×21 modules. At 128×64, the largest usable scale factor is 3 (63×63 px visible area), giving only a 21×21 module display at scale 3 — barely scannable and not robust. More critically, the system had no UI mechanism for the user to *select* which room they wanted to generate a QR code for. The Arduino had no user input path to communicate a room selection to the server, so the QR generation concept was premature. The server endpoint (`/qrbitmap`) was built before the input design was finalized, making it an orphaned feature.

**What we did instead:** We replaced the OLED+LCD combination with a single 4" 480×320 TFT touch screen. The touch interface solves the room selection problem — the user taps a row on the room list to indicate which room they want. The QR code is then generated entirely on-device by the Arduino using the `qrcode` (ricmoo) library, eliminating the need for a server round-trip for every QR request. The 212×212 px rendered QR (version 9, scale 4) is far more reliable to scan than anything that would fit on a 128×64 OLED.

---

### Idea 2: Fully Automated Room Booking via Email Forwarding

**What we considered:** An early ambitious idea was to allow the kiosk to fully automate the booking process. The user would type or select their name/email on the kiosk, the system would fill out a Google Form booking request on their behalf, and a confirmation would be emailed to them. The Arduino or server would handle the form submission in the background.

**Why it was rejected:** Google Forms (and Google Calendar appointment scheduling) includes CAPTCHA verification and bot-detection mechanisms on form submissions. An automated HTTP POST to the form URL is immediately flagged and rejected. Even if we handled the visible form fields correctly, Google's invisible reCAPTCHA system blocks non-browser submissions. Implementing a headless browser on a NAS purely for form automation was deemed out of scope and fragile. Additionally, storing user email addresses introduces privacy concerns (FERPA compliance) that are out of scope for a hardware project.

**What we did instead:** The booking workflow was redesigned so that the *user's own phone* handles the entire booking interaction through their own Google account. The kiosk only generates a QR code encoding the room's public appointment scheduling URL. The user opens it on their phone, selects a time, and books it with their own Google credentials. No user data ever passes through the kiosk system.

---

### Idea 3: Display Only Today's Room Availability

**What we considered:** To keep the UI simple, an early design showed only the current day's room availability for the current time slot. The assumption was that users only care about booking a room right now.

**Why it was rejected:** After reviewing how Google Calendar appointment scheduling actually works for university study rooms, we realized that **same-day time slots are almost always pre-booked or unavailable** for registration by the time a student is standing at the kiosk. Rooms are typically bookable up to 1–2 weeks in advance, and the most in-demand slots are claimed days ahead. Showing only today's data would result in the display showing nearly all rooms as "Taken" almost all the time, which is visually useless and actively discouraging. A display that always shows red provides no actionable information to the user.

**What we did instead:** The system fetches the next 5 available bookable weekdays from the server via `/dates`. The default display shows *tomorrow* (index 1 in the date list) rather than today. Button 3 lets users cycle through future dates, and Button 1 lets them cycle through time slots, so they can find and plan an upcoming booking. This makes the display genuinely useful for planning rather than just confirming what they already know (that today is full).

---

### Idea 4: Using an OLED + LCD Combination as the Primary Display

**What we considered:** The initial multi-display plan assigned the 128×64 OLED to show QR codes or booking confirmation, and a 16×2 character LCD (LiquidCrystal, parallel interface) on Arduino 2 (then called the "UI node") to show room status using scrolling text.

**Why it was rejected:** The 16×2 LCD can only display 32 characters at a time. With 8 rooms to show, the system needed to scroll through them, which felt clunky and made it impossible to see all rooms simultaneously. The OLED-for-QR problem (see Idea 1) compounded this. Additionally, having Arduino 2 responsible for both DHT11 sensing and driving a parallel LCD with room data (received via I2C from the master) created a complex state machine with many failure modes. The LCD wiring also required ~6 digital pins for the parallel bus, which conflicted with I2C pins on some configurations. The combination required two separate boards worth of complexity to do something one TFT could do better.

**What we did instead:** We dropped both the OLED and the LCD and replaced them with a single 4" ST7796S TFT on the SPI bus of the Master board. The TFT eliminated pin conflicts (SPI uses dedicated hardware pins), provided a much larger canvas for information display, and enabled touch input. Arduino 2 was simplified to only read the DHT11 and respond to I2C requests — a clean, single-purpose design.

---

### Idea 5: Removing the LED Wing Boards to Reduce Complexity

**What we considered:** Once the TFT touch screen was adopted as the primary display, we briefly considered eliminating the two LED wing boards entirely, since the TFT already shows green/grey room rows. Four boards would become two, simplifying assembly, wiring, and I2C coordination.

**Why it was rejected:** The project specification requires exactly four Arduino boards. Beyond the formal requirement, the LED wings serve a genuinely different UX purpose: they are visible from a distance, in low-light conditions, and peripherally (without looking directly at the screen). A student walking down the hallway can glance at the LED panel and instantly know whether any room is free without approaching the kiosk. The TFT requires being within ~1 meter and oriented correctly. The two output modalities (TFT for detailed booking, LEDs for at-a-glance status) complement each other rather than duplicate.

**What we did instead:** We kept all four boards and retained the LED wings. We added value to the wings by implementing LDR-based auto-brightness (Wing A) and proximity-based power saving via HC-SR04 (Wing B), turning them from simple LED panels into adaptive, context-aware output devices.

---

### Idea 6: Using a Static IP or Port Forwarding for the Server

**What we considered:** The obvious way to make the NAS server accessible from the Arduino over the internet is to configure port forwarding on the home/university router and access it by the NAS's public IP address. Some setups also considered using a free DDNS service (like No-IP or DuckDNS) to handle dynamic IP changes.

**Why it was rejected:** Port forwarding requires router admin access, which is not available on university networks. Even on home networks, many ISPs block inbound connections on common ports, and some use carrier-grade NAT (CGNAT) that makes direct inbound connections impossible. DDNS services add another external dependency with uptime and reliability concerns. Most critically, plain HTTP would expose the API to anyone, and Arduino's SSL support requires a certificate signed by a known CA — self-signed certificates require custom trust store management that the `WiFiSSLClient` on the R4 WiFi does not support easily.

**What we did instead:** We used Tailscale Funnel, which creates a stable public HTTPS URL with a valid certificate (from Let's Encrypt via Tailscale's domain) without any router configuration or static IP. The NAS's Funnel URL (`zz-cloud.tail6b9dfa.ts.net`) is stable, uses TLS 1.3, and the certificate is trusted by the Arduino's SSL stack out of the box. nginx sits behind the Funnel as a reverse proxy to add rate limiting (60 req/min per IP) and route whitelisting.

---

## Supporting Materials

### Timeline of Development

| Week | Milestone / Completed Items |
|------|----------------------------|
| Week 1 | Project concept finalized; identified 8 CDRLC study rooms; chose Arduino R4 WiFi as Master; confirmed Google Calendar public API usability |
| Week 2 | Established I2C multi-board architecture; defined slave addresses (0x08, 0x09, 0x0A); first I2C ping tests with `i2c_ping_master/slave` sketches |
| Week 3 | Initial server prototype (`room_server.py`) with Google Calendar API polling; `/dates` and `/slot` endpoints working; tested from laptop |
| Week 4 | Arduino 1 WiFi + HTTPS client working; Master can fetch `/slot` and parse availability string; bi-color LED basic control on Wing boards |
| Week 5 | OLED + LCD display design explored; 128×64 QR code generation attempted and rejected (too small, no room selection UI); LCD scrolling room list implemented and later rejected |
| Week 6 | TFT touch screen design adopted; `User_Setup.h` configured for ST7796S; TFT rendering of room list working; touch input mapped to rows |
| Week 7 | On-device QR code generation via `qrcode` library; version 9 ECC_LOW selected; GCAL_URLS array moved to flash; QR scan tested on phone |
| Week 8 | DHT11 moved from Master to Arduino 2 (I2C slave protocol); 4-byte packet with XOR checksum; temperature/humidity shown in TFT header |
| Week 9 | Wing A LDR auto-brightness; Wing B HC-SR04 proximity dimming; non-blocking buzzer state machine; all three buttons debounced |
| Week 10 | Server deployed on NAS via Docker Compose; Tailscale Funnel configured; nginx rate limiting and route whitelist; public HTTPS URL tested from Arduino |
| Week 11 | Final integration test; touch calibration; end-to-end booking flow verified; mobile fallback `/b/<CODE>` page tested; README and final documentation |

---

### Final List of Materials

| Component | Quantity | Notes |
|-----------|----------|-------|
| Arduino R4 WiFi | 1 | Master Node; built-in 2.4 GHz WiFi |
| Arduino UNO R3 | 3 | Env Node, Wing A, Wing B |
| Hosyond 4.0" 480×320 TFT LCD | 1 | ST7796S driver + XPT2046 touch, SPI interface |
| DHT11 temperature & humidity sensor | 1 | Env Node (Arduino 2) |
| Bi-color LED (red/green, common cathode) | 8 | 5 for Wing A, 3 for Wing B |
| 220 Ω resistor | 8 | LED current limiting (one per LED) |
| 10 kΩ resistor | 3 | 1× DHT11 pull-up, 1× LDR voltage divider, 1× spare |
| Passive buzzer | 1 | Master Node (D8) |
| Momentary push button | 3 | Master Node (D4, D5, D6) |
| LDR (photoresistor) | 1 | Wing A (A0) |
| HC-SR04 ultrasonic sensor | 1 | Wing B (D9, D10) |
| Breadboard(s) | 4 | One per Arduino (or perfboard for final build) |
| Jumper wires (M-M, M-F) | ~60 | For I2C bus, LED wiring, sensors |
| USB-A to USB-B cable | 3 | For UNO R3 power + programming |
| USB-A to USB-C cable | 1 | For R4 WiFi power + programming |
| NAS or always-on Linux machine | 1 | For Docker server (Ugreen DH2300 used in this project) |
| 4.7 kΩ resistor | 2 | I2C pull-ups (if bus > 30 cm) |

---

### Hardware Diagram

*(To be completed — insert Fritzing or equivalent wiring diagram showing all four Arduinos, the TFT screen, LEDs, DHT11, LDR, HC-SR04, buzzer, buttons, and I2C bus connections. Use the wiring tables in Section 6 as reference.)*

---

### Final Code Sketches

*(To be filled in by team — include the complete source code for all four Arduino sketches:)*

1. `master_node/master_node.ino` — Arduino 1 (Master Node)
2. `master_node/User_Setup.h` — TFT_eSPI pin configuration
3. `ui_node/ui_node.ino` — Arduino 2 (Environment Sensor Node)
4. `wing_a.ino` — Arduino 3 (Wing A LED Node)
5. `wing_b.ino` — Arduino 4 (Wing B LED Node)

*(All code is available in the project repository. Insert printed or copy-pasted code here.)*

---

### Final List of References

1. **Arduino R4 WiFi Documentation**
   Arduino. *Arduino UNO R4 WiFi Reference.* https://docs.arduino.cc/hardware/uno-r4-wifi/

2. **TFT_eSPI Library**
   Bodmer. *TFT_eSPI — Arduino and PlatformIO IDE compatible TFT library.* GitHub. https://github.com/Bodmer/TFT_eSPI

3. **XPT2046 Touchscreen Library**
   Paul Stoffregen. *XPT2046_Touchscreen.* GitHub. https://github.com/PaulStoffregen/XPT2046_Touchscreen

4. **qrcode Library (ricmoo)**
   ricmoo. *QRCode — A pure-C QR code generation library.* GitHub. https://github.com/ricmoo/QRCode

5. **NTPClient Library**
   Fabrice Weinberg. *NTPClient — Connect to a time server.* GitHub. https://github.com/arduino-libraries/NTPClient

6. **ArduinoHttpClient Library**
   Arduino. *ArduinoHttpClient.* GitHub. https://github.com/arduino-libraries/ArduinoHttpClient

7. **DHT Sensor Library**
   Adafruit. *DHT sensor library for Arduino.* GitHub. https://github.com/adafruit/DHT-sensor-library

8. **Google Calendar API — Appointment Scheduling**
   Google. *Google Calendar API Reference.* https://developers.google.com/calendar/api

9. **Tailscale Funnel Documentation**
   Tailscale. *Tailscale Funnel — Expose local services to the internet.* https://tailscale.com/kb/1223/funnel/

10. **QR Code Capacity Reference**
    Thonky. *QR Code Tutorial — Data Capacity.* https://www.thonky.com/qr-code-tutorial/data-encoding

11. **ST7796S TFT Datasheet**
    Sitronix. *ST7796S TFT-LCD Single Chip Driver Datasheet.* (Available from display module vendor)

12. **HC-SR04 Ultrasonic Sensor Datasheet**
    Generic. *HC-SR04 Product Datasheet.* https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf

13. **DHT11 Datasheet**
    D-Robotics. *DHT11 Humidity & Temperature Sensor Datasheet.* https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf

14. **Docker Documentation**
    Docker Inc. *Docker Compose Reference.* https://docs.docker.com/compose/

15. **nginx Rate Limiting Guide**
    nginx. *Rate Limiting with nginx.* https://www.nginx.com/blog/rate-limiting-nginx/
