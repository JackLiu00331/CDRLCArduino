/*
  CDRLC Study Room Availability Monitor
  Arduino 2 - Environment and Display Node

  Team Members:
    Delon Bui        dbui9@uic.edu
    Sean Kim         skim6497@uic.edu
    Chao Liu         cliu1051@uic.edu
    Andrew Mikielski amiki@uic.edu

  Responsibilities:
    - Read DHT11 temperature and humidity every 2 seconds
    - Drive a 5161AS 1-digit 7-segment display showing the free room count (0-8)
    - Respond to I2C read requests from the master with temperature and humidity data

  I2C role: Slave at address 0x08

  Master writes to this node (2 bytes):
    byte 0 : freeCount  number of free rooms in the currently displayed slot (0-8)
    byte 1 : slotIdx    reserved

  Master reads from this node (4 bytes via Wire.requestFrom):
    byte 0 : temperature integer part  (e.g., 22 for 22.5 C)
    byte 1 : temperature fractional tenths  (e.g., 5 for 22.5 C)
    byte 2 : relative humidity integer percent  (e.g., 65)
    byte 3 : checksum = byte0 ^ byte1 ^ byte2

  Pin assignments:
    D2   DHT11 data line (10 kOhm pull-up resistor to 5V required)
    D3   7-segment pin a (top segment)
    D4   7-segment pin b (top-right)
    D5   7-segment pin c (bottom-right)
    D6   7-segment pin d (bottom)
    D7   7-segment pin e (bottom-left)
    D8   7-segment pin f (top-left)
    D9   7-segment pin g (middle)
    A4   I2C SDA
    A5   I2C SCL

  5161AS (common cathode) wiring - each segment through a 220 ohm resistor:
    Arduino D3  → 220 ohm → pin 7  (segment a - top)
    Arduino D4  → 220 ohm → pin 6  (segment b - top-right)
    Arduino D5  → 220 ohm → pin 4  (segment c - bottom-right)
    Arduino D6  → 220 ohm → pin 2  (segment d - bottom)
    Arduino D7  → 220 ohm → pin 1  (segment e - bottom-left)
    Arduino D8  → 220 ohm → pin 9  (segment f - top-left)
    Arduino D9  → 220 ohm → pin 10 (segment g - middle)
    5161AS COM  pins 3 and 8 → GND  (common cathode ground)

  5161AS pin layout (facing front, decimal point at bottom-right):
    Bottom left to right : 1(e)  2(d)  3(COM)  4(c)  5(dp)
    Top right to left    : 6(b)  7(a)  8(COM)  9(f) 10(g)

  Libraries required:
    DHT sensor library by Adafruit
*/

#include <Wire.h>
#include <DHT.h>

// Pin assignments
#define SLAVE_ADDR    0x08
#define DHT_PIN       2

// 7-segment pins in segment order: a, b, c, d, e, f, g
const int SEG_PINS[7] = { 3, 4, 5, 6, 7, 8, 9 };

#define DHT_TYPE      DHT11
#define READ_INTERVAL 2000UL

DHT dht(DHT_PIN, DHT_TYPE);

// DHT11 sensor readings (updated every READ_INTERVAL, read by master via I2C)
volatile uint8_t tInt  = 0;
volatile uint8_t tFrac = 0;
volatile uint8_t hInt  = 0;

// 7-segment display state (updated when master sends a new free count)
volatile uint8_t dispFreeCount  = 0;
volatile bool    newDisplayData = false;

unsigned long lastRead = 0;

// 5161AS common-cathode segment patterns indexed as [digit][a,b,c,d,e,f,g].
// true = segment ON; Arduino outputs HIGH to illuminate a segment on common-cathode type.
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

// 7-segment display helper functions

void showDigit(uint8_t n) {
  if (n > 9) n = 9;
  for (int i = 0; i < 7; i++)
    digitalWrite(SEG_PINS[i], SEG7[n][i] ? HIGH : LOW);  // common cathode: ON=HIGH
}

void showDash() {
  // Only middle segment (g) on — used as startup placeholder
  for (int i = 0; i < 6; i++) digitalWrite(SEG_PINS[i], LOW);
  digitalWrite(SEG_PINS[6], HIGH);  // g = on
}

// I2C event handlers

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

// Setup and main loop

void setup() {
  Serial.begin(9600);

  for (int i = 0; i < 7; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
    digitalWrite(SEG_PINS[i], LOW);    // all segments off (common cathode)
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
