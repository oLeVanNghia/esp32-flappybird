#include "touch.h"
#include "cal.h"
#include "config.h"
#include <Wire.h>
#include <Arduino.h>

// FT6336G registers
static constexpr uint8_t FT_REG_G_MODE = 0xA4;
// 0x01 = trigger mode: INT pulses once on every new finger-down event ← we use this

// ── ISR ───────────────────────────────────────────────────────────────────────
// FALLING edge only. Runs in < 1 µs; no I2C inside the ISR.
static volatile bool s_touchPending = false;
static int           s_lastX = 0, s_lastY = 0;

void IRAM_ATTR touch_isr() {
    s_touchPending = true;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void ft_write_byte(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission(true);
}

static void ft_read_coords() {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x02);                        // TD_STATUS
    Wire.endTransmission(false);
    uint8_t n = Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)5);
    if (n < 5) return;
    uint8_t td = Wire.read() & 0x0F;
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();
    if (td == 0) return;
    int raw_x = ((int)(xh & 0x0F) << 8) | xl;
    int raw_y = ((int)(yh & 0x0F) << 8) | yl;
    cal_apply(raw_x, raw_y, s_lastX, s_lastY);
}

// ── Public API ────────────────────────────────────────────────────────────────
void touch_init() {
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000UL);
    ft_write_byte(FT_REG_G_MODE, 0x01);   // trigger mode

    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, FALLING);
}

bool touch_was_pressed() {
    if (!s_touchPending) return false;
    s_touchPending = false;
    ft_read_coords();
    return true;
}

bool touch_get_pressed(int &x, int &y) {
    if (!s_touchPending) return false;
    s_touchPending = false;
    ft_read_coords();
    x = s_lastX;
    y = s_lastY;
    return true;
}

// Returns true while at least one finger is on the screen.
// Polls TD_STATUS via I2C — call at most once per frame (33 ms).
bool touch_finger_down() {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x02);
    Wire.endTransmission(false);
    if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)1) < 1) return false;
    return (Wire.read() & 0x0F) > 0;
}
