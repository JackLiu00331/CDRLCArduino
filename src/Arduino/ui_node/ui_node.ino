/*
  ================================================================
  Arduino 2 – Environment Sensor Node  v2.0
  CDRLC Study Room Availability Monitor
  ================================================================
  Responsibilities:
    - Read DHT11 temperature & humidity every 2 seconds
    - I2C Slave at address 0x08
    - Respond to Master's requestFrom(0x08, 4) with:
        byte 0 : temperature integer part  (°C, e.g. 22)
        byte 1 : temperature fractional    (tenths, e.g. 5 → 22.5 °C)
        byte 2 : relative humidity integer (%, e.g. 65)
        byte 3 : checksum = byte0 ^ byte1 ^ byte2

  Wiring:
    D2   DHT11 data line (10 kΩ pull-up to 5 V)
    A4   I2C SDA  (shared bus with Arduinos 1, 3, 4)
    A5   I2C SCL

  Library required:
    DHT sensor library  by Adafruit
    (Adafruit Unified Sensor installs automatically as a dependency)
  ================================================================
*/

#include <Wire.h>
#include <DHT.h>

#define SLAVE_ADDR     0x08
#define DHT_PIN        2
#define DHT_TYPE       DHT11
#define READ_INTERVAL  2000UL   // minimum safe read interval for DHT11

DHT dht(DHT_PIN, DHT_TYPE);

// These are written in loop() and read in the I2C ISR.
// Declared volatile to prevent compiler optimisation issues.
volatile uint8_t tInt  = 0;   // temperature integer part
volatile uint8_t tFrac = 0;   // temperature fractional (tenths)
volatile uint8_t hInt  = 0;   // humidity integer part

unsigned long lastRead = 0;


// ── I2C request handler (called inside ISR context) ───────────────────────────
void requestEvent() {
  uint8_t chk = tInt ^ tFrac ^ hInt;
  Wire.write(tInt);
  Wire.write(tFrac);
  Wire.write(hInt);
  Wire.write(chk);
}


void setup() {
  Serial.begin(9600);
  dht.begin();
  delay(2000);   // DHT11 needs ≥1 s after power-on; 2 s is safer
  Wire.begin(SLAVE_ADDR);
  Wire.onRequest(requestEvent);
  Serial.println(F("[ENV] DHT11 node ready at 0x08"));
}


void loop() {
  unsigned long now = millis();
  if (now - lastRead < READ_INTERVAL) return;
  lastRead = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) {
    uint8_t ti = (uint8_t)t;
    uint8_t tf = (uint8_t)((t - (float)ti) * 10.0f + 0.5f);
    if (tf > 9) { tf = 0; ti++; }   // handle rounding overflow
    tInt  = ti;
    tFrac = tf;
  }
  if (!isnan(h)) {
    hInt = (uint8_t)(h + 0.5f);     // round to nearest integer %
  }

  if (isnan(t) || isnan(h)) {
    Serial.println(F("[DHT] read failed – check wiring & pull-up resistor"));
  } else {
    Serial.print(F("[DHT] "));
    Serial.print(t, 1); Serial.print(F("C  "));
    Serial.print(h, 0); Serial.println('%');
  }
}
