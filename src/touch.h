#pragma once
#include <Arduino.h>

// Minimal FT6336G driver — ISR-based, trigger mode.

void touch_init();

// Returns true exactly once per new finger-down press; clears after reading.
bool touch_was_pressed();

// Same as touch_was_pressed() but also returns display-space coordinates.
bool touch_get_pressed(int &x, int &y);

// Returns true while at least one finger is on the screen (I2C TD_STATUS poll).
// Call at most once per frame to avoid saturating the I2C bus.
bool touch_finger_down();
