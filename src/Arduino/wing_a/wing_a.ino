/*
  ================================================================
  Arduino 3 – Wing A Display Node
  CDRLC Study Room Availability Monitor
  ================================================================
  负责房间：2432  2434  2436  2438  2440（共5个）

  I2C 角色：Slave，地址 0x09
  接收 1字节状态（低5位有效）：
    bit 0 = 房间 2432   (0=可预约绿灯, 1=已预约红灯)
    bit 1 = 房间 2434
    bit 2 = 房间 2436
    bit 3 = 房间 2438
    bit 4 = 房间 2440

  引脚分配（每个房间1对LED）：
    D2  房间 2432 绿
    D3  房间 2432 红
    D4  房间 2434 绿
    D5  房间 2434 红
    D6  房间 2436 绿
    D7  房间 2436 红
    D8  房间 2438 绿
    D9  房间 2438 红
    D10 房间 2440 绿
    D11 房间 2440 红
    A0  光敏电阻（分压电路，自动调光）
    A4  I2C SDA
    A5  I2C SCL

  LED 接线（双色共阴极 bi-color LED）：
    公共脚（最长脚）→ GND（通过 220Ω 限流电阻）
    绿色脚 → Arduino 绿色引脚
    红色脚 → Arduino 红色引脚

  光敏电阻接线：
    LDR 一端 → 5V
    LDR 另一端 → A0 且 → 10kΩ → GND
*/

#include <Wire.h>

#define NUM_ROOMS 5

// {绿色引脚, 红色引脚}，按 bit 0→4 顺序
// UNO R3 PWM 引脚：~3 ~5 ~6 ~9 ~10 ~11
// 绿色脚全部分配在 PWM 引脚上（空闲状态最常见，调光效果更好）
const int LED_PINS[NUM_ROOMS][2] = {
  {3,  2},   // bit0 → 房间 2432  绿=D3(PWM) 红=D2
  {5,  4},   // bit1 → 房间 2434  绿=D5(PWM) 红=D4
  {6,  7},   // bit2 → 房间 2436  绿=D6(PWM) 红=D7
  {9,  8},   // bit3 → 房间 2438  绿=D9(PWM) 红=D8
  {10, 11},  // bit4 → 房间 2440  绿=D10(PWM) 红=D11(PWM)
};
const int LDR_PIN = A0;

volatile uint8_t roomStatus = 0x00;
volatile bool    newData    = false;

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 200UL;


// ── I2C 接收 ──────────────────────────────────────────────────────────────────
void receiveEvent(int numBytes) {
  if (numBytes == 3) {
    uint8_t ctrl = Wire.read();
    uint8_t data = Wire.read();
    uint8_t chk  = Wire.read();
    if ((ctrl ^ data) == chk && ctrl == 0x01) {
      roomStatus = data;
      newData    = true;
    }
  } else {
    while (Wire.available()) Wire.read();
  }
}


// ── LED 更新 ──────────────────────────────────────────────────────────────────
void updateLEDs(uint8_t status, int brightness) {
  for (int i = 0; i < NUM_ROOMS; i++) {
    bool booked = (status >> i) & 0x01;
    // booked=true → 红灯  |  booked=false → 绿灯
    analogWrite(LED_PINS[i][1], booked  ? brightness : 0);  // 红
    analogWrite(LED_PINS[i][0], booked  ? 0 : brightness);  // 绿
  }
}


void setup() {
  Wire.begin(0x09);
  Wire.onReceive(receiveEvent);

  for (int i = 0; i < NUM_ROOMS; i++) {
    pinMode(LED_PINS[i][0], OUTPUT);   // 绿
    pinMode(LED_PINS[i][1], OUTPUT);   // 红
  }

  // 启动时全部绿灯（默认可用）
  updateLEDs(0x00, 200);
}


void loop() {
  unsigned long now = millis();
  if (now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;

  int ldrValue   = analogRead(LDR_PIN);
  int brightness = map(ldrValue, 100, 900, 40, 255);
  brightness     = constrain(brightness, 40, 255);

  if (newData) newData = false;
  updateLEDs(roomStatus, brightness);
}
