/*
  ================================================================
  Arduino 1 – Master Node  v5.1-LCD
  CDRLC Study Room Availability Monitor
  ================================================================
  LCD FALLBACK / DEVELOPMENT BUILD

  Use this sketch while the 4" TFT touch screen (Hosyond ST7796S)
  has not yet arrived. It is functionally identical to the TFT
  version (v5.0) in all server, I2C, button, and buzzer behaviour,
  but replaces TFT + touch + QR-code output with a 16×2
  LiquidCrystal LCD display.

  Swap back to src/Arduino/master_node/master_node.ino (TFT v5.0)
  once the screen is in hand and calibrated.

  ── LCD wiring (4-bit parallel, HD44780-compatible) ──────────────
    LCD pin  1  VSS   → GND
    LCD pin  2  VDD   → 5 V
    LCD pin  3  V0    → wiper of 10 kΩ pot (adjust contrast)
                         or a fixed 1 kΩ resistor to GND works too
    LCD pin  4  RS    → Arduino D7
    LCD pin  5  RW    → GND  (write-only mode)
    LCD pin  6  EN    → Arduino D9
    LCD pin 11  DB4   → Arduino D10
    LCD pin 12  DB5   → Arduino D11
    LCD pin 13  DB6   → Arduino D12
    LCD pin 14  DB7   → Arduino D13
    LCD pin 15  A     → 5 V through 33 Ω resistor  (backlight +)
    LCD pin 16  K     → GND  (backlight −)

  Note: The R4 WiFi outputs 3.3 V logic. Most HD44780-compatible
  modules accept ≥2.2 V as HIGH and work correctly with 3.3 V
  signals even when powered at 5 V.

  ── Other pins (same as TFT build) ───────────────────────────────
    D4   Button 1 – cycle to next time slot
    D5   Button 2 – force server refresh
    D6   Button 3 – cycle to next date
    D8   Passive buzzer
    A4   I2C SDA  (shared bus with Arduinos 2, 3, 4)
    A5   I2C SCL

  ── Display layout ───────────────────────────────────────────────
    Line 1  (static):  "04/28 0930 22.5C"
                        date  slot  temperature
    Line 2  (cycles every 2.5 s through 4 pages):
      page 0 → "2432:F  2434:B  "
      page 1 → "2436:F  2438:B  "
      page 2 → "2440:F  2426:B  "
      page 3 → "2428:F  2430:B  "
    F = Free (available),  B = Busy (booked)

  ── Button behaviour ─────────────────────────────────────────────
    BTN1 (D4) – advance time slot: 09:30 → 10:30 → … → 16:30 → 09:30
    BTN2 (D5) – force immediate re-fetch from server
    BTN3 (D6) – advance to next available weekday

  ── I2C slaves (unchanged from TFT build) ────────────────────────
    0x08  Arduino 2 – DHT11 requestFrom(4 bytes)
    0x09  Arduino 3 – Wing A bi-color LEDs  (rooms 2432–2440)
    0x0A  Arduino 4 – Wing B bi-color LEDs  (rooms 2426–2430)

  ── Libraries needed ─────────────────────────────────────────────
    LiquidCrystal      (built-in with Arduino IDE)
    NTPClient          by Fabrice Weinberg
    ArduinoHttpClient  by Arduino
  ================================================================
*/

#include <Wire.h>
#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LiquidCrystal.h>

// ── WiFi & server ─────────────────────────────────────────────────────────────
const char *SSID       = "NETGEAR77";                         // ← change
const char *PASSWORD   = "pinkbutter932";                     // ← change
const char *SERVER_IP  = "zz-cloud.tail6b9dfa.ts.net";       // Tailscale Funnel hostname
const int   SERVER_PORT = 443;

// ── I2C slave addresses ───────────────────────────────────────────────────────
#define SLAVE_ENV    0x08   // Arduino 2 – DHT11
#define SLAVE_WING_A 0x09   // Arduino 3 – Wing A LEDs
#define SLAVE_WING_B 0x0A   // Arduino 4 – Wing B LEDs

// ── Pins ──────────────────────────────────────────────────────────────────────
#define BTN1       4    // next time slot
#define BTN2       5    // manual server refresh
#define BTN3       6    // next date
#define BUZZER_PIN 8

// LiquidCrystal(RS, EN, D4, D5, D6, D7)
// Using D7/D9/D10-D13 — no conflict with buttons (D4-D6) or buzzer (D8).
LiquidCrystal lcd(7, 9, 10, 11, 12, 13);

// ── Time slots ────────────────────────────────────────────────────────────────
const char *SLOT_LABELS[] = {"09:30","10:30","11:30","12:30",
                              "13:30","14:30","15:30","16:30"};
const char *SLOT_PARAMS[] = {"0930", "1030", "1130", "1230",
                              "1330", "1430", "1530", "1630"};
const int NUM_SLOTS = 8;

// ── Room names ────────────────────────────────────────────────────────────────
// Order matches the 8-char availability string from the server.
const char ROOM_NAMES[8][5] = {
  "2432","2434","2436","2438","2440","2426","2428","2430"
};

// ── Global state ──────────────────────────────────────────────────────────────
char dateList[5][9];     // raw "YYYYMMDD"
char datePretty[5][6];   // "MM/DD" for display
int  numDates = 0;
int  dateIdx  = 1;       // default: tomorrow (index 1)
int  slotIdx  = 0;

bool    roomFree[8]  = {};
uint8_t prevStatusA  = 0xFF;
uint8_t prevStatusB  = 0xFF;

unsigned long lastAutoFetch  = 0;
const unsigned long AUTO_FETCH_MS = 300000UL;   // 5 minutes

// ── DHT11 cache (read from Arduino 2 via I2C) ─────────────────────────────────
float cachedTemp = NAN;
float cachedHumi = NAN;
unsigned long lastDHTRead = 0;
const unsigned long DHT_READ_MS = 30000UL;      // 30 seconds

// ── LCD page cycling ──────────────────────────────────────────────────────────
// 8 rooms shown 2 per page → 4 pages total, rotating automatically.
int  lcdPage = 0;
unsigned long lastLCDPage = 0;
const unsigned long LCD_PAGE_MS = 2500UL;        // 2.5 s per page

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
const unsigned long REFRESH_WAIT_MS = 3000UL;   // wait 3 s after /refresh call

// ── Non-blocking two-tone buzzer ──────────────────────────────────────────────
enum BuzzState { BUZZ_IDLE, BUZZ_NOTE1, BUZZ_NOTE2 };
BuzzState buzzState = BUZZ_IDLE;
unsigned long buzzStart = 0;

void buzzerTrigger() {
  if (buzzState != BUZZ_IDLE) return;
  buzzState = BUZZ_NOTE1;
  buzzStart = millis();
  tone(BUZZER_PIN, 880, 200);
}
void buzzerUpdate() {
  if (buzzState == BUZZ_IDLE) return;
  unsigned long now = millis();
  if (buzzState == BUZZ_NOTE1 && now - buzzStart >= 250) {
    buzzState = BUZZ_NOTE2; buzzStart = now;
    tone(BUZZER_PIN, 1047, 400);
  } else if (buzzState == BUZZ_NOTE2 && now - buzzStart >= 400) {
    buzzState = BUZZ_IDLE;
  }
}

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);

// ══════════════════════════════════════════════════════════════════════════════
// LCD helpers
// ══════════════════════════════════════════════════════════════════════════════

// Write exactly 16 characters to a row (left-pads with spaces if shorter).
void lcdRow(int row, const char *text) {
  lcd.setCursor(0, row);
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.print(buf);
}

// Line 1: "MM/DD HHMM ##.#C"  (date | slot | temperature)
// Total: 5+1+4+1+5 = 16 characters.
void lcdDrawHeader() {
  char line[17];
  if (numDates > 0 && !isnan(cachedTemp)) {
    // e.g. "04/28 0930 22.5C"
    snprintf(line, sizeof(line), "%s %s %4.1fC",
             datePretty[dateIdx], SLOT_PARAMS[slotIdx], cachedTemp);
  } else if (numDates > 0) {
    // No temperature yet: show date + slot with blank temp field
    snprintf(line, sizeof(line), "%s %s  --.-C",
             datePretty[dateIdx], SLOT_PARAMS[slotIdx]);
  } else {
    snprintf(line, sizeof(line), "Loading...     ");
  }
  lcdRow(0, line);
}

// Line 2: two rooms for the current page.
//   page 0 → rooms[0..1]   (2432, 2434)
//   page 1 → rooms[2..3]   (2436, 2438)
//   page 2 → rooms[4..5]   (2440, 2426)
//   page 3 → rooms[6..7]   (2428, 2430)
// Format: "2432:F  2434:B  " (16 chars)
void lcdDrawRoomPage() {
  int a = lcdPage * 2;
  int b = a + 1;
  if (a >= 8) { lcdRow(1, "                "); return; }

  char sa = roomFree[a] ? 'F' : 'B';
  char line[17];
  if (b < 8) {
    char sb = roomFree[b] ? 'F' : 'B';
    // "2432:F  2434:B  " = 4+1+1+2+4+1+1+2 = 16
    snprintf(line, sizeof(line), "%s:%c  %s:%c", ROOM_NAMES[a], sa, ROOM_NAMES[b], sb);
  } else {
    snprintf(line, sizeof(line), "%s:%c", ROOM_NAMES[a], sa);
  }
  lcdRow(1, line);
}

// Call every loop(): advances the page if the timer has elapsed.
// Frozen during pendingRefresh so the "Refreshing…" message is not overwritten.
void lcdPageTick() {
  if (pendingRefresh) return;
  if (millis() - lastLCDPage >= LCD_PAGE_MS) {
    lastLCDPage = millis();
    lcdPage = (lcdPage + 1) % 4;
    lcdDrawRoomPage();
  }
}

// Full LCD redraw — call after any data change or button press.
void lcdRefresh() {
  lcdPage = 0;
  lastLCDPage = millis();
  lcdDrawHeader();
  lcdDrawRoomPage();
}

// ══════════════════════════════════════════════════════════════════════════════
// I2C helpers
// ══════════════════════════════════════════════════════════════════════════════

// Send 3-byte packet to a Wing LED slave: [ctrl, data, ctrl^data]
void sendI2C(uint8_t addr, uint8_t ctrl, uint8_t data) {
  Wire.beginTransmission(addr);
  Wire.write(ctrl);
  Wire.write(data);
  Wire.write(ctrl ^ data);
  Wire.endTransmission();
}

// Request 4 bytes from Arduino 2: [tempInt, tempFrac, humiInt, checksum]
void readDHT11() {
  if (Wire.requestFrom((uint8_t)SLAVE_ENV, (uint8_t)4) < 4) return;
  uint8_t tI = Wire.read(), tF = Wire.read();
  uint8_t hI = Wire.read(), ck = Wire.read();
  if (ck != (uint8_t)(tI ^ tF ^ hI)) {
    Serial.println(F("[DHT] checksum fail")); return;
  }
  cachedTemp = tI + tF * 0.1f;
  cachedHumi = (float)hI;
  Serial.print(F("[DHT] "));
  Serial.print(cachedTemp, 1); Serial.print(F("C  "));
  Serial.print(cachedHumi, 0); Serial.println('%');
}

// ══════════════════════════════════════════════════════════════════════════════
// HTTP helper (HTTPS via WiFiSSLClient)
// ══════════════════════════════════════════════════════════════════════════════

WiFiSSLClient wifiClient;

String httpGet(const String &path) {
  if (WiFi.status() != WL_CONNECTED) return "";
  Serial.print(F("[HTTP] GET ")); Serial.println(path);
  HttpClient http(wifiClient, SERVER_IP, SERVER_PORT);
  http.connectionKeepAlive();
  if (http.get(path) != 0) return "";
  if (http.responseStatusCode() != 200) return "";
  String body = http.responseBody();
  body.trim();
  return body;
}

// ══════════════════════════════════════════════════════════════════════════════
// Date helpers
// ══════════════════════════════════════════════════════════════════════════════

// "YYYYMMDD" → "MM/DD" (compact form that fits on 16-char LCD)
void makePretty(const char *d, char *out) {
  snprintf(out, 6, "%c%c/%c%c", d[4], d[5], d[6], d[7]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Data fetch
// ══════════════════════════════════════════════════════════════════════════════

void fetchDates() {
  String raw = httpGet("/dates");
  if (!raw.length()) return;
  numDates = 0;
  int start = 0;
  while (start < (int)raw.length() && numDates < 5) {
    int comma = raw.indexOf(',', start);
    String tok = (comma < 0) ? raw.substring(start) : raw.substring(start, comma);
    tok.trim();
    if (tok.length() == 8) {
      strncpy(dateList[numDates], tok.c_str(), 8);
      dateList[numDates][8] = '\0';
      makePretty(dateList[numDates], datePretty[numDates]);
      numDates++;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  dateIdx = (numDates > 1) ? 1 : 0;
  memset(slotCached, false, sizeof(slotCached));  // date list changed → invalidate all
  Serial.print(F("[Dates] got ")); Serial.println(numDates);
}

// Apply a slot data string to LEDs and LCD (shared by cache-hit and network paths).
void applySlotData(const char *data) {
  uint8_t statusA = 0, statusB = 0;
  for (int i = 0; i < 8; i++) {
    roomFree[i] = (data[i] == '0');
    if (data[i] == '1') {
      if (i < 5) statusA |= (1 << i);
      else       statusB |= (1 << (i - 5));
    }
  }
  sendI2C(SLAVE_WING_A, 0x01, statusA);
  sendI2C(SLAVE_WING_B, 0x01, statusB);
  if ((prevStatusA & ~statusA) || (prevStatusB & ~statusB)) buzzerTrigger();
  prevStatusA = statusA;
  prevStatusB = statusB;
  lcdRefresh();
}

void fetchAndDisplay() {
  if (numDates == 0) return;

  // ── Cache hit: instant display, no network call ───────────────────────────
  if (slotCached[dateIdx][slotIdx]) {
    Serial.print(F("[Cache] ")); Serial.print(dateList[dateIdx]);
    Serial.print(' '); Serial.println(SLOT_PARAMS[slotIdx]);
    applySlotData(cachedSlots[dateIdx][slotIdx]);
    return;
  }

  // ── Cache miss: show loading indicator, then fetch ────────────────────────
  // We tell the user what we're doing before the blocking HTTP call.
  {
    char loadHdr[17];
    // Line 0: keep date + slot visible so user sees what they requested;
    //         replace temp field with "  ..." to signal "pending".
    snprintf(loadHdr, sizeof(loadHdr), "%-5s %-4s   ...",
             (numDates > 0 ? datePretty[dateIdx] : "??/??"),
             SLOT_PARAMS[slotIdx]);
    lcdRow(0, loadHdr);
    lcdRow(1, "Fetching data...");
  }

  String resp = httpGet(
    String("/slot?date=") + dateList[dateIdx] + "&time=" + SLOT_PARAMS[slotIdx]);
  if (resp.length() != 8) {
    // Network / server error – show brief message then restore previous display.
    lcdRow(1, "Network error!  ");
    unsigned long t = millis();
    while (millis() - t < 1500) {}   // let user read the error (1.5 s)
    lcdRefresh();                     // redraw last-known data
    return;
  }

  strncpy(cachedSlots[dateIdx][slotIdx], resp.c_str(), 8);
  cachedSlots[dateIdx][slotIdx][8] = '\0';
  slotCached[dateIdx][slotIdx] = true;

  applySlotData(cachedSlots[dateIdx][slotIdx]);
  lastAutoFetch = millis();   // only reset the 5-min timer on real network fetch
  Serial.print(F("[Slot] fetched ")); Serial.print(resp);
  Serial.print(F("  ")); Serial.print(dateList[dateIdx]);
  Serial.print(' '); Serial.println(SLOT_PARAMS[slotIdx]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Button helper
// ══════════════════════════════════════════════════════════════════════════════

bool btnPressed(int pin, int idx) {
  if (digitalRead(pin) == LOW && millis() - btnTime[idx] > DEBOUNCE) {
    btnTime[idx] = millis();
    return true;
  }
  return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Setup
// ══════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcdRow(0, "CDRLC Monitor");
  lcdRow(1, "Starting up...");

  Wire.begin();
  {
    unsigned long t = millis();
    while (millis() - t < 200) {}  // I2C bus settle
  }

  if (WiFi.status() == WL_NO_MODULE) {
    lcdRow(0, "ERROR: No WiFi");
    lcdRow(1, "Check hardware");
    while (true) {}
  }

  lcdRow(0, "WiFi: connecting");
  lcdRow(1, SSID);
  Serial.print(F("Connecting to ")); Serial.println(SSID);

  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(SSID, PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {}
  }
  Serial.print(F("Connected. IP: ")); Serial.println(WiFi.localIP());

  lcdRow(0, "WiFi Connected!");
  {
    char ip[17];
    WiFi.localIP().toString().toCharArray(ip, 17);
    lcdRow(1, ip);
  }
  {
    unsigned long t = millis();
    while (millis() - t < 1500) {}  // show IP for 1.5 s
  }

  timeClient.begin();
  timeClient.update();

  readDHT11();
  lastDHTRead = millis();

  lcdRow(0, "Getting dates...");
  lcdRow(1, "Please wait...  ");
  fetchDates();
  // fetchAndDisplay() will show its own "Fetching data..." for the first slot
  fetchAndDisplay();
}

// ══════════════════════════════════════════════════════════════════════════════
// Main loop
// ══════════════════════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  buzzerUpdate();     // non-blocking buzzer state machine
  lcdPageTick();      // auto-advance room page every 2.5 s

  // ── BTN1 – advance to next time slot ──────────────────────────────────────
  if (btnPressed(BTN1, 0)) {
    slotIdx = (slotIdx + 1) % NUM_SLOTS;
    fetchAndDisplay();
  }

  // ── BTN2 – manual refresh (non-blocking) ──────────────────────────────────
  if (btnPressed(BTN2, 1) && !pendingRefresh) {
    pendingRefresh = true;
    refreshStart   = now;
    lcdRow(0, "Refreshing...   ");
    lcdRow(1, "Please wait...  ");     // freeze page cycling + inform user
    httpGet("/refresh");               // tell server to rebuild its cache
    memset(slotCached, false, sizeof(slotCached));  // invalidate our local cache
  }
  if (pendingRefresh && now - refreshStart >= REFRESH_WAIT_MS) {
    pendingRefresh = false;
    lcdRow(0, "Getting dates...");
    lcdRow(1, "Please wait...  ");
    fetchDates();
    fetchAndDisplay();
  }

  // ── BTN3 – advance to next date ───────────────────────────────────────────
  if (btnPressed(BTN3, 2) && numDates > 0) {
    dateIdx = (dateIdx + 1) % numDates;
    fetchAndDisplay();
  }

  // ── Periodic DHT11 re-read (every 30 s) ───────────────────────────────────
  if (now - lastDHTRead >= DHT_READ_MS) {
    lastDHTRead = now;
    readDHT11();
    lcdDrawHeader();  // refresh line 1 with new temperature
  }

  // ── Auto-refresh every 5 minutes ──────────────────────────────────────────
  // Only invalidate the currently displayed slot so other cached slots stay fast.
  if (now - lastAutoFetch >= AUTO_FETCH_MS) {
    slotCached[dateIdx][slotIdx] = false;
    fetchAndDisplay();
  }
}
