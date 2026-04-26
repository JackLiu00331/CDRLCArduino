/*
  ================================================================
  Arduino 2 – Environment & Display Node  v3.2
  CDRLC Study Room Availability Monitor
  ================================================================
  Responsibilities:
    - Read DHT11 temperature & humidity every 2 seconds
    - Drive 5161AS 1-digit 7-seg directly → free room count (0–8)
    - I2C Slave at address 0x08

  I2C protocol:
    Master → this node  (Wire.write, 2 bytes):
      byte 0 : freeCount  (0–8, number of free rooms in current slot)
      byte 1 : slotIdx    (reserved)

    Master ← this node  (Wire.requestFrom(0x08, 4)):
      byte 0 : temperature integer part  (°C)
      byte 1 : temperature fractional    (tenths)
      byte 2 : relative humidity integer (%)
      byte 3 : checksum = byte0 ^ byte1 ^ byte2

  Wiring:
    D2   DHT11 data line (10 kΩ pull-up to 5 V)
    A4   I2C SDA
    A5   I2C SCL

  5161AS (common anode) direct wiring – each segment via 220Ω:
    Arduino D3  → 220Ω → pin7  (segment a – top)
    Arduino D4  → 220Ω → pin6  (segment b – top-right)
    Arduino D5  → 220Ω → pin4  (segment c – bottom-right)
    Arduino D6  → 220Ω → pin2  (segment d – bottom)
    Arduino D7  → 220Ω → pin1  (segment e – bottom-left)
    Arduino D8  → 220Ω → pin9  (segment f – top-left)
    Arduino D9  → 220Ω → pin10 (segment g – middle)
    5161AS COM  pin3 & pin8 → 5V  (common anode)

  5161AS pin layout (facing front, decimal point bottom-right):
    Bottom L→R : 1(e)  2(d)  3(COM)  4(c)  5(dp)
    Top    R→L : 6(b)  7(a)  8(COM)  9(f) 10(g)

  Libraries required:
    DHT sensor library by Adafruit
  ================================================================
*/

#include <Wire.h>
#include <DHT.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define SLAVE_ADDR    0x08
#define DHT_PIN       2

// 7-segment pins in segment order: a, b, c, d, e, f, g
const int SEG_PINS[7] = { 3, 4, 5, 6, 7, 8, 9 };

#define DHT_TYPE      DHT11
#define READ_INTERVAL 2000UL

DHT dht(DHT_PIN, DHT_TYPE);

// ── DHT11 readings ────────────────────────────────────────────────────────────
volatile uint8_t tInt  = 0;
volatile uint8_t tFrac = 0;
volatile uint8_t hInt  = 0;

// ── Display state ─────────────────────────────────────────────────────────────
volatile uint8_t dispFreeCount  = 0;
volatile bool    newDisplayData = false;

unsigned long lastRead = 0;

// ── 5161AS common-anode segment patterns [digit][a,b,c,d,e,f,g] ──────────────
// true = segment ON (Arduino outputs LOW for common anode)
const bool SEG7[10][7] = {
  {1,1,1,1,1,1,0},  // 0
  {0,1,1,0,0,0,0},  // 1
  {1,1,0,1,1,0,1},  // 2
  {1,1,1,1,0,0,1},  // 3
  {0,1,1,0,0,1,1},  // 4
  {1,0,1,1,0,1,1},  // 5
  {1,0,1,1,1,1,1},  // 6
  {1,1,1,0,0,0,0},  // 7
  {1,1,1,1,1,1,1},  // 8
  {1,1,1,1,0,1,1},  // 9
};

// ── 7-segment display helpers ─────────────────────────────────────────────────

void showDigit(uint8_t n) {
  if (n > 9) n = 9;
  for (int i = 0; i < 7; i++)
    digitalWrite(SEG_PINS[i], SEG7[n][i] ? LOW : HIGH);  // common anode: ON=LOW
}

void showDash() {
  // Only middle segment (g) on — used as startup placeholder
  for (int i = 0; i < 6; i++) digitalWrite(SEG_PINS[i], HIGH);
  digitalWrite(SEG_PINS[6], LOW);   // g = on
}

// ── I2C handlers ──────────────────────────────────────────────────────────────

void requestEvent() {
  uint8_t chk = tInt ^ tFrac ^ hInt;
  Wire.write(tInt);
  Wire.write(tFrac);
  Wire.write(hInt);
  Wire.write(chk);
}

void receiveEvent(int numBytes) {
  if (numBytes >= 2) {
    uint8_t fc = Wire.read();
    Wire.read();                        // slotIdx – reserved
    while (Wire.available()) Wire.read();
    dispFreeCount  = fc;
    newDisplayData = true;
  } else {
    while (Wire.available()) Wire.read();
  }
}

// ── Setup & Loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(9600);

  for (int i = 0; i < 7; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
    digitalWrite(SEG_PINS[i], HIGH);   // all segments off (common anode)
  }
  showDash();   // startup placeholder

  dht.begin();
  delay(2000);  // DHT11 warm-up

  Wire.begin(SLAVE_ADDR);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);

  Serial.println(F("[ENV] Environment & Display node ready at 0x08"));
}


void loop() {
  // Update 7-seg when master sends new free-count
  if (newDisplayData) {
    newDisplayData = false;
    showDigit(dispFreeCount);
    Serial.print(F("[DISP] free rooms = "));
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
