#include "cal.h"
#include "config.h"
#include <Wire.h>
#include <Arduino.h>
#include <Preferences.h>

// ── Calibration data ──────────────────────────────────────────────────────────

struct CalData {
    bool  swap;   // true  → disp_x uses raw_y, disp_y uses raw_x
                  // false → disp_x uses raw_x, disp_y uses raw_y
    float sx, ox; // disp_x = sx * raw_a + ox
    float sy, oy; // disp_y = sy * raw_b + oy
};

static CalData g_cal   = { true, 1.0f, 0.0f, 1.0f, 0.0f };
static bool    g_valid = false;

// Default: swap axes (raw_y→disp_x, raw_x→disp_y) — the most common mapping
// for ILI9341 rotation=1.  Works well enough to navigate the calibration menu.
static void apply_defaults() {
    g_cal  = { true, 1.333f, 0.0f, 1.0f, 0.0f };
    g_valid = false;
}

void cal_apply(int raw_x, int raw_y, int &disp_x, int &disp_y) {
    float a = (float)(g_cal.swap ? raw_y : raw_x);
    float b = (float)(g_cal.swap ? raw_x : raw_y);
    disp_x = (int)(g_cal.sx * a + g_cal.ox);
    disp_y = (int)(g_cal.sy * b + g_cal.oy);
    // Clamp to screen bounds
    if (disp_x < 0)          disp_x = 0;
    if (disp_x >= SCREEN_W)  disp_x = SCREEN_W - 1;
    if (disp_y < 0)          disp_y = 0;
    if (disp_y >= SCREEN_H)  disp_y = SCREEN_H - 1;
}

bool cal_is_valid() { return g_valid; }

void cal_init() {
    Preferences prefs;
    prefs.begin("touch_cal", true);
    g_valid = prefs.getBool("valid", false);
    if (g_valid) {
        g_cal.swap = prefs.getBool("swap",  true);
        g_cal.sx   = prefs.getFloat("sx",   1.333f);
        g_cal.ox   = prefs.getFloat("ox",   0.0f);
        g_cal.sy   = prefs.getFloat("sy",   1.0f);
        g_cal.oy   = prefs.getFloat("oy",   0.0f);
        Serial.printf("[cal] loaded: swap=%d sx=%.4f ox=%.1f sy=%.4f oy=%.1f\n",
                      g_cal.swap, g_cal.sx, g_cal.ox, g_cal.sy, g_cal.oy);
    } else {
        apply_defaults();
        Serial.println("[cal] no data — using defaults, run calibration from menu");
    }
    prefs.end();
}

// ── Low-level helpers (bypass touch.cpp — no calibration applied) ─────────────

static void draw_target(Adafruit_ILI9341 &tft, int x, int y) {
    tft.drawFastHLine(x - 18, y,      37, 0xF800);
    tft.drawFastVLine(x,      y - 18, 37, 0xF800);
    tft.drawCircle(x, y, 8,  0xF800);
    tft.drawCircle(x, y, 3,  0xFFFF);
}

// Blocks until one finger tap is detected; returns raw FT6336G coordinates.
// Returns false on timeout (10 s).
static bool cal_read_tap(int &raw_x, int &raw_y) {
    const uint32_t TIMEOUT = 10000;
    uint32_t start = millis();

    // Wait for finger-down
    while (millis() - start < TIMEOUT) {
        Wire.beginTransmission(TOUCH_I2C_ADDR);
        Wire.write(0x02);                      // TD_STATUS
        Wire.endTransmission(false);
        uint8_t n = Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)5);
        if (n >= 5) {
            uint8_t td = Wire.read() & 0x0F;
            uint8_t xh = Wire.read();
            uint8_t xl = Wire.read();
            uint8_t yh = Wire.read();
            uint8_t yl = Wire.read();
            if (td > 0) {
                raw_x = ((int)(xh & 0x0F) << 8) | xl;
                raw_y = ((int)(yh & 0x0F) << 8) | yl;
                Serial.printf("[cal] tap raw=(%d,%d)\n", raw_x, raw_y);
                // Wait for finger to lift
                delay(60);
                for (;;) {
                    Wire.beginTransmission(TOUCH_I2C_ADDR);
                    Wire.write(0x02);
                    Wire.endTransmission(false);
                    if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)1) >= 1)
                        if ((Wire.read() & 0x0F) == 0) break;
                    delay(20);
                }
                delay(250);  // debounce
                return true;
            }
        }
        delay(40);
    }
    return false;  // timeout
}

static void cal_screen(Adafruit_ILI9341 &tft, int step) {
    tft.fillScreen(0x0000);
    tft.setTextColor(0xFFFF);
    tft.setTextSize(2);
    tft.setCursor(50, 90);
    tft.print("CALIBRATION ");
    tft.print(step);
    tft.println("/2");
    tft.setTextSize(1);
    tft.setCursor(55, 120);
    tft.println("Tap the red cross");
}

// ── Public calibration procedure ─────────────────────────────────────────────

void cal_run(Adafruit_ILI9341 &tft) {
    // Calibration targets in display coordinates
    const int P1X = 40,            P1Y = 40;
    const int P2X = SCREEN_W - 40, P2Y = SCREEN_H - 40;

    int r1x, r1y, r2x, r2y;

    // ── Point 1: top-left ────────────────────────────────────────────────────
    cal_screen(tft, 1);
    draw_target(tft, P1X, P1Y);
    if (!cal_read_tap(r1x, r1y)) { apply_defaults(); return; }

    // ── Point 2: bottom-right ────────────────────────────────────────────────
    cal_screen(tft, 2);
    draw_target(tft, P2X, P2Y);
    if (!cal_read_tap(r2x, r2y)) { apply_defaults(); return; }

    // ── Compute affine transform ──────────────────────────────────────────────
    // Determine axis mapping: whichever raw axis has the larger delta between
    // the two diagonally-opposite targets corresponds to the display X axis.
    int dr_x = r2x - r1x;
    int dr_y = r2y - r1y;
    int dd_x = P2X - P1X;   // 240
    int dd_y = P2Y - P1Y;   // 160

    bool swap = (abs(dr_y) > abs(dr_x));

    float sx, ox, sy, oy;
    if (swap) {
        // disp_x ← raw_y,  disp_y ← raw_x
        sx = (dr_y != 0) ? (float)dd_x / dr_y : 1.0f;
        ox = P1X - sx * r1y;
        sy = (dr_x != 0) ? (float)dd_y / dr_x : 1.0f;
        oy = P1Y - sy * r1x;
    } else {
        // disp_x ← raw_x,  disp_y ← raw_y
        sx = (dr_x != 0) ? (float)dd_x / dr_x : 1.0f;
        ox = P1X - sx * r1x;
        sy = (dr_y != 0) ? (float)dd_y / dr_y : 1.0f;
        oy = P1Y - sy * r1y;
    }

    g_cal   = { swap, sx, ox, sy, oy };
    g_valid = true;

    Serial.printf("[cal] result: swap=%d sx=%.4f ox=%.1f sy=%.4f oy=%.1f\n",
                  swap, sx, ox, sy, oy);

    // ── Save to NVS ───────────────────────────────────────────────────────────
    Preferences prefs;
    prefs.begin("touch_cal", false);
    prefs.putBool ("valid", true);
    prefs.putBool ("swap",  swap);
    prefs.putFloat("sx",    sx);
    prefs.putFloat("ox",    ox);
    prefs.putFloat("sy",    sy);
    prefs.putFloat("oy",    oy);
    prefs.end();

    // ── Confirmation screen ───────────────────────────────────────────────────
    tft.fillScreen(0x0000);
    tft.setTextColor(0x07E0);   // green
    tft.setTextSize(2);
    tft.setCursor(70, 100);
    tft.println("Calibration");
    tft.setCursor(90, 128);
    tft.println("Complete!");
    delay(1500);
}
