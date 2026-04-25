// ============================================================
// TFT_eSPI  User_Setup.h
// Hosyond 4.0"  480×320  ILI9486 (SPI)  on  Arduino R4 WiFi
//
// Place this file in the master_node/ sketch folder.
// TFT_eSPI searches the sketch directory before its own
// library folder, so this override takes effect automatically.
//
// ── Troubleshooting ──────────────────────────────────────────
// Colours wrong (red ↔ blue swapped):
//   TFT_RGB_ORDER is already set to TFT_BGR below.
//   If colours are still wrong, swap it to TFT_RGB.
//
// Display looks like a photographic negative (inverted):
//   Uncomment:  #define TFT_INVERSION_ON
//
// Screen garbled / no output:
//   Lower SPI_FREQUENCY to 16000000.
// ============================================================

#define ILI9486_DRIVER

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
// ILI9486 SPI timing is more conservative than ST7796S.
// 20 MHz is a safe starting point; try 27000000 if you want
// faster redraws and the display is stable.
#define SPI_FREQUENCY        20000000
#define SPI_TOUCH_FREQUENCY   2500000
#define SPI_READ_FREQUENCY    8000000

// ── Colour byte order ────────────────────────────────────────
// ILI9486 sends Blue before Red (BGR), opposite to ST7796S.
// Swap to TFT_RGB if colours look wrong after this change.
#define TFT_RGB_ORDER TFT_BGR

// ── Colour inversion (uncomment if display looks inverted) ───
// #define TFT_INVERSION_ON

// ── Backlight polarity ───────────────────────────────────────
#define TFT_BACKLIGHT_ON HIGH

// ── Built-in fonts to compile in ─────────────────────────────
#define LOAD_GLCD    // Font 1 –  6×8  (small)
#define LOAD_FONT2   // Font 2 – 12×16 (medium)  ← used in UI
#define LOAD_FONT4   // Font 4 – 26×32 (large)   ← used for splash
#define LOAD_GFXFF   // AdafruitGFX free-fonts support
#define SMOOTH_FONT
