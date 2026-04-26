/*
  ================================================================
  Arduino 1 – Master Node  v5.0
  CDRLC Study Room Availability Monitor
  ================================================================
  Responsibilities:
    - Connect to WiFi; poll room availability from the NAS server
    - Drive a 4" ST7796S TFT touch screen:
        Normal view  – colour-coded room list (green=free / grey=taken)
        QR view      – on-device QR code when user taps a free room
    - On QR tap: user scans QR → Google Calendar booking page opens
    - Read DHT11 temp/humidity from Arduino 2 via I2C requestFrom
    - Broadcast LED status to Wing A (0x09) and Wing B (0x0A) over I2C
    - Drive passive buzzer (two-tone beep when a room becomes free)
    - Three push buttons: cycle slot / manual refresh / cycle date

  I2C bus:
    Master (this board) : no address – Wire.begin() with no arg
    0x08  Arduino 2     : DHT11 sensor – requestFrom(0x08, 4)
    0x09  Arduino 3     : Wing A LEDs  (rooms 2432 2434 2436 2438 2440)
    0x0A  Arduino 4     : Wing B LEDs  (rooms 2426 2428 2430)

  Pin assignments:
    D2   HC-SR04 Trig  (proximity sensor – screen auto-dim)
    D3   TFT Backlight (PWM – brightness control)
    D4   Button 1 – next time slot
    D5   Button 2 – manual server refresh
    D6   Button 3 – next date
    D7   HC-SR04 Echo  (proximity sensor – screen auto-dim)
    D8   Passive buzzer
    D9   TFT RST
    D10  TFT CS
    D11  SPI MOSI  (TFT + Touch shared bus)
    D12  SPI MISO
    D13  SPI SCK
    A0   TFT DC (Data/Command)   [= digital pin 14 on R4]
    A1   XPT2046 Touch CS        [= digital pin 15 on R4]
    A2   LDR (photoresistor) – ambient light → LED brightness for all wings
    A4   I2C SDA
    A5   I2C SCL

  LDR wiring (voltage divider):
    LDR one end → 5V
    LDR other end → A2  AND  → 10kΩ → GND

  HC-SR04 wiring:
    VCC  → 5V
    GND  → GND
    Trig → D2
    Echo → D7

  TFT backlight wiring:
    TFT BL pin → D3  (do NOT tie BL to 3.3V/5V directly)
    Dim when nobody within 100 cm, full brightness when someone approaches.

  Libraries (install via Arduino Library Manager):
    TFT_eSPI            by Bodmer
    XPT2046_Touchscreen by Paul Stoffregen
    qrcode              by ricmoo
    NTPClient           by Fabrice Weinberg
    ArduinoHttpClient   by Arduino
  ================================================================
*/

#include <Wire.h>
#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <qrcode.h>

// ── WiFi & server ─────────────────────────────────────────────────────────────
const char *SSID = "NETGEAR77";                        // ← change to your WiFi SSID
const char *PASSWORD = "pinkbutter932";                // ← change to your WiFi password
const char *SERVER_IP = "zz-cloud.tail6b9dfa.ts.net"; // ← Tailscale Funnel hostname
const int SERVER_PORT = 443;                          // HTTPS via Tailscale Funnel

// ── I2C slave addresses ───────────────────────────────────────────────────────
#define SLAVE_ENV 0x08    // Arduino 2 – DHT11 environment sensor
#define SLAVE_WING_A 0x09 // Arduino 3 – Wing A bi-color LEDs
#define SLAVE_WING_B 0x0A // Arduino 4 – Wing B bi-color LEDs

// ── Pins ──────────────────────────────────────────────────────────────────────
#define TRIG_PIN   2   // HC-SR04 trigger
#define TFT_BL_PIN 3   // TFT backlight (PWM)
#define LDR_PIN    A2  // photoresistor (ambient light → LED brightness)
#define BTN1       4
#define BTN2       5
#define BTN3       6
#define ECHO_PIN   7   // HC-SR04 echo
#define BUZZER_PIN 8

// ── Backlight / proximity / activity ─────────────────────────────────────────
#define BL_FULL             255  // active brightness
#define BL_DIM               40  // idle dim level
#define BL_OFF                0  // screen off
#define BL_FADE_START_MS  10000UL  // idle 10 s → begin smooth fade
#define BL_FADE_END_MS    13000UL  // idle 13 s → fully dimmed (3-s fade window)
#define BL_OFF_MS         30000UL  // idle 30 s → screen off completely
#define PRESENCE_DIST_CM    100  // ≤ 100 cm counts as "someone present"
#define DIST_CHECK_MS       200  // HC-SR04 poll interval (ms)

// ── TFT object ───────────────────────────────────────────────────────────────
// Touch is handled by TFT_eSPI's built-in XPT2046 driver (no separate library).
// TOUCH_CS pin is declared in User_Setup.h (pin 15 / A1).
TFT_eSPI tft;

// Touch calibration data – obtained by running the TFT_eSPI Touch_calibrate
// example and recording the five values it prints.  Re-run if touch feels off.
static const uint16_t TOUCH_CAL[5] = { 277, 3647, 219, 3577, 7 };

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define COL_BG 0x0000       // black
#define COL_HDR_BG 0x000F   // very dark navy  (header bar)
#define COL_FREE_BG 0x03C0  // dark green      (free room row)
#define COL_TAKEN_BG 0x2104 // very dark grey  (taken room row)
#define COL_FREE_FG 0xFFFF  // white
#define COL_TAKEN_FG 0x7BEF // medium grey
#define COL_TITLE 0x07FF    // cyan            (header title)
#define COL_DATE 0xFFE0     // yellow          (date + slot line)
#define COL_TEMP 0xFD20     // orange          (temperature readout)
#define COL_ACCENT 0x07E0   // bright green    (free-count badge)
#define COL_ERR_BG 0x4000   // dark red        (error overlay background)

// ── Screen layout  (480×320 landscape after setRotation(1)) ──────────────────
#define SCREEN_W 480
#define SCREEN_H 320
#define HEADER_H 56 // header bar height in pixels
#define ROW_H 33    // room row height  (8 × 33 + 56 = 320 exactly)

// ── Time slots ────────────────────────────────────────────────────────────────
const char *SLOT_LABELS[] = {"09:30", "10:30", "11:30",
                             "12:30", "13:30", "14:30", "15:30", "16:30"};
const char *SLOT_PARAMS[] = {"0930", "1030", "1130",
                             "1230", "1330", "1430", "1530", "1630"};
const int NUM_SLOTS = 8;

// ── Room names & Google Calendar booking URLs ─────────────────────────────────
// String literals on ARM Cortex-M4 are stored in flash, not SRAM.
const char ROOM_NAMES[8][5] = {
    "2432", "2434", "2436", "2438", "2440", "2426", "2428", "2430"};
const char *const GCAL_URLS[8] = {
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ2zV3rqByvVaYY7-waxW0Ua1PFUrJy-geZAjUoquyZkwl-b7k-AVlj-gXtu6hL2fdoNyOW_fogm",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ3qsbG4VZdKujEEomP4ZHcGG9G4DrTlALvL_aiJevQ2RQ1CqCTSox-V5VLmuvL2tx6UihIkAMbK",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1QyvW6zFu7S-UsVPVzcjZ1BuXTmbqFs1ijAzbAdAUjuyBEK37hNdn60h3GTYWUBGzGHM2tGpq9",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1HATv8WKSJRkxB09vAt9Kf3--pxITTPS9jiW8sVQ_h5vQvwVSXi2qdcyiUtFhJ7koaQ6YYmn0r",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ1ZA6mIdf3_OSJa22JX9-ziy8iqdcPLFaTjOAKSgvKm3LLnjAMPYiAyOSlVzHHS5jHVlaov9E_l",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ2KJdEXUrjMzwDlPBlhJYrh_kMoGgkZSWfbTMVNhFQJRdlt-cEW5u5v1VpLk_kvXfhkQVIzMa6e",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5",
    "https://calendar.google.com/calendar/appointments/schedules/AcZssZ0p9czIXJNtpMOf5tXBGmslRJMv64NSx_5D7atsWFCoajdv5GvbMZb_k7alyZ5zxU16KJ1Elsr5"};

// ── Global state ──────────────────────────────────────────────────────────────
char dateList[5][9];    // "YYYYMMDD"
char datePretty[5][11]; // "Mon MM/DD"
int numDates = 0;
int dateIdx = 1; // default: tomorrow (index 1)
int slotIdx = 0;

bool roomFree[8] = {};
uint8_t prevStatusA = 0xFF;
uint8_t prevStatusB = 0xFF;

unsigned long lastAutoFetch = 0;
const unsigned long AUTO_FETCH_MS = 300000UL; // 5 minutes

// ── DHT11 data (polled from Arduino 2 via I2C) ────────────────────────────────
float cachedTemp = NAN;
float cachedHumi = NAN;
unsigned long lastDHTRead = 0;
const unsigned long DHT_READ_MS = 30000UL; // re-read every 30 s

// ── Display state machine ─────────────────────────────────────────────────────
enum DispState
{
  DISP_NORMAL,
  DISP_QR
};
DispState dispState = DISP_NORMAL;
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_COOLDOWN = 500UL; // ms between accepted touches
// After any QR ↔ Normal transition, block all touches until the finger
// physically leaves the screen.  Prevents one tap from firing two actions
// (e.g. "dismiss QR" immediately re-booking the same room).
bool touchMustRelease = false;

// ── Today's date (set after NTP sync in setup) ───────────────────────────────
// Used to detect when the displayed date is today (snapshot mode):
// free rooms show "Free" instead of "Book >" and tapping is disabled.
char todayDate[9] = "";   // "YYYYMMDD", e.g. "20260425"

// ── Slot data cache ───────────────────────────────────────────────────────────
// Avoids a network round-trip on every button press.
// cachedSlots[dateIdx][slotIdx] = 8-char status string ("10010100")
// slotCached[dateIdx][slotIdx]  = true once that slot has been fetched.
char cachedSlots[5][8][9];
bool slotCached[5][8];     // zero-initialised (all false) by default

// ── LED brightness (read from LDR, sent to both wings via I2C) ───────────────
uint8_t ledBrightness = 200;          // cached value, updated every loop tick

// ── Backlight state ───────────────────────────────────────────────────────────
int           currentBL     = BL_FULL;
unsigned long lastActiveTime = 0;   // last time activity was detected
unsigned long lastDistCheck  = 0;

// ── Button debounce ───────────────────────────────────────────────────────────
unsigned long btnTime[3] = {0, 0, 0};
const unsigned long DEBOUNCE = 200UL;

// ── Non-blocking manual refresh (BTN2) ───────────────────────────────────────
bool pendingRefresh = false;
unsigned long refreshStart = 0;
const unsigned long REFRESH_WAIT_MS = 3000UL;

// ── Non-blocking two-tone buzzer ──────────────────────────────────────────────
enum BuzzState
{
  BUZZ_IDLE,
  BUZZ_NOTE1,
  BUZZ_NOTE2
};
BuzzState buzzState = BUZZ_IDLE;
unsigned long buzzStart = 0;

void buzzerTrigger()
{
  if (buzzState != BUZZ_IDLE)
    return;
  buzzState = BUZZ_NOTE1;
  buzzStart = millis();
  tone(BUZZER_PIN, 880, 200);
}
void buzzerUpdate()
{
  if (buzzState == BUZZ_IDLE)
    return;
  unsigned long now = millis();
  if (buzzState == BUZZ_NOTE1 && now - buzzStart >= 250)
  {
    buzzState = BUZZ_NOTE2;
    buzzStart = now;
    tone(BUZZER_PIN, 1047, 400);
  }
  else if (buzzState == BUZZ_NOTE2 && now - buzzStart >= 400)
  {
    buzzState = BUZZ_IDLE;
  }
}

// ── HC-SR04 proximity + activity-based backlight ──────────────────────────────

long readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30 ms timeout
  if (duration == 0) return 999;                  // out of range → no one present
  return duration / 58L;
}

// Call whenever user activity is detected (button, touch, or proximity).
// Immediately restores full brightness and resets the idle timer.
void markActive() {
  lastActiveTime = millis();
  if (currentBL != BL_FULL) {
    currentBL = BL_FULL;
    analogWrite(TFT_BL_PIN, BL_FULL);
    // Wake wing LEDs at the same time the screen lights up
    sendI2C(SLAVE_WING_A, 0x01, prevStatusA);
    sendI2C(SLAVE_WING_B, 0x01, prevStatusB);
  }
}

// Compute brightness from idle time and apply via PWM.
// Timeline:
//   0–10 s  : BL_FULL (255)
//   10–13 s : smooth fade from BL_FULL → BL_DIM  (3-second linear ramp)
//   13–30 s : BL_DIM  (40)  — dim but readable
//   30 s+   : BL_OFF  (0)   — screen off
void updateBacklight() {
  unsigned long now = millis();

  // Poll distance sensor at DIST_CHECK_MS interval
  if (now - lastDistCheck >= DIST_CHECK_MS) {
    lastDistCheck = now;
    if (readDistanceCm() <= PRESENCE_DIST_CM) {
      markActive();
      return;
    }
  }

  unsigned long idle = now - lastActiveTime;
  int target;
  if      (idle < BL_FADE_START_MS) target = BL_FULL;
  else if (idle < BL_FADE_END_MS)   target = map(idle,
                                                  BL_FADE_START_MS, BL_FADE_END_MS,
                                                  BL_FULL, BL_DIM);
  else if (idle < BL_OFF_MS)        target = BL_DIM;
  else                               target = BL_OFF;

  if (target != currentBL) {
    currentBL = target;
    analogWrite(TFT_BL_PIN, currentBL);
  }
}

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);

// ══════════════════════════════════════════════════════════════════════════════
// I2C helpers
// ══════════════════════════════════════════════════════════════════════════════

// Send 4-byte packet to a Wing LED slave:
//   [ctrl, data, brightness, ctrl^data^brightness]
// brightness = 0 when screen is off (BL_OFF) so LEDs sleep with the screen.
void sendI2C(uint8_t addr, uint8_t ctrl, uint8_t data)
{
  uint8_t bri = (currentBL == BL_OFF) ? 0 : ledBrightness;
  Wire.beginTransmission(addr);
  Wire.write(ctrl);
  Wire.write(data);
  Wire.write(bri);
  Wire.write(ctrl ^ data ^ bri);
  Wire.endTransmission();
}

// Send 2 bytes to Arduino 2: [freeCount, slotIdx]
// Arduino 2 drives the TM1637 (time slot) and 74HC595+7-seg (free count).
void sendDisplayData() {
  int fc = 0;
  for (int i = 0; i < 8; i++) if (roomFree[i]) fc++;
  Wire.beginTransmission(SLAVE_ENV);
  Wire.write((uint8_t)fc);
  Wire.write((uint8_t)slotIdx);
  Wire.endTransmission();
}

// Request 4 bytes from Arduino 2: [tempInt, tempFrac, humiInt, checksum]
void readDHT11()
{
  if (Wire.requestFrom((uint8_t)SLAVE_ENV, (uint8_t)4) < 4)
    return;
  uint8_t tI = Wire.read();
  uint8_t tF = Wire.read();
  uint8_t hI = Wire.read();
  uint8_t ck = Wire.read();
  if (ck != (uint8_t)(tI ^ tF ^ hI))
  {
    Serial.println(F("[DHT] checksum fail"));
    return;
  }
  cachedTemp = tI + tF * 0.1f;
  cachedHumi = (float)hI;
  Serial.print(F("[DHT] "));
  Serial.print(cachedTemp, 1);
  Serial.print(F("C  "));
  Serial.print(cachedHumi, 0);
  Serial.println('%');
}

// ══════════════════════════════════════════════════════════════════════════════
// HTTP helper (HTTPS via WiFiSSLClient)
// ══════════════════════════════════════════════════════════════════════════════

WiFiSSLClient wifiClient;

String httpGet(const String &path)
{
  if (WiFi.status() != WL_CONNECTED)
    return "";
  Serial.print(F("[HTTP] GET "));
  Serial.println(path);
  HttpClient http(wifiClient, SERVER_IP, SERVER_PORT);
  http.connectionKeepAlive();
  if (http.get(path) != 0)
    return "";
  int statusCode = http.responseStatusCode();
  // Always consume the response body — even on non-200 — so that the
  // keep-alive connection is left in a clean state for the next request.
  // Returning early without reading body leaves stale bytes in the socket
  // and corrupts the next request on the same connection.
  String body = http.responseBody();
  if (statusCode != 200)
  {
    Serial.print(F("[HTTP] status ")); Serial.println(statusCode);
    return "";
  }
  body.trim();
  return body;
}

// ══════════════════════════════════════════════════════════════════════════════
// TFT drawing
// ══════════════════════════════════════════════════════════════════════════════

// Draw text horizontally centred at Y position
void tftCenter(const char *txt, int y, uint8_t font,
               uint16_t fg, uint16_t bg)
{
  tft.setTextColor(fg, bg);
  tft.drawString(txt, (SCREEN_W - tft.textWidth(txt, font)) / 2, y, font);
}

// Full-screen splash used during startup
void drawSplash(const char *line1, const char *line2 = nullptr)
{
  tft.fillScreen(COL_BG);
  tftCenter(line1, 118, 4, COL_TITLE, COL_BG);
  if (line2)
    tftCenter(line2, 166, 2, COL_FREE_FG, COL_BG);
}

// Normal screen: colour-coded room availability list
void drawNormalScreen()
{
  // Count free rooms first so the header can display it
  int freeCount = 0;
  for (int i = 0; i < 8; i++)
    if (roomFree[i])
      freeCount++;

  // ── Header bar ────────────────────────────────────────────────────────────
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HDR_BG);

  // Top row  →  title (left) + temperature (right)
  tft.setTextColor(COL_TITLE, COL_HDR_BG);
  tft.drawString("CDRLC Study Rooms", 10, 6, 2);

  if (!isnan(cachedTemp))
  {
    char env[20];
    snprintf(env, sizeof(env), "%.1fC  %.0f%%", cachedTemp, cachedHumi);
    int w = tft.textWidth(env, 2);
    tft.setTextColor(COL_TEMP, COL_HDR_BG);
    tft.drawString(env, SCREEN_W - w - 10, 6, 2);
  }

  // Bottom row  →  date + slot (left) + free-room count (right)
  if (numDates > 0)
  {
    char info[26];
    snprintf(info, sizeof(info), "%s  %s", datePretty[dateIdx], SLOT_LABELS[slotIdx]);
    tft.setTextColor(COL_DATE, COL_HDR_BG);
    tft.drawString(info, 10, 32, 2);
  }
  {
    char fs[10];
    snprintf(fs, sizeof(fs), "%d free", freeCount);
    tft.setTextColor((freeCount > 0) ? COL_ACCENT : COL_TAKEN_FG, COL_HDR_BG);
    int w = tft.textWidth(fs, 2);
    tft.drawString(fs, SCREEN_W - w - 10, 32, 2);
  }

  // Is the displayed date today?  If so show "Free" (no booking) instead of "Book >"
  bool showingToday = (todayDate[0] != '\0' && numDates > 0 &&
                       strcmp(dateList[dateIdx], todayDate) == 0);

  // ── Room rows  (8 × ROW_H = 264 px; HEADER_H + 264 = 320) ───────────────
  for (int i = 0; i < 8; i++)
  {
    int y = HEADER_H + i * ROW_H;
    uint16_t bg = roomFree[i] ? COL_FREE_BG : COL_TAKEN_BG;
    uint16_t fg = roomFree[i] ? COL_FREE_FG : COL_TAKEN_FG;

    tft.fillRect(0, y, SCREEN_W, ROW_H - 1, bg);
    tft.drawFastHLine(0, y + ROW_H - 1, SCREEN_W, COL_BG); // 1 px gap

    // Room label – left-aligned
    char label[12];
    snprintf(label, sizeof(label), "Room %s", ROOM_NAMES[i]);
    tft.setTextColor(fg, bg);
    tft.drawString(label, 16, y + (ROW_H - 16) / 2, 2);

    // Status pill – right-aligned
    // Today's free rooms: show "Free" as plain text (no interactive button).
    // Other days' free rooms: show "Book >" inside a tappable white pill.
    int py = y + (ROW_H - 16) / 2;
    if (roomFree[i] && !showingToday)
    {
      // Interactive "Book >" pill
      const char *tag = "Book >";
      int tw = tft.textWidth(tag, 2);
      int px = SCREEN_W - 92 + (82 - tw) / 2;
      tft.fillRoundRect(SCREEN_W - 92, y + 6, 82, ROW_H - 13, 7, COL_FREE_FG);
      tft.setTextColor(COL_FREE_BG, COL_FREE_FG);
      tft.drawString(tag, px, py, 2);
    }
    else if (roomFree[i] && showingToday)
    {
      // Today's snapshot: free but can't book — plain "Free" label in accent green
      const char *tag = "Free";
      int tw = tft.textWidth(tag, 2);
      int px = SCREEN_W - 92 + (82 - tw) / 2;
      tft.setTextColor(COL_ACCENT, COL_FREE_BG);
      tft.drawString(tag, px, py, 2);
    }
    else
    {
      // Taken room
      const char *tag = "Taken";
      int tw = tft.textWidth(tag, 2);
      int px = SCREEN_W - 92 + (82 - tw) / 2;
      tft.setTextColor(COL_TAKEN_FG, COL_TAKEN_BG);
      tft.drawString(tag, px, py, 2);
    }
  }
}

// QR screen: full-screen QR code for a room's Google Calendar booking page.
// URLs are taken directly from GCAL_URLS[] (flash) — no network call needed.
//
// QR Version 7 (45×45 modules), ECC_LOW, byte-mode capacity = 154 chars.
// All CDRLC Google Calendar URLs are ~140 chars — comfortably within capacity.
// Buffer: ceil(45²/8) = 254 bytes → static uint8_t qrData[256].
// Display: scale=5, QR = 225×225 px — large and easily scannable.
//
// IMPORTANT: ricmoo qrcode library returns 0 on success (C convention).
// Never cast the return value to bool. Check qrc.size > 0 instead.
void drawQRScreen(int roomIdx)
{
  // ── Generate QR from Google Calendar URL (instant, no network) ───────────
  static uint8_t qrData[256];   // V7 needs 254 bytes; 256 gives safe margin
  QRCode qrc;
  qrcode_initText(&qrc, qrData, 7, ECC_LOW, GCAL_URLS[roomIdx]);

  Serial.print(F("[QR] Room ")); Serial.print(ROOM_NAMES[roomIdx]);
  Serial.print(F("  size=")); Serial.print(qrc.size);
  Serial.print(F("  url_len=")); Serial.println(strlen(GCAL_URLS[roomIdx]));

  if (qrc.size == 0)
  {
    Serial.println(F("[QR] ERROR: size=0 — URL exceeds V7 ECC_LOW 154-char limit"));
    tft.fillScreen(COL_BG);
    tftCenter("QR Error", SCREEN_H / 2, 4, TFT_RED, COL_BG);
    unsigned long t = millis();
    while (millis() - t < 2000) {}
    dispState = DISP_NORMAL;
    drawNormalScreen();
    return;
  }

  // ── Header (44 px): room name + date/slot context ────────────────────────
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, SCREEN_W, 44, COL_HDR_BG);

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "Room %s  -  Scan to Book", ROOM_NAMES[roomIdx]);
  tftCenter(hdr, 6, 2, COL_TITLE, COL_HDR_BG);

  if (numDates > 0)
  {
    char sub[30];
    snprintf(sub, sizeof(sub), "%s  %s", datePretty[dateIdx], SLOT_LABELS[slotIdx]);
    tftCenter(sub, 26, 2, COL_DATE, COL_HDR_BG);
  }

  // ── QR code: scale to fill available area ────────────────────────────────
  // Available: 480×(320-44-28) = 480×248 px.
  // V7 (size=45): scale = min(480-16/45, 248/45) = min(10,5) = 5 → 225×225 px
  const int AVAIL_H = SCREEN_H - 44 - 28;
  const int AVAIL_W = SCREEN_W - 16;
  int SCALE = min(AVAIL_W / (int)qrc.size, AVAIL_H / (int)qrc.size);
  if (SCALE < 1) SCALE = 1;
  const int QR_PX = (int)qrc.size * SCALE;
  const int xOff  = (SCREEN_W - QR_PX) / 2;
  const int yOff  = 44 + (AVAIL_H - QR_PX) / 2;

  tft.fillRect(xOff - 4, yOff - 4, QR_PX + 8, QR_PX + 8, TFT_WHITE);

  tft.startWrite();
  for (int row = 0; row < (int)qrc.size; row++)
    for (int col = 0; col < (int)qrc.size; col++)
      if (qrcode_getModule(&qrc, col, row))
        tft.fillRect(xOff + col * SCALE, yOff + row * SCALE, SCALE, SCALE, TFT_BLACK);
  tft.endWrite();

  tftCenter("Tap anywhere to return", SCREEN_H - 22, 2, COL_TAKEN_FG, COL_BG);
}

// Draw the header bar with a custom status badge (replaces the "N free" badge).
// Keeps the date/slot context visible so the user knows what they're waiting for.
void drawHeaderWithBadge(const char *badge, uint16_t badgeColor)
{
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HDR_BG);

  tft.setTextColor(COL_TITLE, COL_HDR_BG);
  tft.drawString("CDRLC Study Rooms", 10, 6, 2);

  if (!isnan(cachedTemp))
  {
    char env[20];
    snprintf(env, sizeof(env), "%.1fC  %.0f%%", cachedTemp, cachedHumi);
    int w = tft.textWidth(env, 2);
    tft.setTextColor(COL_TEMP, COL_HDR_BG);
    tft.drawString(env, SCREEN_W - w - 10, 6, 2);
  }

  if (numDates > 0)
  {
    char info[26];
    snprintf(info, sizeof(info), "%s  %s", datePretty[dateIdx], SLOT_LABELS[slotIdx]);
    tft.setTextColor(COL_DATE, COL_HDR_BG);
    tft.drawString(info, 10, 32, 2);
  }

  int w = tft.textWidth(badge, 2);
  tft.setTextColor(badgeColor, COL_HDR_BG);
  tft.drawString(badge, SCREEN_W - w - 10, 32, 2);
}

// Fill only the content area (below the header) with a centred status message.
// bigLine is drawn large (font 4, ~26 px); smallLine is drawn smaller below it.
// The header is left untouched, keeping the date/slot context on screen.
void drawContentStatus(uint16_t bg, const char *bigLine, const char *smallLine = nullptr)
{
  const int cy = HEADER_H;
  const int ch = SCREEN_H - HEADER_H;                      // 264 px
  tft.fillRect(0, cy, SCREEN_W, ch, bg);
  int totalH = 26 + (smallLine ? 26 : 0);                  // font4≈26 + gap+font2≈26
  int y1 = cy + (ch - totalH) / 2;
  tftCenter(bigLine, y1, 4, TFT_WHITE, bg);
  if (smallLine)
    tftCenter(smallLine, y1 + 36, 2, COL_TAKEN_FG, bg);
}

// ══════════════════════════════════════════════════════════════════════════════
// Touch handler  (called every loop iteration)
// ══════════════════════════════════════════════════════════════════════════════

void handleTouch()
{
  if (pendingRefresh) return;

  // tft.getTouch() applies the stored calibration and returns true screen
  // coordinates (0–479, 0–319) directly – no manual map() needed.
  uint16_t tx, ty;
  bool isTouched = tft.getTouch(&tx, &ty);

  // Wait-for-release guard: after any QR↔Normal transition, ignore all
  // touches until the finger physically leaves the screen.  This prevents
  // the "dismiss QR" tap from immediately re-booking the same room because
  // the finger is still down when the new screen renders.
  if (touchMustRelease)
  {
    if (!isTouched) touchMustRelease = false;
    return;
  }

  if (!isTouched) return;

  // Any touch counts as user activity — wake/reset backlight timer.
  // If the screen was completely off, just wake it up without processing
  // the tap as a booking action (user didn't know what they were tapping).
  bool wasOff = (currentBL == BL_OFF);
  markActive();
  if (wasOff) { touchMustRelease = true; return; }

  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_COOLDOWN)
    return;
  lastTouchTime = now;

  Serial.print(F("[Touch] "));
  Serial.print(tx);
  Serial.print(',');
  Serial.println(ty);

  if (dispState == DISP_QR)
  {
    // Any tap on QR screen → return to room list; require finger lift first.
    dispState = DISP_NORMAL;
    drawNormalScreen();
    touchMustRelease = true;
    return;
  }

  // Don't allow booking taps when showing today's snapshot data —
  // Google Calendar blocks same-day booking so a QR would be useless.
  bool showingToday = (todayDate[0] != '\0' && numDates > 0 &&
                       strcmp(dateList[dateIdx], todayDate) == 0);

  if (dispState == DISP_NORMAL && ty >= HEADER_H && !showingToday)
  {
    int row = (ty - HEADER_H) / ROW_H;
    if (row >= 0 && row < 8 && roomFree[row])
    {
      Serial.print(F("[Touch] Book room "));
      Serial.println(ROOM_NAMES[row]);
      dispState = DISP_QR;
      drawQRScreen(row);
      touchMustRelease = true;   // require finger lift before next action
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// Date helpers
// ══════════════════════════════════════════════════════════════════════════════

const char *dowAbbrev(const char *d)
{
  int y = (d[0] - '0') * 1000 + (d[1] - '0') * 100 + (d[2] - '0') * 10 + (d[3] - '0');
  int m = (d[4] - '0') * 10 + (d[5] - '0');
  int day = (d[6] - '0') * 10 + (d[7] - '0');
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3)
    y--;
  int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + day) % 7;
  static const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return names[dow];
}

void makePretty(const char *d, char *out)
{
  snprintf(out, 11, "%s %c%c/%c%c", dowAbbrev(d), d[4], d[5], d[6], d[7]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Data fetch
// ══════════════════════════════════════════════════════════════════════════════

void fetchDates()
{
  String raw = httpGet("/dates");
  if (!raw.length())
    return;
  numDates = 0;
  int start = 0;
  while (start < (int)raw.length() && numDates < 5)
  {
    int comma = raw.indexOf(',', start);
    String tok = (comma < 0) ? raw.substring(start) : raw.substring(start, comma);
    tok.trim();
    if (tok.length() == 8)
    {
      strncpy(dateList[numDates], tok.c_str(), 8);
      dateList[numDates][8] = '\0';
      makePretty(dateList[numDates], datePretty[numDates]);
      numDates++;
    }
    if (comma < 0)
      break;
    start = comma + 1;
  }
  // Pick the default date:
  //   Mon–Thu (NTP day 1–4): today is dateList[0] but is all-blocked by Google;
  //                           default to index 1 (tomorrow).
  //   Fri–Sun (NTP day 5,6,0): today is NOT in the list; the list starts from
  //                             next Monday, so index 0 is already the best default.
  // timeClient.getDay() → 0=Sun 1=Mon 2=Tue 3=Wed 4=Thu 5=Fri 6=Sat
  int dow = timeClient.getDay();
  bool todayInList = (dow >= 1 && dow <= 4);   // Mon–Thu
  dateIdx = (numDates > 1 && todayInList) ? 1 : 0;

  Serial.print(F("[Dates] got ")); Serial.print(numDates);
  Serial.print(F("  dow=")); Serial.print(dow);
  Serial.print(F("  dateIdx=")); Serial.println(dateIdx);

  memset(slotCached, false, sizeof(slotCached));  // date list changed → invalidate all
}

// Apply a slot data string to LEDs and TFT (shared by cache-hit and network paths).
void applySlotData(const char *data)
{
  uint8_t statusA = 0, statusB = 0;
  for (int i = 0; i < 8; i++)
  {
    roomFree[i] = (data[i] == '0');
    if (data[i] == '1')
    {
      if (i < 5) statusA |= (1 << i);
      else       statusB |= (1 << (i - 5));
    }
  }
  sendI2C(SLAVE_WING_A, 0x01, statusA);
  sendI2C(SLAVE_WING_B, 0x01, statusB);
  if ((prevStatusA & ~statusA) || (prevStatusB & ~statusB)) buzzerTrigger();
  prevStatusA = statusA;
  prevStatusB = statusB;
  sendDisplayData();   // update TM1637 (time slot) + 7-seg (free count) on Arduino 2
  if (dispState == DISP_NORMAL) drawNormalScreen();
}

// Prefetch all 8 time slots for every available date in one pass.
// Uses the /allslots endpoint: one HTTP request per date (max 5 requests)
// instead of 40 individual /slot calls.  After this returns, every
// cachedSlots[d][s] entry is populated and all navigation is instant.
// Also resets the 5-minute auto-refresh timer.
void prefetchAllDates()
{
  for (int d = 0; d < numDates; d++)
  {
    // Progress overlay ─ keep header visible so user sees which date is loading
    drawHeaderWithBadge("loading...", COL_DATE);
    char sub[20];
    snprintf(sub, sizeof(sub), "%d / %d", d + 1, numDates);
    drawContentStatus(COL_TAKEN_BG, datePretty[d], sub);

    String resp = httpGet(String("/allslots?date=") + dateList[d]);
    Serial.print(F("[Prefetch] ")); Serial.print(dateList[d]);
    Serial.print(F("  len=")); Serial.println(resp.length());

    if (resp.length() < 8) continue;   // network error for this date – skip

    // Parse comma-separated 8-char slot strings into the cache
    // Format: "10010100,11111111,00010000,..." (8 entries, 71 chars total)
    int pos = 0;
    for (int s = 0; s < NUM_SLOTS; s++)
    {
      int comma = resp.indexOf(',', pos);
      String tok = (comma < 0) ? resp.substring(pos) : resp.substring(pos, comma);
      tok.trim();
      if (tok.length() == 8)
      {
        strncpy(cachedSlots[d][s], tok.c_str(), 8);
        cachedSlots[d][s][8] = '\0';
        slotCached[d][s] = true;
      }
      if (comma < 0) break;
      pos = comma + 1;
    }
  }
  lastAutoFetch = millis();   // reset 5-min timer after full prefetch
  Serial.println(F("[Prefetch] Done — all slots cached"));
}

void fetchAndDisplay()
{
  if (numDates == 0) return;

  // ── Cache hit: instant display, no network call ───────────────────────────
  if (slotCached[dateIdx][slotIdx])
  {
    Serial.print(F("[Cache] ")); Serial.print(dateList[dateIdx]);
    Serial.print(' '); Serial.println(SLOT_PARAMS[slotIdx]);
    applySlotData(cachedSlots[dateIdx][slotIdx]);
    return;
  }

  // ── Cache miss: show loading state, then fetch ────────────────────────────
  // Keep the QR view intact if the user is scanning – only update in NORMAL mode.
  if (dispState == DISP_NORMAL)
  {
    drawHeaderWithBadge("loading...", COL_DATE);
    drawContentStatus(COL_TAKEN_BG, "Fetching data...", "Please wait");
  }

  String resp = httpGet(String("/slot?date=") + dateList[dateIdx] + "&time=" + SLOT_PARAMS[slotIdx]);
  if (resp.length() != 8)
  {
    if (dispState == DISP_NORMAL)
    {
      drawHeaderWithBadge("error!", TFT_RED);
      drawContentStatus(COL_ERR_BG, "Network Error", "Showing last known data");
      unsigned long t = millis();
      while (millis() - t < 2000) {}   // let user read the error message
      drawNormalScreen();               // restore previous room data
    }
    return;
  }

  strncpy(cachedSlots[dateIdx][slotIdx], resp.c_str(), 8);
  cachedSlots[dateIdx][slotIdx][8] = '\0';
  slotCached[dateIdx][slotIdx] = true;

  applySlotData(cachedSlots[dateIdx][slotIdx]);
  lastAutoFetch = millis();   // only reset 5-min timer on real network fetch
  Serial.print(F("[Slot] fetched ")); Serial.print(resp);
  Serial.print(F("  ")); Serial.print(dateList[dateIdx]);
  Serial.print(' '); Serial.println(SLOT_PARAMS[slotIdx]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Button helper
// ══════════════════════════════════════════════════════════════════════════════

bool btnPressed(int pin, int idx)
{
  if (digitalRead(pin) == LOW && millis() - btnTime[idx] > DEBOUNCE)
  {
    btnTime[idx] = millis();
    return true;
  }
  return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Setup & Loop
// ══════════════════════════════════════════════════════════════════════════════

void setup()
{
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(TFT_BL_PIN, OUTPUT);
  analogWrite(TFT_BL_PIN, BL_FULL);    // full brightness at startup
  lastActiveTime = millis();            // treat boot as activity

  // TFT – pin mapping is in User_Setup.h
  tft.init();
  tft.setRotation(1); // landscape: 480 wide × 320 tall
  tft.fillScreen(COL_BG);

  // Touch – TFT_eSPI built-in driver; apply calibration data.
  tft.setTouch(const_cast<uint16_t*>(TOUCH_CAL));

  drawSplash("CDRLC Monitor", "Starting up...");

  Wire.begin();
  {
    unsigned long t = millis();
    while (millis() - t < 200)
    {
    }
  } // I2C settle

  if (WiFi.status() == WL_NO_MODULE)
  {
    drawSplash("WiFi Error", "No WiFi module found");
    while (true)
    {
    }
  }

  drawSplash("CDRLC Monitor", ("Connecting: " + String(SSID)).c_str());
  Serial.print(F("Connecting to WiFi: "));
  Serial.println(SSID);
  while (WiFi.status() != WL_CONNECTED)
  {
    WiFi.begin(SSID, PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000)
    {
    }
  }
  Serial.print(F("Connected. IP: "));
  Serial.println(WiFi.localIP());
  drawSplash("WiFi Connected!", WiFi.localIP().toString().c_str());
  {
    unsigned long t = millis();
    while (millis() - t < 1000)
    {
    }
  }

  timeClient.begin();
  timeClient.update();

  // Compute today's date string ("YYYYMMDD") from the NTP-adjusted epoch.
  // getEpochTime() already applies the UTC offset (-18000 = CDT/UTC-5),
  // so gmtime() gives the correct local date components.
  {
    time_t t = timeClient.getEpochTime();
    struct tm *ptm = gmtime(&t);
    snprintf(todayDate, sizeof(todayDate), "%04d%02d%02d",
             ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
    Serial.print(F("[Date] Today: ")); Serial.println(todayDate);
  }

  readDHT11();
  lastDHTRead = millis();

  fetchDates();
  prefetchAllDates();   // load all dates × all slots → all navigation instant after this
  fetchAndDisplay();    // cache hit → applies slot data and draws normal screen
}

void loop()
{
  unsigned long now = millis();

  // Read LDR → LED brightness for both wings.
  // Calibrated range:
  //   102  = finger pressed on sensor (darkest)  → brightness 40
  //   375  = dim bedroom (micro-light)            → brightness ~120
  //   850  = flashlight direct                   → brightness 255
  {
    static unsigned long lastBrightnessSync = 0;
    int ldr = analogRead(LDR_PIN);
    ledBrightness = (uint8_t)constrain(map(ldr, 102, 850, 40, 255), 40, 255);
    // Resend brightness + screen-state to both wings every second so LED
    // brightness tracks the LDR, and LEDs go dark when the screen sleeps.
    if (now - lastBrightnessSync >= 1000) {
      lastBrightnessSync = now;
      sendI2C(SLAVE_WING_A, 0x01, prevStatusA);
      sendI2C(SLAVE_WING_B, 0x01, prevStatusB);
    }
  }

  updateBacklight();
  buzzerUpdate();
  handleTouch();

  // BTN1 – advance to next time slot
  if (btnPressed(BTN1, 0))
  {
    markActive();
    Serial.print(F("[BTN1/D4] slotIdx: "));
    Serial.print(slotIdx); Serial.print(F(" -> "));
    slotIdx = (slotIdx + 1) % NUM_SLOTS;
    Serial.print(slotIdx);
    Serial.print(F("  ("));
    Serial.print(SLOT_LABELS[slotIdx]);
    Serial.println(F(")"));
    dispState = DISP_NORMAL;
    fetchAndDisplay();
  }

  // BTN2 – manual refresh  (non-blocking: call /refresh, wait 3 s, re-fetch)
  if (btnPressed(BTN2, 1) && !pendingRefresh)
  {
    markActive();
    dispState = DISP_NORMAL;
    pendingRefresh = true;
    refreshStart = now;
    drawHeaderWithBadge("refreshing...", COL_DATE);
    drawContentStatus(COL_TAKEN_BG, "Refreshing from server...", "Please wait");
    httpGet("/refresh");                              // tell server to rebuild its cache
    memset(slotCached, false, sizeof(slotCached));   // invalidate all local cached slots
  }
  if (pendingRefresh && now - refreshStart >= REFRESH_WAIT_MS)
  {
    pendingRefresh = false;
    fetchDates();
    prefetchAllDates();   // reload all dates × all slots
    fetchAndDisplay();    // cache hit → instant display update
  }

  // BTN3 – advance to next available date
  if (btnPressed(BTN3, 2) && numDates > 0)
  {
    markActive();
    Serial.print(F("[BTN3/D6] dateIdx: "));
    Serial.print(dateIdx); Serial.print(F(" -> "));
    dateIdx = (dateIdx + 1) % numDates;
    Serial.print(dateIdx);
    Serial.print(F("  ("));
    Serial.print(datePretty[dateIdx]);
    Serial.println(F(")"));
    dispState = DISP_NORMAL;
    fetchAndDisplay();
  }

  // Periodic DHT11 re-read (every 30 s)
  if (now - lastDHTRead >= DHT_READ_MS)
  {
    lastDHTRead = now;
    readDHT11();
    if (dispState == DISP_NORMAL)
      drawNormalScreen();
  }

  // Auto-refresh every 5 minutes: reload all dates × all slots so every
  // cached entry stays fresh.  prefetchAllDates() also resets lastAutoFetch.
  if (now - lastAutoFetch >= AUTO_FETCH_MS)
  {
    memset(slotCached, false, sizeof(slotCached));
    prefetchAllDates();
    fetchAndDisplay();    // cache hit → updates display with fresh data
  }
}
