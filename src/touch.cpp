#include "touch.h"
#include "config.h"
#include <Wire.h>
#include <Arduino.h>

// FT6336G registers
static constexpr uint8_t FT_REG_G_MODE = 0xA4;
//   0x00 = polling  : INT stays LOW while finger is present
//   0x01 = trigger  : INT pulses once on every new finger-down event  ← we use this

// ── ISR ───────────────────────────────────────────────────────────────────────
// Runs in < 1 µs; just sets a flag.  No I2C inside the ISR.
static volatile bool s_touchPending = false;

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

// ── Public API ────────────────────────────────────────────────────────────────
void touch_init() {
    // Hardware reset
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000UL);

    // Trigger mode: INT fires once per new finger-down event.
    // The ISR captures it immediately regardless of the game-loop frame rate.
    ft_write_byte(FT_REG_G_MODE, 0x01);

    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, FALLING);
}

bool touch_was_pressed() {
    if (!s_touchPending) return false;
    s_touchPending = false;   // consume — next call returns false until next tap
    return true;
}
