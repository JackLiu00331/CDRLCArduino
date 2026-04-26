/*
  ================================================================
  Arduino 2 – Environment & Display Node  v3.0
  CDRLC Study Room Availability Monitor
  ================================================================
  Responsibilities:
    - Read DHT11 temperature & humidity every 2 seconds
    - Drive TM1637 4-digit display  → current time slot (e.g. 09:30)
    - Drive 74HC595 + 1-digit 7-seg → free room count for that slot
    - I2C Slave at address 0x08

  I2C protocol:
    Master → this node  (Wire.write, 2 bytes):
      byte 0 : freeCount   (0–8, number of free rooms)
      byte 1 : slotIdx     (0–7, index into time-slot table)

    Master ← this node  (Wire.requestFrom(0x08, 4)):
      byte 0 : temperature integer part  (°C, e.g. 22)
      byte 1 : temperature fractional    (tenths, e.g. 5 → 22.5 °C)
      byte 2 : relative humidity integer (%)
      byte 3 : checksum = byte0 ^ byte1 ^ byte2

  Wiring:
    D2   DHT11 data line (10 kΩ pull-up to 5 V)
    D3   TM1637 CLK
    D4   TM1637 DIO
    D5   74HC595 DS   (serial data)
    D6   74HC595 SHCP (shift clock)
    D7   74HC595 STCP (latch)
    A4   I2C SDA  (shared bus with Arduinos 1, 3, 4)
    A5   I2C SCL

  TM1637 wiring:
    VCC → 5V,  GND → GND,  CLK → D3,  DIO → D4

  74HC595 + 1-digit 7-seg wiring (common cathode):
    74HC595 QA–QG → 7-seg segments a–g
    74HC595 QH    → 7-seg dp (decimal point, unused)
    7-seg common cathode → GND (via 220Ω resistor)
    74HC595 VCC/MR → 5V,  GND/OE → GND

  Libraries required:
    DHT sensor library  by Adafruit
    (Adafruit Unified Sensor installs automatically as a dependency)
    TM1637 is bit-banged manually — no library needed.
  ================================================================
*/

#include <Wire.h>
#include <DHT.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define SLAVE_ADDR   0x08
#define DHT_PIN      2
#define TM_CLK       3    // TM1637 clock
#define TM_DIO       4    // TM1637 data
#define HC_DATA      5    // 74HC595 DS   (serial data in)
#define HC_CLK       6    // 74HC595 SHCP (shift register clock)
#define HC_LATCH     7    // 74HC595 STCP (storage register clock / latch)

#define DHT_TYPE       DHT11
#define READ_INTERVAL  2000UL   // DHT11 minimum safe poll interval

DHT dht(DHT_PIN, DHT_TYPE);

// ── DHT11 readings (written in loop, read in I2C ISR) ────────────────────────
volatile uint8_t tInt  = 0;
volatile uint8_t tFrac = 0;
volatile uint8_t hInt  = 0;

// ── Display state (written in I2C ISR, read in loop) ─────────────────────────
volatile uint8_t dispFreeCount  = 0;
volatile uint8_t dispSlotIdx    = 0;
volatile bool    newDisplayData = false;

unsigned long lastRead = 0;

// ── 7-segment digit patterns (common cathode, Q0=a Q1=b Q2=c Q3=d Q4=e Q5=f Q6=g)
const uint8_t SEG7[10] = {
  0x3F,  // 0
  0x06,  // 1
  0x5B,  // 2
  0x4F,  // 3
  0x66,  // 4
  0x6D,  // 5
  0x7D,  // 6
  0x07,  // 7
  0x7F,  // 8
  0x6F,  // 9
};

// ── 4 digits for each of the 8 time slots ────────────────────────────────────
// Format: {d0, d1, d2, d3} → displays as "d0 d1 : d2 d3"
// (colon bit is set automatically on d1)
const uint8_t SLOT_DIGITS[8][4] = {
  {0, 9, 3, 0},  // 09:30
  {1, 0, 3, 0},  // 10:30
  {1, 1, 3, 0},  // 11:30
  {1, 2, 3, 0},  // 12:30
  {1, 3, 3, 0},  // 13:30
  {1, 4, 3, 0},  // 14:30
  {1, 5, 3, 0},  // 15:30
  {1, 6, 3, 0},  // 16:30
};

// ══════════════════════════════════════════════════════════════════════════════
// TM1637 bit-bang driver
// Protocol: I2C-like but NOT compatible with Wire — must be bit-banged.
// ══════════════════════════════════════════════════════════════════════════════

void tm_start() {
  digitalWrite(TM_DIO, HIGH);
  digitalWrite(TM_CLK, HIGH);
  delayMicroseconds(2);
  digitalWrite(TM_DIO, LOW);
  delayMicroseconds(2);
  digitalWrite(TM_CLK, LOW);
}

void tm_stop() {
  digitalWrite(TM_CLK, LOW);
  delayMicroseconds(2);
  digitalWrite(TM_DIO, LOW);
  delayMicroseconds(2);
  digitalWrite(TM_CLK, HIGH);
  delayMicroseconds(2);
  digitalWrite(TM_DIO, HIGH);
}

// Send one byte LSB-first; wait for ACK pulse.
void tm_writeByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(TM_CLK, LOW);
    digitalWrite(TM_DIO, (b >> i) & 0x01 ? HIGH : LOW);
    delayMicroseconds(2);
    digitalWrite(TM_CLK, HIGH);
    delayMicroseconds(2);
  }
  // ACK: TM1637 pulls DIO low for one clock pulse
  digitalWrite(TM_CLK, LOW);
  pinMode(TM_DIO, INPUT);       // release DIO so TM1637 can pull it
  delayMicroseconds(2);
  digitalWrite(TM_CLK, HIGH);
  delayMicroseconds(2);
  digitalWrite(TM_CLK, LOW);
  pinMode(TM_DIO, OUTPUT);      // reclaim DIO
}

// Write 4 digits to TM1637 with colon on (bit 7 of digit 1).
// brightness: 0 (min) – 7 (max)
void tm_show(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t brightness) {
  // Command 1: data write, auto-increment address
  tm_start();
  tm_writeByte(0x40);
  tm_stop();

  // Command 2: set start address 0x00, write all 4 digits
  tm_start();
  tm_writeByte(0xC0);
  tm_writeByte(SEG7[d0]);
  tm_writeByte(SEG7[d1] | 0x80);   // 0x80 = colon on
  tm_writeByte(SEG7[d2]);
  tm_writeByte(SEG7[d3]);
  tm_stop();

  // Command 3: display on + brightness (0x88 = on, | brightness 0–7)
  tm_start();
  tm_writeByte(0x88 | (brightness & 0x07));
  tm_stop();
}

// Show "----" on TM1637 (used during startup before data arrives)
void tm_showDash() {
  tm_start(); tm_writeByte(0x40); tm_stop();
  tm_start();
  tm_writeByte(0xC0);
  for (int i = 0; i < 4; i++) tm_writeByte(0x40);  // 0x40 = middle dash segment
  tm_stop();
  tm_start(); tm_writeByte(0x8F); tm_stop();   // on, max brightness
}

// ══════════════════════════════════════════════════════════════════════════════
// 74HC595 + 1-digit 7-segment driver
// ══════════════════════════════════════════════════════════════════════════════

void hc_shift(uint8_t segments) {
  digitalWrite(HC_LATCH, LOW);
  shiftOut(HC_DATA, HC_CLK, MSBFIRST, segments);
  digitalWrite(HC_LATCH, HIGH);
}

// ══════════════════════════════════════════════════════════════════════════════
// I2C handlers
// ══════════════════════════════════════════════════════════════════════════════

// Master reads DHT11 data from us
void requestEvent() {
  uint8_t chk = tInt ^ tFrac ^ hInt;
  Wire.write(tInt);
  Wire.write(tFrac);
  Wire.write(hInt);
  Wire.write(chk);
}

// Master sends display data to us: [freeCount, slotIdx]
void receiveEvent(int numBytes) {
  if (numBytes >= 2) {
    uint8_t fc = Wire.read();
    uint8_t si = Wire.read();
    while (Wire.available()) Wire.read();   // discard any extra bytes
    if (si < 8) {
      dispFreeCount  = fc;
      dispSlotIdx    = si;
      newDisplayData = true;
    }
  } else {
    while (Wire.available()) Wire.read();
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// Setup & Loop
// ══════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(9600);

  // 74HC595
  pinMode(HC_DATA,  OUTPUT);
  pinMode(HC_CLK,   OUTPUT);
  pinMode(HC_LATCH, OUTPUT);

  // TM1637
  pinMode(TM_CLK, OUTPUT);
  pinMode(TM_DIO, OUTPUT);

  // Startup placeholder display
  tm_showDash();         // 4-digit: "----"
  hc_shift(0x40);        // 1-digit: middle dash

  dht.begin();
  delay(2000);           // DHT11 warm-up

  Wire.begin(SLAVE_ADDR);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);

  Serial.println(F("[ENV] Environment & Display node ready at 0x08"));
}


void loop() {
  // Update both displays when master sends new slot/count data
  if (newDisplayData) {
    newDisplayData = false;

    uint8_t si = dispSlotIdx;
    tm_show(SLOT_DIGITS[si][0], SLOT_DIGITS[si][1],
            SLOT_DIGITS[si][2], SLOT_DIGITS[si][3], 7);

    uint8_t fc = dispFreeCount < 10 ? dispFreeCount : 9;
    hc_shift(SEG7[fc]);

    Serial.print(F("[DISP] slot="));
    Serial.print(si);
    Serial.print(F("  free="));
    Serial.println(dispFreeCount);
  }

  // DHT11 read every 2 seconds
  unsigned long now = millis();
  if (now - lastRead < READ_INTERVAL) return;
  lastRead = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) {
    uint8_t ti = (uint8_t)t;
    uint8_t tf = (uint8_t)((t - (float)ti) * 10.0f + 0.5f);
    if (tf > 9) { tf = 0; ti++; }
    tInt  = ti;
    tFrac = tf;
  }
  if (!isnan(h)) {
    hInt = (uint8_t)(h + 0.5f);
  }

  if (isnan(t) || isnan(h)) {
    Serial.println(F("[DHT] read failed – check wiring & pull-up resistor"));
  } else {
    Serial.print(F("[DHT] "));
    Serial.print(t, 1); Serial.print(F("C  "));
    Serial.print(h, 0); Serial.println('%');
  }
}
