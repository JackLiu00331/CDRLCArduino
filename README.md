# CDRLC Study Room Availability Monitor

A real-time study room availability system for a university library (CDRLC). Four Arduino boards work together to display live Google Calendar room status on a 4" TFT touch screen and bi-color LED panels, and let users initiate a room booking by simply tapping the screen.

---

## Features

| Feature | Description |
|---------|-------------|
| **Real-time availability** | Polls Google Calendar's appointment API every 5 minutes |
| **TFT touch display** | 480×320 colour screen: green rows = free, grey = booked |
| **One-tap QR booking** | Tap a free room → QR code appears on-screen → scan with phone → Google Calendar booking page |
| **Bi-color LED panels** | Hardware green/red LEDs for at-a-glance status from a distance |
| **Proximity auto-dim** | Wing B LEDs dim to 10% when no one is nearby (HC-SR04) |
| **Ambient auto-brightness** | Wing A LED brightness follows room light level (LDR) |
| **Buzzer alert** | Two-tone beep when a room transitions from booked → free |
| **Temperature & humidity** | DHT11 reading shown live in the TFT header |
| **Multi-date browsing** | Button cycles through available weekdays |
| **Multi-slot browsing** | Button cycles through 8 one-hour slots (09:30–16:30) |
| **Manual refresh** | Button triggers an immediate server re-fetch |
| **Emergency mobile page** | `/b/<CODE>` URL shows all rooms with booking links on any phone |
| **Rate-limited HTTPS** | nginx caps requests at 60/min per IP; Tailscale Funnel provides TLS |

---

## Hardware Components

### Per board

| Board | Model | Key hardware |
|-------|-------|-------------|
| Arduino 1 – Master | Arduino R4 WiFi | 4" TFT (ST7796S), XPT2046 touch, passive buzzer, 3× push buttons |
| Arduino 2 – Env Node | Arduino UNO R3 | DHT11 temperature & humidity sensor |
| Arduino 3 – Wing A | Arduino UNO R3 | 5× bi-color LEDs (rooms 2432–2440), LDR |
| Arduino 4 – Wing B | Arduino UNO R3 | 3× bi-color LEDs (rooms 2426–2430), HC-SR04 ultrasonic |

### Full component list

- Arduino R4 WiFi × 1
- Arduino UNO R3 × 3
- Hosyond 4.0" 480×320 TFT LCD (ST7796S driver, SPI, includes XPT2046 touch) × 1
- DHT11 temperature & humidity sensor × 1
- Bi-color (red/green common-cathode) LED × 8
- 220 Ω resistors × 8 (LED current limiting)
- 10 kΩ resistor × 1 (DHT11 data pull-up)
- Passive buzzer × 1
- Momentary push button × 3
- LDR (light-dependent resistor) × 1
- 10 kΩ resistor × 1 (LDR voltage divider)
- HC-SR04 ultrasonic distance sensor × 1
- NAS or always-on Linux machine (e.g. Ugreen DH2300)
- I2C pull-up resistors (4.7 kΩ on SDA & SCL if bus is long)

---

## System Architecture

```
┌─────────────────────────────────────────────┐
│            Google Calendar API               │
│    (public read-only, no login needed)       │
└───────────────────┬─────────────────────────┘
                    │ HTTPS
                    ▼
┌─────────────────────────────────────────────┐
│              NAS  (Docker)                  │
│  ┌──────────────────────────────────────┐   │
│  │  Tailscale container  (network: host)│   │
│  │  Funnel: :443 → 127.0.0.1:8080      │   │
│  └──────────────┬───────────────────────┘   │
│                 │                            │
│  ┌──────────────▼───────────────────────┐   │
│  │  nginx container  (network: host)    │   │
│  │  :8080  rate-limit + route filter    │   │
│  └──────────────┬───────────────────────┘   │
│                 │ Docker bridge              │
│  ┌──────────────▼───────────────────────┐   │
│  │  room-server container               │   │
│  │  Python HTTP server  :8080           │   │
│  └──────────────────────────────────────┘   │
└───────────────────┬─────────────────────────┘
                    │ HTTPS :443 (Tailscale Funnel)
                    ▼
┌─────────────────────────────────────────────┐
│      Arduino 1 – Master Node (R4 WiFi)      │
│                                             │
│  WiFiSSLClient ──► GET /slot, /dates ...    │
│                                             │
│  TFT (SPI)  ◄── drawNormalScreen()         │
│  Touch (SPI) ──► handleTouch()             │
│  Buzzer ──► tone()                         │
│                                             │
│  I2C Master ────────────────────────────── │
│     ├─ 0x08  requestFrom ──► Arduino 2     │
│     ├─ 0x09  sendI2C    ──► Arduino 3      │
│     └─ 0x0A  sendI2C    ──► Arduino 4      │
└─────────────────────────────────────────────┘
         │                │              │
         ▼                ▼              ▼
  Arduino 2          Arduino 3      Arduino 4
  DHT11 sensor       Wing A LEDs    Wing B LEDs
  (0x08)             (0x09)         (0x0A)
                     2432–2440      2426–2430
                     LDR auto-dim   HC-SR04 proximity
```

---

## Arduino Boards

### Board 1 – Master Node  (`master_node/`)

**Hardware:** Arduino R4 WiFi

The brain of the system. Connects to the NAS server over HTTPS (via Tailscale Funnel), polls room availability, and coordinates all other boards.

**TFT normal screen layout (480×320):**
```
┌─────────────────────────────────────────────────────┐
│ CDRLC Study Rooms          [21.5C  62%]  ← header  │
│ Mon 04/28  09:30           3 free                   │
├─────────────────────────────────────────────────────┤
│ Room 2432  ████████████████████████  [ Book > ]     │ ← green
│ Room 2434  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  [  Taken  ]    │ ← grey
│ Room 2436  ████████████████████████  [ Book > ]     │
│   ...                                               │
└─────────────────────────────────────────────────────┘
```

**TFT QR screen:** Tap a green room row → full-screen QR code → user scans with phone → Google Calendar booking page.

**Buttons:**

| Button | Pin | Action |
|--------|-----|--------|
| BTN1 | D4 | Cycle to next time slot (09:30 → 10:30 → … → 16:30 → wrap) |
| BTN2 | D5 | Force server data refresh (calls `/refresh`, waits 3 s, re-fetches) |
| BTN3 | D6 | Cycle to next available date |

---

### Board 2 – Environment Node  (`ui_node/`)

**Hardware:** Arduino UNO R3

Simple I2C slave. Reads the DHT11 sensor every 2 seconds and responds to Master's `requestFrom(0x08, 4)` with a 4-byte packet:

```
byte 0: temperature integer   (e.g. 22 for 22.5 °C)
byte 1: temperature fractional tenths  (e.g. 5)
byte 2: humidity integer %    (e.g. 65)
byte 3: checksum = b0 ^ b1 ^ b2
```

---

### Board 3 – Wing A LED Node  (`wing_a.ino`)

**Hardware:** Arduino UNO R3

Controls **5 bi-color LEDs** for rooms 2432, 2434, 2436, 2438, 2440.

- Receives an I2C 3-byte packet `[0x01, statusByte, checksum]` from Master
- `statusByte` bits 0–4 map to rooms 2432–2440 (1 = booked, 0 = free)
- **Green** = available, **Red** = booked
- Reads an LDR on A0 to auto-adjust LED brightness to ambient light level

---

### Board 4 – Wing B LED Node  (`wing_b.ino`)

**Hardware:** Arduino UNO R3

Controls **3 bi-color LEDs** for rooms 2426, 2428, 2430.

- Same I2C protocol as Wing A (bits 0–2 of statusByte)
- HC-SR04 ultrasonic sensor on D9/D10: dims LEDs to 10% when no one is within 150 cm, full brightness when someone approaches

---

## Code Files

### Arduino

| File | Description |
|------|-------------|
| `master_node/master_node.ino` | Main controller: WiFi, TFT, touch, QR generation, I2C master, buttons, buzzer |
| `master_node/User_Setup.h` | TFT_eSPI pin & driver configuration for ST7796S on R4 WiFi |
| `ui_node/ui_node.ino` | Arduino 2: DHT11 I2C slave, responds to requestFrom with 4-byte temp/humi packet |
| `wing_a.ino` | Arduino 3: Wing A bi-color LED controller with LDR auto-brightness |
| `wing_b.ino` | Arduino 4: Wing B bi-color LED controller with HC-SR04 proximity dimming |

### Server

| File | Description |
|------|-------------|
| `room_server.py` | Python HTTP server – polls Google Calendar API, serves availability data, manages booking sessions |
| `Dockerfile` | Builds the room-server Docker image (Python 3.11 slim) |
| `docker-compose.yml` | Orchestrates three containers: Tailscale, nginx, room-server |
| `nginx.conf` | Reverse proxy with 60 req/min rate limiting and route whitelist |
| `tailscale-serve.json` | Tailscale Funnel config – exposes port 443 publicly via HTTPS |

---

## Server API Endpoints

All endpoints are HTTPS via Tailscale Funnel. Nginx enforces a 60 req/min rate limit per IP.

| Endpoint | Used by | Description |
|----------|---------|-------------|
| `GET /dates` | Arduino | Comma-separated valid weekdays, e.g. `20260428,20260429` |
| `GET /slot?date=YYYYMMDD&time=HHMM` | Arduino | 8-char status string: `0`=free, `1`=booked, e.g. `10010100` |
| `GET /hotslots?date=YYYYMMDD` | Arduino | Most-booked time label(s), e.g. `1030,1130` |
| `GET /refresh` | Arduino (BTN2) | Triggers an immediate background re-fetch of all rooms |
| `GET /status` | Debug | Human-readable full availability table |
| `GET /newbook?date=YYYYMMDD&time=HHMM` | Arduino | Creates a 10-min booking session, returns 6-char code |
| `GET /b/<CODE>` | Phone (QR sticker / fallback) | Mobile page listing all rooms with Google Calendar links |

---

## How It Works

### User Flow

```
1. Walk up to the display
2. TFT shows 8 colour-coded room rows for today's next slot
3. Green row = available  →  grey row = booked
4. Tap any green "Book >" row
5. TFT switches to full-screen QR code for that room
6. Scan the QR code with your phone
7. Google Calendar appointment page opens  →  pick a time and book
8. Tap anywhere on the TFT to go back to the room list
```

### Button Flow

```
BTN1: cycle time slot   → fetches new /slot data → redraws TFT + Wing LEDs
BTN2: force refresh     → calls /refresh → waits 3 s → fetchAndDisplay()
BTN3: cycle date        → fetches new /slot data → redraws TFT + Wing LEDs
```

### System Data Flow (every 5 minutes)

```
Master ──GET /slot──► server
server ──"10010100"──► Master
Master: parse 8 chars → update roomFree[] array
      → sendI2C(Wing A, statusA)
      → sendI2C(Wing B, statusB)
      → buzzerTrigger() if any room became free
      → drawNormalScreen() on TFT
Wing A/B: receive I2C → set green/red LEDs accordingly
```

### QR Booking Flow (on touch)

```
User taps free row (row index = i)
  → qrcode_initText(GCAL_URLS[i], version=9, ECC_LOW)
  → draw white background rect + black modules on TFT
  → user scans QR with phone
  → opens Google Calendar appointment page for room i
  → user picks time slot and books directly on Google
  → tap TFT → dispState = DISP_NORMAL → drawNormalScreen()
```

No server involvement in the QR flow; the Arduino generates the QR code on-device using the `qrcode` library.

---

## NAS Server Setup

The server runs as three Docker containers on a NAS or any always-on Linux machine.

### Container topology

```
tailscale  (network: host)
  – exposes the device to your Tailscale network
  – Funnel forwards public HTTPS :443 → localhost:8080

nginx  (network: host)
  – listens on :8080
  – rate-limits: 60 req/min per IP
  – whitelists only known routes, rejects everything else
  – proxies to room-server via Docker bridge IP

room-server  (internal Docker bridge, port 8080)
  – Python HTTP server
  – polls Google Calendar API every 5 minutes
  – serves availability data to Arduino
  – hosts mobile booking page /b/<CODE>
```

### Environment variables

| Variable | Container | Default | Description |
|----------|-----------|---------|-------------|
| `TZ` | room-server | `America/Chicago` | Timezone for date calculations |
| `BOOK_BASE_URL` | room-server | `http://localhost:8080` | Public URL (optional, used only for /b/ page link generation) |
| `TS_AUTHKEY` | tailscale | — | Your Tailscale auth key (one-time setup) |
| `TS_SERVE_CONFIG` | tailscale | `/tailscale-serve.json` | Path to Funnel config inside the container |

---

## Step-by-Step Replication Guide

### Step 1 – Hardware preparation

1. Gather all components from the hardware list above
2. For each Arduino UNO R3, solder headers if not pre-soldered

**Arduino 1 (Master) wiring:**

| Component | Component Pin | Arduino R4 Pin |
|-----------|--------------|----------------|
| TFT | SDI/MOSI | D11 |
| TFT | SCK | D13 |
| TFT | SDO/MISO | D12 |
| TFT | CS | D10 |
| TFT | DC/RS | A0 |
| TFT | RST | D9 |
| TFT | LED | 3.3V (or PWM pin for dimming) |
| Touch | T_CS | A1 |
| Buzzer | + | D8 |
| Button 1 | one leg | D4, other leg → GND |
| Button 2 | one leg | D5, other leg → GND |
| Button 3 | one leg | D6, other leg → GND |
| I2C SDA | — | A4 |
| I2C SCL | — | A5 |

**Arduino 2 (Env Node) wiring:**

| Component | Pin | Arduino UNO Pin |
|-----------|-----|-----------------|
| DHT11 | VCC | 5V |
| DHT11 | GND | GND |
| DHT11 | DATA | D2 (+ 10 kΩ pull-up to 5V) |
| I2C SDA | — | A4 |
| I2C SCL | — | A5 |

**Arduino 3 (Wing A) wiring – 5 bi-color LEDs:**

Each LED: long pin (common cathode) → 220 Ω → GND

| Room | Green pin | Red pin |
|------|-----------|---------|
| 2432 | D3 (PWM) | D2 |
| 2434 | D5 (PWM) | D4 |
| 2436 | D6 (PWM) | D7 |
| 2438 | D9 (PWM) | D8 |
| 2440 | D10 (PWM)| D11 |

LDR: one end → 5V, other end → A0 and → 10 kΩ → GND

**Arduino 4 (Wing B) wiring – 3 bi-color LEDs + HC-SR04:**

| Room | Green pin | Red pin |
|------|-----------|---------|
| 2426 | D3 (PWM) | D2 |
| 2428 | D5 (PWM) | D4 |
| 2430 | D6 (PWM) | D7 |

HC-SR04: VCC → 5V, GND → GND, Trig → D9, Echo → D10

**I2C bus:** Connect A4+A5 of all four Arduinos together (shared bus). Add 4.7 kΩ pull-up resistors from SDA→5V and SCL→5V if your bus wires are longer than ~30 cm.

---

### Step 2 – Install Arduino libraries

Open **Arduino IDE → Tools → Manage Libraries** and install:

| Library | Author | Required by |
|---------|--------|-------------|
| TFT_eSPI | Bodmer | master_node |
| XPT2046_Touchscreen | Paul Stoffregen | master_node |
| qrcode | ricmoo | master_node |
| NTPClient | Fabrice Weinberg | master_node |
| ArduinoHttpClient | Arduino | master_node |
| DHT sensor library | Adafruit | ui_node |
| Adafruit Unified Sensor | Adafruit | ui_node (dependency) |

---

### Step 3 – Configure Arduino code

**`master_node/master_node.ino`** – change these lines near the top:

```cpp
const char* SSID        = "YourSSID";           // your WiFi network name
const char* PASSWORD    = "YourPassword";        // your WiFi password
const char* SERVER_IP   = "YOUR-NAS.ts.net";    // Tailscale hostname (see Step 5)
```

**`master_node/User_Setup.h`** – if colours look wrong after first flash, change:
```cpp
#define TFT_RGB_ORDER TFT_RGB   // → try TFT_BGR
```

**GCAL_URLS in `master_node.ino`** – replace with your own room booking URLs:
1. Open Google Calendar
2. Go to your room's Appointment Scheduling page
3. Copy the URL from the browser address bar (format: `https://calendar.google.com/calendar/appointments/schedules/AcZss...`)
4. Replace the corresponding entry in the `GCAL_URLS` array

---

### Step 4 – Upload Arduino sketches

Upload each sketch to its board:

| Sketch | Target board |
|--------|-------------|
| `master_node/master_node.ino` | Arduino R4 WiFi |
| `ui_node/ui_node.ino` | Arduino UNO R3 (address 0x08) |
| `wing_a.ino` | Arduino UNO R3 (address 0x09) |
| `wing_b.ino` | Arduino UNO R3 (address 0x0A) |

> **Note:** Upload each board individually before connecting the I2C bus, so you can verify Serial output for each one.

---

### Step 5 – NAS / Docker setup

#### 5a. Install Docker on your NAS

Follow your NAS manufacturer's guide to enable Docker (or install Docker Engine on any Linux machine).

#### 5b. Set up Tailscale

1. Install the Tailscale app on your NAS or use the Docker container in `docker-compose.yml`
2. Obtain an auth key from [https://login.tailscale.com/admin/settings/keys](https://login.tailscale.com/admin/settings/keys)
3. Put it in `docker-compose.yml` under `TS_AUTHKEY`
4. Note your device's Tailscale hostname (e.g. `dh2300.tail1234ab.ts.net`)
5. Enable Funnel in the Tailscale admin console for your device

#### 5c. Copy project files to NAS

```bash
scp room_server.py Dockerfile docker-compose.yml nginx.conf \
    tailscale-serve.json schedule.json  user@your-nas:/opt/cdrlc/
```

#### 5d. Deploy containers

```bash
cd /opt/cdrlc
docker compose up -d
```

Verify everything is running:

```bash
docker compose ps
curl http://localhost:8080/dates      # should return date string
curl http://localhost:8080/status     # full availability table
```

#### 5e. Test public access

```bash
curl https://YOUR-NAS.ts.net/dates
```

#### 5f. Update Arduino code

Set `SERVER_IP` in `master_node.ino` to your Tailscale hostname and re-upload.

---

### Step 6 – Touch screen calibration

After first power-on, the touch coordinates may be offset. To calibrate:

1. Install the **TFT_eSPI** library examples: `File → Examples → TFT_eSPI → Generic → Touch_calibrate`
2. Upload and follow the on-screen instructions (tap the crosshairs)
3. Note the four values printed to Serial: `TS_MINX`, `TS_MAXX`, `TS_MINY`, `TS_MAXY`
4. Update the four `#define` lines near the top of `master_node.ino`:
   ```cpp
   #define TS_MINX  ????
   #define TS_MAXX  ????
   #define TS_MINY  ????
   #define TS_MAXY  ????
   ```
5. Re-upload `master_node.ino`

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| TFT shows garbled colours | RGB/BGR byte order mismatch | Change `TFT_RGB_ORDER` in `User_Setup.h` from `TFT_RGB` to `TFT_BGR` |
| TFT shows garbled pixels | SPI clock too fast | Lower `SPI_FREQUENCY` to `20000000` in `User_Setup.h` |
| Touch position is shifted or inverted | Wrong calibration values | Re-run the Touch_calibrate example |
| No data on TFT (shows splash forever) | WiFi not connecting | Verify SSID/PASSWORD; check WiFi band (R4 WiFi is 2.4 GHz only) |
| HTTP errors, no room data | SERVER_IP wrong, or Tailscale Funnel not active | Check Tailscale admin console; verify `curl https://YOUR-NAS.ts.net/dates` works |
| nginx returns 404 for all requests | nginx upstream can't reach room-server | Run `docker inspect cdrlc-room-server` to get bridge IP, update `nginx.conf` upstream |
| LEDs always green (never red) | I2C packet not arriving at Wing boards | Check wiring; verify pull-up resistors; use `i2c_ping_master.ino` to test bus |
| DHT11 reads NaN | Bad wiring or missing pull-up | Confirm 10 kΩ pull-up on DHT11 DATA → 5V; verify D2 connection |
| QR code doesn't scan | URL too long for QR version 9 | Check URL length; if >154 chars, the initText call returns false – Serial will show the error |

---

## Google Calendar API Note

Room availability is fetched from Google Calendar's public **ListAvailableSlots** RPC endpoint using a browser-side API key. This key is embedded in `room_server.py` and is read-only – it cannot modify any calendar data. No OAuth login or user credentials are required for the availability polling.

The booking URLs in `GCAL_URLS` point to each room's **public Appointment Scheduling page**. Users complete booking directly on Google Calendar; no credentials pass through this system.

---

## Project File Tree

```
Project/
├── master_node/
│   ├── master_node.ino     ← Arduino 1 (Master)
│   └── User_Setup.h        ← TFT_eSPI pin config
├── ui_node/
│   └── ui_node.ino         ← Arduino 2 (DHT11 sensor)
├── wing_a.ino              ← Arduino 3 (Wing A LEDs)
├── wing_b.ino              ← Arduino 4 (Wing B LEDs)
├── room_server.py          ← Python availability server
├── Dockerfile              ← Docker image for room-server
├── docker-compose.yml      ← Three-container stack
├── nginx.conf              ← Reverse proxy + rate limiter
├── tailscale-serve.json    ← Tailscale Funnel config
└── README.md               ← This file
```
