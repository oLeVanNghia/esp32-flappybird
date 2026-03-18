# Sound System Design — Flabby Bird

**Date:** 2026-03-18
**Status:** Approved

## Overview

Add square-wave sound effects to Flabby Bird using the onboard I2S amplifier present on the 2.8" ESP32-S3 display board. The I2S peripheral already used for microphone input is reconfigured for full-duplex operation, adding speaker output on IO6 with no extra hardware required.

## Hardware

| Pin | Function |
|-----|----------|
| IO1 | Amplifier enable — driven LOW to enable |
| IO4 | I2S MCLK (already configured) |
| IO5 | I2S BCLK (already configured) |
| IO6 | I2S DOUT — speaker output (new) |
| IO7 | I2S WS (already configured) |
| IO8 | I2S DIN — microphone input (already configured) |

## Architecture

- A new `sound.h` / `sound.cpp` module encapsulates all audio output.
- A FreeRTOS sound task runs on **Core 0** alongside `mic_task`, writing square-wave PCM to I2S TX.
- The mic RX path is unchanged in logic; `mic_init()` is extended to also configure I2S TX (IO6) and pull IO1 LOW.
- The game loop (Core 1) calls fire-and-forget `sound_*()` functions that post to a `QueueHandle_t` (depth 4). If the queue is full, the event is silently dropped — no blocking.

## New Files

### `src/sound.h`

```cpp
void sound_init();      // call once in setup(), after mic_init()
void sound_flap();
void sound_score();
void sound_die();
void sound_menu_tap();
```

### `src/sound.cpp`

- `SoundEvent` enum: `SND_FLAP`, `SND_SCORE`, `SND_DIE`, `SND_MENU_TAP`.
- Static `QueueHandle_t s_queue` (depth 4).
- `sound_task` (Core 0, 4 KB stack): dequeues events, synthesizes square-wave PCM into a stack buffer, writes to I2S TX via `i2s_channel_write` (IDF 5.x) or `i2s_write` (IDF 4.x). Uses the same `USE_NEW_I2S_API` preprocessor guard as `mic.cpp`.
- Square-wave synthesis: for a tone of frequency `f` Hz at sample rate 16000 Hz, half-period = `16000 / (2 * f)` samples. Amplitude `±(INT32_MAX / 4)` (−6 dB headroom).

## Sound Events

| Function | Frequency | Duration | Character |
|---|---|---|---|
| `sound_flap()` | 880 Hz | 60 ms | Short chirp up |
| `sound_score()` | 1047 Hz | 80 ms | Short blip |
| `sound_die()` | 220 → 110 Hz sweep | 300 ms | Descending tone |
| `sound_menu_tap()` | 660 Hz | 40 ms | Soft click |

The `sound_die()` sweep is achieved by linearly interpolating the half-period sample count from the 220 Hz value to the 110 Hz value over the 300 ms duration.

## Changes to Existing Files

### `src/mic.cpp`

- After enabling the I2S RX channel, also configure `dout = GPIO_NUM_6` in the `gpio_cfg` struct and call `i2s_channel_enable` for TX.
- Pull IO1 LOW (`pinMode(1, OUTPUT); digitalWrite(1, LOW)`) to enable the amplifier.
- Add `PIN_I2S_AMP_EN` and `PIN_I2S_DOUT` constants to `config.h`.

### `src/config.h`

```c
#define PIN_I2S_DOUT     6
#define PIN_I2S_AMP_EN   1
```

### `src/main.cpp`

- `#include "sound.h"`
- `setup()`: call `sound_init()` after `mic_init()`.
- `bird.flap()` call sites → also call `sound_flap()`.
- Pipe passed (score increment) → also call `sound_score()`.
- Death transition (`state = STATE_DEAD`) → also call `sound_die()`.
- `handleMenuTouch()` → also call `sound_menu_tap()`.

## I2S Configuration Notes

- Sample rate: 16000 Hz (unchanged from mic config).
- Word width: 32-bit (unchanged).
- Full-duplex: `i2s_new_channel` creates a channel with both TX and RX handles; both are init'd with `i2s_channel_init_std_mode` and enabled.
- IDF 4.x: `i2s_config_t.mode` gains `I2S_MODE_TX`; `i2s_pin_config_t.data_out_num = PIN_I2S_DOUT`.

## Non-Goals

- No background music.
- No volume control.
- No WAV/MP3 playback.
