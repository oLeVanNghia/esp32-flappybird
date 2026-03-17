#pragma once
#include <Adafruit_ILI9341.h>

// Two-point touch calibration for FT6336G + ILI9341 rotation=1.
// Saves/loads calibration data in NVS ("touch_cal" namespace).

// Load calibration from NVS.  Falls back to a default axis-swap transform
// so the game is playable even before first calibration.
void cal_init();

// True if real (user-collected) calibration data is stored in NVS.
bool cal_is_valid();

// Blocking calibration procedure — draws directly on tft, no canvas needed.
// Call from setup() or the main-menu calibrate button.
void cal_run(Adafruit_ILI9341 &tft);

// Convert FT6336G raw coords → display coords using stored calibration.
// Called from touch.cpp instead of the old TOUCH_RAW_TO_X/Y macros.
void cal_apply(int raw_x, int raw_y, int &disp_x, int &disp_y);
