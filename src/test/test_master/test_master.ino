/*
  I2C 通讯测试 – Master (Arduino 1, UNO R4 WiFi)
*/
#include <Wire.h>

#define SLAVE_WING_A  0x09
#define SLAVE_WING_B  0x0A

const uint8_t TEST_SEQ_A[] = {0x00, 0x1F, 0x0F, 0x15, 0x0A, 0x01};
const uint8_t TEST_SEQ_B[] = {0x00, 0x07, 0x05, 0x02, 0x03, 0x04};
const int SEQ_LEN = 6;

int           seqIdx    = 0;
unsigned long lastRound = 0;
const unsigned long ROUND_INTERVAL = 2000UL;


void scanI2C() {
  Serial.println(F("\n========== I2C BUS SCAN =========="));
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("[SCAN] Found device at 0x"));
      if (addr < 0x10) Serial.print('0');
      Serial.print(addr, HEX);
      if (addr == SLAVE_WING_A) Serial.print(F("  <- Wing A"));
      if (addr == SLAVE_WING_B) Serial.print(F("  <- Wing B"));
      if (addr == 0x27 || addr == 0x3F) Serial.print(F("  <- LCD backpack"));
      Serial.println();
      found++;
    }
  }
  Serial.print(F("[SCAN] Done. Found ")); Serial.print(found); Serial.println(F(" device(s)."));
  Serial.println(F("Expect: 0x09 (Wing A)  0x0A (Wing B)"));
  Serial.println(F("==================================\n"));
}

bool sendTestPacket(uint8_t addr, uint8_t data) {
  uint8_t ctrl = 0x01;
  uint8_t chk  = ctrl ^ data;
  Wire.beginTransmission(addr);
  Wire.write(ctrl); Wire.write(data); Wire.write(chk);
  uint8_t err = Wire.endTransmission();

  Serial.print(F("[TX] -> 0x"));
  if (addr < 0x10) Serial.print('0');
  Serial.print(addr, HEX);
  Serial.print(F("  data=0x"));
  if (data < 0x10) Serial.print('0');
  Serial.print(data, HEX);
  Serial.println(err == 0 ? F("  OK") : F("  FAIL"));
  return err == 0;
}

void setup() {
  Serial.begin(9600);
  while (!Serial) { unsigned long t = millis(); while (millis() - t < 10) {} }
  Wire.begin();
  { unsigned long t = millis(); while (millis() - t < 500) {} }
  scanI2C();
  lastRound = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastRound < ROUND_INTERVAL) return;
  lastRound = now;

  Serial.print(F("\n--- Round ")); Serial.print(seqIdx + 1);
  Serial.print(F(" ---  dataA=0b")); Serial.print(TEST_SEQ_A[seqIdx], BIN);
  Serial.print(F("  dataB=0b")); Serial.println(TEST_SEQ_B[seqIdx], BIN);

  sendTestPacket(SLAVE_WING_A, TEST_SEQ_A[seqIdx]);
  sendTestPacket(SLAVE_WING_B, TEST_SEQ_B[seqIdx]);

  seqIdx = (seqIdx + 1) % SEQ_LEN;
}
