/*
  ================================================================
  Arduino 4 – Wing B Display Node
  CDRLC Study Room Availability Monitor
  ================================================================
  负责房间：2426  2428  2430（共3个）
  附加功能：HC-SR04 超声波传感器检测人员靠近，息屏/亮屏

  I2C 角色：Slave，地址 0x0A
  接收 1字节状态（低3位有效）：
    bit 0 = 房间 2426   (0=可预约绿灯, 1=已预约红灯)
    bit 1 = 房间 2428
    bit 2 = 房间 2430

  引脚分配：
    D2  房间 2426 绿
    D3  房间 2426 红
    D4  房间 2428 绿
    D5  房间 2428 红
    D6  房间 2430 绿
    D7  房间 2430 红
    D9  HC-SR04 Trig（触发）
    D10 HC-SR04 Echo（回响）
    A4  I2C SDA
    A5  I2C SCL

  HC-SR04 接线：
    VCC  → 5V
    GND  → GND
    Trig → D9
    Echo → D10

  省电逻辑：
    距离 > 150cm（无人靠近）→ LED 亮度降为 10%
    距离 ≤ 150cm（有人靠近）→ LED 全亮（255）
*/

#include <Wire.h>

#define NUM_ROOMS 3

// UNO R3 PWM 引脚：~3 ~5 ~6 ~9 ~10 ~11
// 绿色脚全部分配在 PWM 引脚上（空闲状态调光效果更好）
const int LED_PINS[NUM_ROOMS][2] = {
  {3, 2},   // bit0 → 房间 2426  绿=D3(PWM) 红=D2
  {5, 4},   // bit1 → 房间 2428  绿=D5(PWM) 红=D4
  {6, 7},   // bit2 → 房间 2430  绿=D6(PWM) 红=D7
};
#define TRIG_PIN 9
#define ECHO_PIN 10
#define PRESENCE_THRESHOLD_CM 150

volatile uint8_t roomStatus = 0x00;
volatile bool    newData    = false;

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 300UL;


// ── 超声波测距 ────────────────────────────────────────────────────────────────
long readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 超时30ms
  if (duration == 0) return 999;  // 超出量程视为无人
  return duration / 58L;
}


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
    analogWrite(LED_PINS[i][1], booked ? brightness : 0);  // 红
    analogWrite(LED_PINS[i][0], booked ? 0 : brightness);  // 绿
  }
}


void setup() {
  Wire.begin(0x0A);
  Wire.onReceive(receiveEvent);

  for (int i = 0; i < NUM_ROOMS; i++) {
    pinMode(LED_PINS[i][0], OUTPUT);
    pinMode(LED_PINS[i][1], OUTPUT);
  }
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  updateLEDs(0x00, 200);
}


void loop() {
  unsigned long now = millis();
  if (now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;

  long dist      = readDistanceCm();
  int  brightness = (dist <= PRESENCE_THRESHOLD_CM) ? 255 : 25;
  newData        = false;
  updateLEDs(roomStatus, brightness);
}
