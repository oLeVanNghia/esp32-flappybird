#pragma once
#include <cstdint>
#include <esp_idf_version.h>

// IDF ≥ 5.0 uses driver/i2s_std.h (new API); IDF < 5.0 uses legacy driver/i2s.h
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  define USE_NEW_I2S_API 1
#endif

// ── Pin definitions ───────────────────────────────────────────────────────────
// Source: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display

// Display (ILI9341, SPI2/FSPI)
// =============================================================================
// CS=10, DC=46, SCLK=12, MOSI=11, MISO=13, BL=45, RST=-1 (shared with EN)
// =============================================================================
#define PIN_TFT_CS    10
#define PIN_TFT_DC    46
#define PIN_TFT_MOSI  11
#define PIN_TFT_SCLK  12
#define PIN_TFT_MISO  13
#define PIN_TFT_BL    45
// RST is shared with ESP32-S3 EN — no software reset (-1)

// Touch (FT6336G, I2C)
#define PIN_TOUCH_SDA   16
#define PIN_TOUCH_SCL   15
#define PIN_TOUCH_RST   18
#define PIN_TOUCH_INT   17
#define TOUCH_I2C_ADDR  0x38

// Microphone (MEMS I2S) — source: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
#define PIN_I2S_MCLK     4
#define PIN_I2S_BCLK     5
#define PIN_I2S_WS       7
#define PIN_I2S_DATA     8   // IO8, not IO6 — microphone data in
#define PIN_I2S_DOUT     6   // IO6 — speaker data out (onboard amp)
#define PIN_I2S_AMP_EN   1   // IO1 — amplifier enable, active LOW

// ── Display ───────────────────────────────────────────────────────────────────
#define SCREEN_W   320
#define SCREEN_H   240
#define ILI9341_SPI_FREQ  27000000UL  // ILI9341 write limit ≈ 25 MHz

// ── Game layout ───────────────────────────────────────────────────────────────
#define GROUND_Y    218
#define CEILING_Y     0
#define BIRD_X       60
#define BIRD_R       12
#define BIRD_HIT_R    9

#define PIPE_W        52
#define PIPE_GAP      80
#define PIPE_COUNT     3
#define PIPE_SPACING 175

// ── Physics ───────────────────────────────────────────────────────────────────
#define GRAVITY          0.6f
#define FLAP_VEL        -5.5f
#define MAX_FALL_VEL    10.0f
#define INITIAL_SPEED    2.0f
#define SPEED_INC        0.6f
#define SPEED_STEP        20

// ── Microphone clap detection ─────────────────────────────────────────────────
// I2S MEMS mic: 24-bit left-justified in a 32-bit word.
// After the >>8 shift in mic_task the peak range is ±2²³ (≈ ±8 000 000).
//   6 000  → 0.07 % of full scale → triggers on room conversation / noise
//   100 000 → ~1 %  → triggers on sharp sounds near the board (tap, clap)
//   400 000 → ~5 %  → triggers only on loud, close claps
// Increase if the bird flaps by itself; decrease if claps are not detected.
#define MIC_THRESHOLD   400000
#define MIC_COOLDOWN_MS    300

// ── Compile-time RGB565 colour helper ─────────────────────────────────────────
#define C565(r,g,b) ((uint16_t)(((uint16_t)((r)&0xF8u)<<8) | \
                                ((uint16_t)((g)&0xFCu)<<3) | \
                                ((uint16_t)(b)>>3)))

// ── Touch coordinate transform ────────────────────────────────────────────────
// FT6336G reports in portrait orientation (raw_x 0‥240, raw_y 0‥320).
// With ILI9341 setRotation(1) the display is landscape 320×240.
// Default mapping: swap axes.  If menu tap targets feel mirrored or shifted,
// try inverting: e.g. (SCREEN_W-1-(ry)) or (SCREEN_H-1-(rx)).
#define TOUCH_RAW_TO_X(rx, ry)   (ry)
#define TOUCH_RAW_TO_Y(rx, ry)   (rx)

// ── Back-to-menu button (top-left corner, shown on every game screen) ─────────
#define BACK_BTN_X   4
#define BACK_BTN_Y   4
#define BACK_BTN_W  58
#define BACK_BTN_H  24

// ── TFT_ colour aliases (Adafruit_GFX defines BLACK/WHITE/etc without prefix) ─
#define TFT_BLACK    0x0000u
#define TFT_WHITE    0xFFFFu
#define TFT_RED      0xF800u
#define TFT_GREEN    0x07E0u
#define TFT_BLUE     0x001Fu
#define TFT_YELLOW   0xFFE0u
#define TFT_ORANGE   0xFDA0u

// ── Game colours ─────────────────────────────────────────────────────────────
#define CLR_SKY_TOP     C565(100, 180, 240)
#define CLR_SKY_BTM     C565(180, 225, 255)
#define CLR_CLOUD       TFT_WHITE
#define CLR_GND_TOP     C565(100, 200,  55)
#define CLR_GROUND      C565( 70, 162,  38)
#define CLR_DIRT        C565(200, 160, 100)
#define CLR_PIPE        C565( 85, 190,  50)
#define CLR_PIPE_CAP    C565( 55, 165,  35)
#define CLR_PIPE_LIGHT  C565(130, 220,  80)
#define CLR_PIPE_DARK   C565( 45, 138,  25)
#define CLR_SCORE_SHAD  C565( 40,  40,  40)
