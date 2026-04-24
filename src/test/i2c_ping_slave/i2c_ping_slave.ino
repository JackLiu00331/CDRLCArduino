/*
  I2C Ping Test – Slave (Arduino 3 或 Arduino 4)
  只需接：A4(SDA)  A5(SCL)  GND

  ⚠ 烧录前改这一行：
      Arduino 3 (Wing A) → #define MY_ADDR 0x09
      Arduino 4 (Wing B) → #define MY_ADDR 0x0A
*/
#include <Wire.h>

#define MY_ADDR 0x0B    // ← Arduino 3 用 0x09，Arduino 4 改成 0x0A

volatile bool    received = false;
volatile uint8_t rxByte   = 0;

void receiveEvent(int n) {
  if (Wire.available()) rxByte = Wire.read();
  while (Wire.available()) Wire.read();
  received = true;
}

void setup() {
  Serial.begin(9600);

  // 启动提示（setup 中 millis 忙等，不用 delay）
  Serial.println(F("Starting..."));
  { unsigned long t = millis(); while (millis() - t < 500) {} }

  Wire.begin(MY_ADDR);
  Wire.onReceive(receiveEvent);

  Serial.print(F("Slave ready at 0x"));
  Serial.println(MY_ADDR, HEX);
}

void loop() {
  if (!received) return;
  received = false;
  Serial.print(F("RX: 0x"));
  Serial.println(rxByte, HEX);
}
