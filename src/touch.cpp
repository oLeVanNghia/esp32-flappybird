#include "touch.h"
#include "config.h"
#include <Wire.h>

// FT6336G I2C registers
static constexpr uint8_t FT_REG_TD_STATUS = 0x02;
static constexpr uint8_t FT_EVT_PRESS     = 0x00;  // event flag in high nibble

static bool     s_prevTouching = false;
static bool     s_pressed      = false;

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint8_t ft_read_byte(uint8_t reg) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

// ── Public API ────────────────────────────────────────────────────────────────
void touch_init() {
    // Deassert reset
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000UL);

    // INT pin as input (we poll instead of using interrupts)
    pinMode(PIN_TOUCH_INT, INPUT);
}

bool touch_was_pressed() {
    // Read number of touch points
    uint8_t td = ft_read_byte(FT_REG_TD_STATUS) & 0x0F;
    bool touching = (td > 0);

    // Rising edge detection
    bool event = touching && !s_prevTouching;
    s_prevTouching = touching;

    if (event) {
        // Verify the event flag is "press down" (optional sanity check)
        // Register 0x03 holds event[7:6] and X high for touch point 0.
        uint8_t evt = (ft_read_byte(0x03) >> 6) & 0x03;
        if (evt == FT_EVT_PRESS) {
            s_pressed = true;
        }
    }

    if (s_pressed) {
        s_pressed = false;
        return true;
    }
    return false;
}
