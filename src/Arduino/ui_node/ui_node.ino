/*
  ================================================================
  Arduino 2 – Environment & Display Node  v3.1
  CDRLC Study Room Availability Monitor
  ================================================================
  Responsibilities:
    - Read DHT11 temperature & humidity every 2 seconds
    - Drive 74HC595 + 5161AS 1-digit 7-seg → free room count (0–8)
    - I2C Slave at address 0x08

  I2C protocol:
    Master → this node  (Wire.write, 2 bytes):
      byte 0 : freeCount  (0–8, number of free rooms in current slot)
      byte 1 : slotIdx    (0–7, reserved for future use)

    Master ← this node  (Wire.requestFrom(0x08, 4)):
      byte 0 : temperature integer part  (°C, e.g. 22)
      byte 1 : temperature fractional    (tenths, e.g. 5 → 22.5 °C)
      byte 2 : relative humidity integer (%)
      byte 3 : checksum = byte0 ^ byte1 ^ byte2

  Wiring:
    D2   DHT11 data line (10 kΩ pull-up to 5 V)
    D5   74HC595 DS   (serial data in)
    D6   74HC595 SHCP (shift clock)
    D7   74HC595 STCP (latch)
    A4   I2C SDA  (shared bus with Arduinos 1, 3, 4)
    A5   I2C SCL

  74HC595 + 5161AS wiring (COMMON ANODE):
    74HC595 QA (pin15) → 220Ω → 5161AS pin7  (segment a)
    74HC595 QB (pin 1) → 220Ω → 5161AS pin6  (segment b)
    74HC595 QC (pin 2) → 220Ω → 5161AS pin4  (segment c)
    74HC595 QD (pin 3) → 220Ω → 5161AS pin2  (segment d)
    74HC595 QE (pin 4) → 220Ω → 5161AS pin1  (segment e)
    74HC595 QF (pin 5) → 220Ω → 5161AS pin9  (segment f)
    74HC595 QG (pin 6) → 220Ω → 5161AS pin10 (segment g)
    5161AS COM (pin3 & pin8) → 5V   ← common anode
    74HC595 VCC / MR → 5V,  GND / OE → GND

  5161AS pin layout (facing front, decimal point bottom-right):
    Bottom L→R : 1(e)  2(d)  3(COM)  4(c)  5(dp)
    Top    R→L : 6(b)  7(a)  8(COM)  9(f) 10(g)

  Libraries required:
    DHT sensor library  by Adafruit
    (Adafruit Unified Sensor installs automatically as a dependency)
  ================================================================
*/

#include <Wire.h>
#include <DHT.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define SLAVE_ADDR   0x08
#define DHT_PIN      2
#define HC_DATA      5    // 74HC595 DS   (serial data in)
#define HC_CLK       6    // 74HC595 SHCP (shift register clock)
#define HC_LATCH     7    // 74HC595 STCP (storage register / latch)

#define DHT_TYPE      DHT11
#define READ_INTERVAL 2000UL   // DHT11 minimum safe poll interval

DHT dht(DHT_PIN, DHT_TYPE);

// ── DHT11 readings (written in loop, read in I2C ISR) ────────────────────────
volatile uint8_t tInt  = 0;
volatile uint8_t tFrac = 0;
volatile uint8_t hInt  = 0;

// ── Display state (written in I2C ISR, consumed in loop) ─────────────────────
volatile uint8_t dispFreeCount  = 0;
volatile bool    newDisplayData = false;

unsigned long lastRead = 0;

// ── 5161AS common-anode digit patterns ───────────────────────────────────────
// Wiring: bit0→QA→a, bit1→QB→b, ..., bit6→QG→g  (shiftOut LSBFIRST)
// Common anode: segment ON when output LOW → all bits inverted vs common cathode
const uint8_t SEG7[10] = {
  0xC0,  // 0
  0xF9,  // 1
  0xA4,  // 2
  0xB0,  // 3
  0x99,  // 4
  0x92,  // 5
  0x82,  // 6
  0xF8,  // 7
  0x80,  // 8
  0x90,  // 9
};

// ── 74HC595 driver ────────────────────────────────────────────────────────────
void hc_shift(uint8_t segments) {
  digitalWrite(HC_LATCH, LOW);
  shiftOut(HC_DATA, HC_CLK, LSBFIRST, segments);  // bit0→QA→segment-a
  digitalWrite(HC_LATCH, HIGH);
}

// ── I2C handlers ──────────────────────────────────────────────────────────────

// Master reads DHT11 data from us
void requestEvent() {
  uint8_t chk = tInt ^ tFrac ^ hInt;
  Wire.write(tInt);
  Wire.write(tFrac);
  Wire.write(hInt);
  Wire.write(chk);
}

// Master sends display data: [freeCount, slotIdx]
void receiveEvent(int numBytes) {
  if (numBytes >= 2) {
    uint8_t fc = Wire.read();
    Wire.read();                       // slotIdx – not used for 1-digit display
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

  pinMode(HC_DATA,  OUTPUT);
  pinMode(HC_CLK,   OUTPUT);
  pinMode(HC_LATCH, OUTPUT);

  hc_shift(0xBF);   // startup: show middle dash (g segment only, ~0x40)

  dht.begin();
  delay(2000);      // DHT11 warm-up

  Wire.begin(SLAVE_ADDR);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);

  Serial.println(F("[ENV] Environment & Display node ready at 0x08"));
}


void loop() {
  // Update 7-seg when master sends new free-count
  if (newDisplayData) {
    newDisplayData = false;
    uint8_t fc = dispFreeCount < 10 ? dispFreeCount : 9;
    hc_shift(SEG7[fc]);
    Serial.print(F("[DISP] free rooms = "));
    Serial.println(fc);
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
