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
    0x0A  Arduino 4                                                                           : Wing B LEDs  (rooms 2426 2428 2430)

  Pin assignments:
    D4   Button 1 – next time slot
    D5   Button 2 – manual server refresh
    D6   Button 3 – next date
    D8   Passive buzzer
    D9   TFT RST
    D10  TFT CS
    D11  SPI MOSI  (TFT + Touch shared bus)
    D12  SPI MISO
    D13  SPI SCK
    A0   TFT DC (Data/Command)   [= digital pin 14 on R4]
    A1   XPT2046 Touch CS        [= digital pin 15 on R4]
    A4   I2C SDA
    A5   I2C SCL

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
#include <XPT2046_Touchscreen.h>
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
#define BTN1 4
#define BTN2 5
#define BTN3 6
#define BUZZER_PIN 8
#define TOUCH_CS A1 // XPT2046 chip-select (digital pin 15 on R4)

// ── TFT & touch objects ───────────────────────────────────────────────────────
// Physical pins are declared in User_Setup.h; TFT_eSPI reads that file.
TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS);

// Touch calibration – run the TFT_eSPI Touch_calibrate example sketch once
// to find your panel's true min/max ADC values and update these four defines.
#define TS_MINX 200
#define TS_MAXX 3800
#define TS_MINY 200
#define TS_MAXY 3800

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

// ── Slot data cache ───────────────────────────────────────────────────────────
// Avoids a network round-trip on every button press.
// cachedSlots[dateIdx][slotIdx] = 8-char status string ("10010100")
// slotCached[dateIdx][slotIdx]  = true once that slot has been fetched.
char cachedSlots[5][8][9];
bool slotCached[5][8];     // zero-initialised (all false) by default

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

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);

// ══════════════════════════════════════════════════════════════════════════════
// I2C helpers
// ══════════════════════════════════════════════════════════════════════════════

// Send 3-byte packet to a Wing LED slave: [ctrl, data, ctrl^data]
void sendI2C(uint8_t addr, uint8_t ctrl, uint8_t data)
{
  Wire.beginTransmission(addr);
  Wire.write(ctrl);
  Wire.write(data);
  Wire.write(ctrl ^ data);
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
  if (http.responseStatusCode() != 200)
    return "";
  String body = http.responseBody();
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
    const char *tag = roomFree[i] ? "Book >" : "Taken";
    int tw = tft.textWidth(tag, 2);
    int px = SCREEN_W - 92 + (82 - tw) / 2;
    int py = y + (ROW_H - 16) / 2;
    if (roomFree[i])
    {
      tft.fillRoundRect(SCREEN_W - 92, y + 6, 82, ROW_H - 13, 7, COL_FREE_FG);
      tft.setTextColor(COL_FREE_BG, COL_FREE_FG);
    }
    else
    {
      tft.setTextColor(COL_TAKEN_FG, COL_TAKEN_BG);
    }
    tft.drawString(tag, px, py, 2);
  }
}

// QR screen: full-screen QR code pointing to a room's Google Calendar page
void drawQRScreen(int roomIdx)
{
  // Version 9, ECC_LOW → max 154 bytes; our ~143-char URLs fit comfortably.
  // Buffer for V9: ceil(53×53 / 8) + 1 = 353 bytes; 400 is a safe upper bound.
  uint8_t qrData[400];
  QRCode qrc;
  if (!qrcode_initText(&qrc, qrData, 9, ECC_LOW, GCAL_URLS[roomIdx]))
  {
    tft.fillScreen(COL_BG);
    tftCenter("QR Error: URL too long", SCREEN_H / 2, 2, TFT_RED, COL_BG);
    return;
  }

  tft.fillScreen(COL_BG);

  // Header
  tft.fillRect(0, 0, SCREEN_W, 44, COL_HDR_BG);
  char hdr[32];
  snprintf(hdr, sizeof(hdr), "Room %s  -  Scan to Book", ROOM_NAMES[roomIdx]);
  tftCenter(hdr, 13, 2, COL_TITLE, COL_HDR_BG);

  // Centre QR in the space between header (44 px) and footer (28 px)
  const int SCALE = 4; // 53 × 4 = 212 px wide
  const int QR_PX = (int)qrc.size * SCALE;
  const int AVAIL = SCREEN_H - 44 - 28;
  const int xOff = (SCREEN_W - QR_PX) / 2;
  const int yOff = 44 + (AVAIL - QR_PX) / 2;

  // White quiet-zone background
  tft.fillRect(xOff - 8, yOff - 8, QR_PX + 16, QR_PX + 16, TFT_WHITE);

  // Dark modules
  tft.startWrite();
  for (int row = 0; row < (int)qrc.size; row++)
  {
    for (int col = 0; col < (int)qrc.size; col++)
    {
      if (qrcode_getModule(&qrc, col, row))
      {
        tft.fillRect(xOff + col * SCALE, yOff + row * SCALE, SCALE, SCALE, TFT_BLACK);
      }
    }
  }
  tft.endWrite();

  // Footer
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
  if (pendingRefresh) return;   // block all touch input while a server refresh is in progress
  if (!ts.touched())
    return;
  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_COOLDOWN)
    return;
  lastTouchTime = now;

  TS_Point p = ts.getPoint();
  int tx = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
  int ty = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
  Serial.print(F("[Touch] "));
  Serial.print(tx);
  Serial.print(',');
  Serial.println(ty);

  if (dispState == DISP_QR)
  {
    // Any tap on QR screen → return to room list
    dispState = DISP_NORMAL;
    drawNormalScreen();
    return;
  }

  if (dispState == DISP_NORMAL && ty >= HEADER_H)
  {
    int row = (ty - HEADER_H) / ROW_H;
    if (row >= 0 && row < 8 && roomFree[row])
    {
      Serial.print(F("[Touch] Book room "));
      Serial.println(ROOM_NAMES[row]);
      dispState = DISP_QR;
      drawQRScreen(row);
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
  dateIdx = (numDates > 1) ? 1 : 0;
  memset(slotCached, false, sizeof(slotCached));  // date list changed → invalidate all
  Serial.print(F("[Dates] got "));
  Serial.println(numDates);
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
  if (dispState == DISP_NORMAL) drawNormalScreen();
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
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);

  // TFT – pin mapping is in User_Setup.h
  tft.init();
  tft.setRotation(1); // landscape: 480 wide × 320 tall
  tft.fillScreen(COL_BG);

  // Touch controller – shares the same SPI bus
  ts.begin();

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

  readDHT11();
  lastDHTRead = millis();

  drawSplash("Loading...", "Getting availability");
  fetchDates();
  fetchAndDisplay(); // first data fetch → shows its own loading overlay then drawNormalScreen()
}

void loop()
{
  unsigned long now = millis();

  buzzerUpdate();
  handleTouch();

  // BTN1 – advance to next time slot
  if (btnPressed(BTN1, 0))
  {
    dispState = DISP_NORMAL;
    slotIdx = (slotIdx + 1) % NUM_SLOTS;
    fetchAndDisplay();
  }

  // BTN2 – manual refresh  (non-blocking: call /refresh, wait 3 s, re-fetch)
  if (btnPressed(BTN2, 1) && !pendingRefresh)
  {
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
    drawHeaderWithBadge("loading...", COL_DATE);
    drawContentStatus(COL_TAKEN_BG, "Getting availability...", "Please wait");
    fetchDates();
    fetchAndDisplay();
  }

  // BTN3 – advance to next available date
  if (btnPressed(BTN3, 2) && numDates > 0)
  {
    dispState = DISP_NORMAL;
    dateIdx = (dateIdx + 1) % numDates;
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

  // Auto-refresh every 5 minutes
  // Only invalidate current slot so other cached slots stay fast.
  if (now - lastAutoFetch >= AUTO_FETCH_MS)
  {
    slotCached[dateIdx][slotIdx] = false;
    fetchAndDisplay();
  }
}
