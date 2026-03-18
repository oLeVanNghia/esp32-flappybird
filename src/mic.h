#pragma once
#include "config.h"

// Initialise the onboard MEMS microphone (I2S RX) and allocate the speaker
// TX channel on the same I2S peripheral. TX is NOT enabled here — call
// sound_init() afterwards to enable it.
void mic_init();

// Returns true exactly once per detected clap; auto-clears after reading.
bool mic_clap_ready();

#ifdef USE_NEW_I2S_API
#include <driver/i2s_std.h>
// Returns the TX channel handle created in mic_init(); nullptr if init failed.
i2s_chan_handle_t mic_get_tx_chan();
// Enables the TX channel (called by sound_init() when ready to write).
void mic_enable_tx_chan();
#endif
