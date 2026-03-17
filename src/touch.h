#pragma once
#include <Arduino.h>

// Minimal FT6336G driver.
// Only tracks whether a NEW finger-down event occurred (rising edge).
// Since the whole screen is the play area we don't need XY coordinates.

void touch_init();

// Returns true exactly once per new finger-down press; clears after reading.
bool touch_was_pressed();
