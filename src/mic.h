#pragma once

// Onboard MEMS microphone (I2S) — clap/blow detection.
// Runs a background FreeRTOS task on Core 0; the game loop polls on Core 1.

void mic_init();

// Returns true exactly once per detected clap; auto-clears after reading.
bool mic_clap_ready();
