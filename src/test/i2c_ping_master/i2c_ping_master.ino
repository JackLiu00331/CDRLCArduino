/*
  I2C Ping Test – Master (Arduino 1, UNO R4 WiFi)
  只需接：A4(SDA)  A5(SCL)  GND  + 4.7kΩ上拉×2
*/
#include <Wire.h>

#define SLAVE_WING_A  0x09
#define SLAVE_WING_B  0x0A
#define SLAVE_WING_C  0x0B

unsigned long lastPingA   = 0;
unsigned long lastPingB   = 0;
unsigned long lastPingC   = 0;

unsigned long lastRound   = 0;
const unsigned long PING_GAP    = 100UL;
const unsigned long ROUND_DELAY = 2000UL;

enum PingState { PING_A, PING_B,PING_C, PING_WAIT };
PingState pingState = PING_A;

void scanI2C() {
  Serial.println(F("=== I2C Scan ==="));
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  Found 0x"));
      if (addr < 0x10) Serial.print('0');
      Serial.println(addr, HEX);
      found++;
    }
  }
  Serial.print(F("Total: ")); Serial.println(found);
  Serial.println(F("Expect: 0x09 (Wing A)  0x0A (Wing B)  0x0B (Wing C)"));
  Serial.println(F("================\n"));
}

void pingDevice(uint8_t addr, const char* name) {
  Wire.beginTransmission(addr);
  Wire.write(0xAB);
  uint8_t err = Wire.endTransmission();
  Serial.print(F("Ping ")); Serial.print(name);
  Serial.print(F(" (0x")); if (addr < 0x10) Serial.print('0');
  Serial.print(addr, HEX); Serial.print(F("): "));
  Serial.println(err == 0 ? F("OK") : F("FAIL – no ACK"));
}

void setup() {
  Serial.begin(9600);
  while (!Serial) { unsigned long t = millis(); while (millis() - t < 10) {} }
  Wire.begin();
  { unsigned long t = millis(); while (millis() - t < 500) {} }
  scanI2C();
  lastPingA = millis();
}

void loop() {
  unsigned long now = millis();

  switch (pingState) {
    case PING_A:
      pingDevice(SLAVE_WING_A, "Wing A");
      lastPingB = now;
      pingState = PING_B;
      break;

    case PING_B:
      if (now - lastPingB >= PING_GAP) {
        pingDevice(SLAVE_WING_B, "Wing B");
        lastPingC = now;
        pingState = PING_C;
      }
      break;

    case PING_C:
      if (now - lastPingC >= PING_GAP) {
        pingDevice(SLAVE_WING_C, "Wing C");
        lastRound = now;
        pingState = PING_WAIT;
      }
      break;
    
    case PING_WAIT:
      if (now - lastRound >= ROUND_DELAY) {
        pingState = PING_A;
      }
      break;
  }
}
