#pragma once

// Initialise audio output. Call once in setup() after mic_init().
// Enables the I2S TX channel, powers on the amplifier, and starts the sound task.
void sound_init();

// One-shot sound effects — fire-and-forget, never block.
void sound_flap();       // 880 Hz / 60 ms  — wing chirp
void sound_score();      // 1047 Hz / 80 ms — point blip
void sound_die();        // 220→110 Hz / 300 ms — descending sweep
void sound_menu_tap();   // 660 Hz / 40 ms  — UI click

// Background music control.
void sound_music_start(); // begin looping Flabby Bird melody
void sound_catch_start(); // begin looping Catch! melody
void sound_music_stop();  // stop any background melody
