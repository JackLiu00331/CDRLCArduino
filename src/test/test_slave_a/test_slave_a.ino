/*
  I2C 通讯测试 – Slave A (Arduino 3, UNO R3, 地址 0x09)
*/
#include <Wire.h>

#define NUM_ROOMS  5
#define SLAVE_ADDR 0x09

const int LED_PINS[NUM_ROOMS][2] = {
  {3,  2}, {5,  4}, {6,  7}, {9,  8}, {10, 11},
};

volatile uint8_t rxCtrl  = 0;
volatile uint8_t rxData  = 0;
volatile uint8_t rxChk   = 0;
volatile uint8_t rxBytes = 0;
volatile bool    rxReady = false;
volatile bool    rxError = false;

// 错误闪烁状态（非阻塞）
bool          errBlink      = false;
unsigned long errBlinkStart = 0;
const unsigned long ERR_BLINK_MS = 300UL;

void receiveEvent(int numBytes) {
  if (numBytes == 3) {
    rxCtrl  = Wire.read(); rxData  = Wire.read(); rxChk   = Wire.read();
    rxBytes = 3; rxReady = true; rxError = false;
  } else {
    rxBytes = numBytes;
    while (Wire.available()) Wire.read();
    rxReady = true; rxError = true;
  }
}

void showLEDs(uint8_t status) {
  for (int i = 0; i < NUM_ROOMS; i++) {
    bool booked = (status >> i) & 0x01;
    digitalWrite(LED_PINS[i][1], booked ? HIGH : LOW);
    digitalWrite(LED_PINS[i][0], booked ? LOW  : HIGH);
  }
}

void setup() {
  Serial.begin(9600);
  for (int i = 0; i < NUM_ROOMS; i++) {
    pinMode(LED_PINS[i][0], OUTPUT);
    pinMode(LED_PINS[i][1], OUTPUT);
  }

  // 启动闪烁（setup 中 millis 忙等）
  showLEDs(0x00);
  { unsigned long t = millis(); while (millis() - t < 400) {} }
  showLEDs(0x1F);
  { unsigned long t = millis(); while (millis() - t < 400) {} }
  showLEDs(0x00);

  Wire.begin(SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Serial.println(F("[I2C] Slave A ready at 0x09"));
}

void loop() {
  unsigned long now = millis();

  // 错误闪烁结束后复位 LED
  if (errBlink && now - errBlinkStart >= ERR_BLINK_MS) {
    errBlink = false;
    showLEDs(0x00);
  }

  if (!rxReady) return;
  rxReady = false;

  if (rxError) {
    Serial.print(F("[RX] ERROR: expected 3 bytes, got ")); Serial.println(rxBytes);
    showLEDs(0x1F);
    errBlink = true; errBlinkStart = now;
    return;
  }

  uint8_t expectedChk = rxCtrl ^ rxData;
  bool    chkOK       = (rxChk == expectedChk) && (rxCtrl == 0x01);

  Serial.print(F("[RX] ctrl=0x")); if (rxCtrl < 0x10) Serial.print('0'); Serial.print(rxCtrl, HEX);
  Serial.print(F("  data=0x"));   if (rxData < 0x10) Serial.print('0'); Serial.print(rxData, HEX);
  Serial.print(F("  chk=0x"));    if (rxChk  < 0x10) Serial.print('0'); Serial.print(rxChk,  HEX);

  if (chkOK) {
    Serial.print(F("  OK  bits="));
    for (int i = NUM_ROOMS - 1; i >= 0; i--) Serial.print((rxData >> i) & 0x01);
    Serial.println();
    showLEDs(rxData);
  } else {
    Serial.print(F("  CHECKSUM FAIL (expected 0x"));
    if (expectedChk < 0x10) Serial.print('0');
    Serial.print(expectedChk, HEX); Serial.println(F(")"));
    showLEDs(0x1F);
    errBlink = true; errBlinkStart = now;
  }
}
