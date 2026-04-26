/*
  CDRLC Study Room Availability Monitor
  Arduino 4 - Wing B LED Display Node

  Team Members:
    Delon Bui        dbui9@uic.edu
    Sean Kim         skim6497@uic.edu
    Chao Liu         cliu1051@uic.edu
    Andrew Mikielski amiki@uic.edu

  Rooms managed: 2426, 2428, 2430 (3 rooms)

  I2C role: Slave at address 0x0A
  Receives a 4-byte packet from the master node on each update:
    byte 0 : ctrl       = 0x01 (packet type identifier)
    byte 1 : status     bits 0-2, one per room (0 = free/green, 1 = booked/red)
                          bit 0 = room 2426
                          bit 1 = room 2428
                          bit 2 = room 2430
    byte 2 : brightness LED brightness (0 = screen off / LEDs sleep, 40-255 = active)
                          Determined by the LDR photoresistor on the master node (A2).
    byte 3 : checksum   = byte0 ^ byte1 ^ byte2

  Pin assignments:
    D2   Room 2426 red  LED (non-PWM, uses digitalWrite)
    D3   Room 2426 green LED (PWM, uses analogWrite for dimming)
    D4   Room 2428 red  LED (non-PWM, uses digitalWrite)
    D5   Room 2428 green LED (PWM)
    D6   Room 2430 green LED (PWM)
    D7   Room 2430 red  LED (non-PWM)
    A4   I2C SDA
    A5   I2C SCL

  LED wiring (bi-color common-cathode):
    Common pin (longest leg) → 220 ohm resistor → GND
    Green pin → Arduino green pin (PWM pin, supports analogWrite for brightness)
    Red pin   → Arduino red pin  (non-PWM pin, uses digitalWrite HIGH/LOW only)

  Note: The HC-SR04 ultrasonic sensor and LDR photoresistor are both located on
        the master node (Arduino R4 WiFi). The master controls TFT backlight
        brightness and broadcasts an ambient-light-adjusted brightness value in
        every I2C packet. When brightness = 0 (screen off / sleep mode), all LEDs
        are extinguished, including red booked-room indicators.
*/

#include <Wire.h>

#define NUM_ROOMS 3

// LED pin pairs as {green, red}, ordered by status bit 0 through 2.
// Green pins are all PWM-capable (UNO R3: ~3 ~5 ~6) for brightness dimming.
// Red pins are non-PWM and can only be toggled with digitalWrite.
const int LED_PINS[NUM_ROOMS][2] = {
  {3, 2},   // bit0 → room 2426  green=D3(PWM) red=D2
  {5, 4},   // bit1 → room 2428  green=D5(PWM) red=D4
  {6, 7},   // bit2 → room 2430  green=D6(PWM) red=D7
};

volatile uint8_t roomStatus    = 0x00;
volatile uint8_t ledBrightness = 200;    // updated by master via I2C
volatile bool    newData       = false;

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 200UL;


// I2C receive event handler
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


// LED update function - applies room status and brightness to all LED pairs
void updateLEDs(uint8_t status, int brightness) {
  for (int i = 0; i < NUM_ROOMS; i++) {
    bool booked = (status >> i) & 0x01;
    if (brightness == 0) {
      // Screen is off (sleep mode) → extinguish ALL LEDs including red
      digitalWrite(LED_PINS[i][1], LOW);
      analogWrite (LED_PINS[i][0], 0);
    } else {
      // Red pins (D2 D4 D7) are non-PWM; use digitalWrite HIGH/LOW only.
      // Green pins (D3 D5 D6) are PWM; use analogWrite for brightness dimming.
      digitalWrite(LED_PINS[i][1], booked ? HIGH : LOW);        // red
      analogWrite (LED_PINS[i][0], booked ? 0    : brightness); // green
    }
  }
}


void setup() {
  Wire.begin(0x0A);
  Wire.onReceive(receiveEvent);

  for (int i = 0; i < NUM_ROOMS; i++) {
    pinMode(LED_PINS[i][0], OUTPUT);   // green
    pinMode(LED_PINS[i][1], OUTPUT);   // red
  }

  // All green at startup (default: all rooms assumed available)
  updateLEDs(0x00, 200);
}


void loop() {
  unsigned long now = millis();
  if (now - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = now;

  if (newData) newData = false;
  updateLEDs(roomStatus, ledBrightness);  // brightness is set by master LDR and sent via I2C
}
