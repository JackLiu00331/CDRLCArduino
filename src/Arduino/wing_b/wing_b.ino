/*
  ================================================================
  Arduino 4 – Wing B Display Node
  CDRLC Study Room Availability Monitor
  ================================================================
  负责房间：2426  2428  2430（共3个）

  I2C 角色：Slave，地址 0x0A
  接收 4字节数据包（master_node 发送）：
    byte 0 : ctrl       = 0x01
    byte 1 : status     低3位有效
               bit 0 = 房间 2426   (0=可预约绿灯, 1=已预约红灯)
               bit 1 = 房间 2428
               bit 2 = 房间 2430
    byte 2 : brightness  LED 亮度 (40–255)，由 master 的光敏电阻决定
    byte 3 : checksum   = byte0 ^ byte1 ^ byte2

  引脚分配：
    D2  房间 2426 红
    D3  房间 2426 绿 (PWM ~3)
    D4  房间 2428 红
    D5  房间 2428 绿 (PWM ~5)
    D6  房间 2430 绿 (PWM ~6)
    D7  房间 2430 红
    A4  I2C SDA
    A5  I2C SCL

  LED 接线（双色共阴极 bi-color LED）：
    公共脚（最长脚）→ GND（通过 220Ω 限流电阻）
    绿色脚 → Arduino 绿色引脚
    红色脚 → Arduino 红色引脚

  注：HC-SR04 超声波传感器已移至 master_node（Arduino R4 WiFi），
      由 master_node 直接控制 TFT 背光亮度。
*/

#include <Wire.h>

#define NUM_ROOMS 3

// UNO R3 PWM 引脚：~3 ~5 ~6 ~9 ~10 ~11
// 绿色脚全部分配在 PWM 引脚上（空闲状态调光效果更好）
// {绿色引脚, 红色引脚}，按 bit 0→2 顺序
const int LED_PINS[NUM_ROOMS][2] = {
  {3, 2},   // bit0 → 房间 2426  绿=D3(PWM) 红=D2
  {5, 4},   // bit1 → 房间 2428  绿=D5(PWM) 红=D4
  {6, 7},   // bit2 → 房间 2430  绿=D6(PWM) 红=D7
};

volatile uint8_t roomStatus    = 0x00;
volatile uint8_t ledBrightness = 200;    // updated by master via I2C
volatile bool    newData       = false;

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 200UL;


// ── I2C 接收 ──────────────────────────────────────────────────────────────────
void receiveEvent(int numBytes) {
  if (numBytes == 4) {
    uint8_t ctrl = Wire.read();
    uint8_t data = Wire.read();
    uint8_t bri  = Wire.read();
    uint8_t chk  = Wire.read();
    if ((ctrl ^ data ^ bri) == chk && ctrl == 0x01) {
      roomStatus    = data;
      ledBrightness = bri;
      newData       = true;
    }
  } else {
    while (Wire.available()) Wire.read();
  }
}


// ── LED 更新 ──────────────────────────────────────────────────────────────────
void updateLEDs(uint8_t status, int brightness) {
  for (int i = 0; i < NUM_ROOMS; i++) {
    bool booked = (status >> i) & 0x01;
    // 红色引脚均为非PWM脚（D2 D4 D7），必须用 digitalWrite
    // 绿色引脚均为PWM脚（D3 D5 D6），用 analogWrite 实现调光
    digitalWrite(LED_PINS[i][1], booked ? HIGH : LOW);        // 红
    analogWrite (LED_PINS[i][0], booked ? 0    : brightness); // 绿
  }
}


void setup() {
  Wire.begin(0x0A);
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

  if (newData) newData = false;
  updateLEDs(roomStatus, ledBrightness);  // 亮度由 master LDR 决定
}
