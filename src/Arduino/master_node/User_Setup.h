// ============================================================
// TFT_eSPI  User_Setup.h
// Hosyond 4.0"  480×320  ST7796S (SPI)  on  Arduino R4 WiFi
//
// Place this file in the master_node/ sketch folder.
// TFT_eSPI searches the sketch directory before its own
// library folder, so this override takes effect automatically.
//
// If colours look wrong (red ↔ blue swapped) change
//   TFT_RGB_ORDER  from  TFT_RGB  to  TFT_BGR
// If display is garbled, lower SPI_FREQUENCY to 20000000.
// ============================================================

#define ST7796_DRIVER

// Internal panel dimensions in portrait orientation.
// setRotation(1) in code gives us the 480×320 landscape view.
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ── SPI pin assignments (Arduino R4 WiFi) ────────────────────
//   A0 = digital pin 14 on R4,  A1 = digital pin 15
#define TFT_CS    10    // D10  – TFT chip-select
#define TFT_DC    14    // A0   – Data / Command
#define TFT_RST    9    // D9   – Hardware reset
#define TFT_MOSI  11    // D11  – MOSI  (shared SPI bus)
#define TFT_SCLK  13    // D13  – SCK   (shared SPI bus)
#define TFT_MISO  12    // D12  – MISO  (shared SPI bus)

#define TOUCH_CS  15    // A1   – XPT2046 touch chip-select

// ── SPI clock frequencies ────────────────────────────────────
#define SPI_FREQUENCY        27000000
#define SPI_TOUCH_FREQUENCY   2500000
#define SPI_READ_FREQUENCY    8000000

// ── Colour byte order ────────────────────────────────────────
#define TFT_RGB_ORDER TFT_RGB   // swap to TFT_BGR if colours look wrong

// ── Backlight polarity ───────────────────────────────────────
#define TFT_BACKLIGHT_ON HIGH

// ── Built-in fonts to compile in ─────────────────────────────
#define LOAD_GLCD    // Font 1 –  6×8  (small)
#define LOAD_FONT2   // Font 2 – 12×16 (medium)  ← used in UI
#define LOAD_FONT4   // Font 4 – 26×32 (large)   ← used for splash
#define LOAD_GFXFF   // AdafruitGFX free-fonts support
#define SMOOTH_FONT
