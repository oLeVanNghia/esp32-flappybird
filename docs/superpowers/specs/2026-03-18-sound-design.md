# Sound System Design — Flabby Bird

**Date:** 2026-03-18
**Status:** Approved (v4 — adds background music)

## Overview

Add square-wave sound effects and looping chiptune background music to Flabby Bird using the onboard I2S amplifier on the 2.8" ESP32-S3 display board. The I2S peripheral already used for microphone input is reconfigured for full-duplex operation, adding speaker output on IO6 with no extra hardware required.

## Hardware

The lcdwiki.com board schematic confirms the following pin assignments:

| Pin | Function | Status |
|-----|----------|--------|
| IO1 | Amplifier enable — active LOW (confirmed; lcdwiki: "audio output enable signal") | New |
| IO4 | I2S MCLK | Existing |
| IO5 | I2S BCLK | Existing |
| IO6 | I2S DOUT — speaker output (distinct from IO8 mic input) | New |
| IO7 | I2S WS | Existing |
| IO8 | I2S DIN — microphone input | Existing |

`config.h` currently has `#define PIN_I2S_DATA 8 // IO8, not IO6` — the comment clarifies the *mic* data pin is IO8, not IO6. IO6 is the separate speaker DOUT; no conflict.

## Architecture

- A new `sound.h` / `sound.cpp` module encapsulates all audio output.
- A single FreeRTOS sound task runs on **Core 0** alongside `mic_task`, handling both music and effects.
- `mic_init()` allocates a **full-duplex** I2S channel via a single `i2s_new_channel()` call that populates **both** `s_tx_chan` and `s_rx_chan` from `I2S_NUM_0`. This is the only `i2s_new_channel()` call in the codebase — `sound_init()` does **not** call it. There is no peripheral conflict.
- `mic_init()` does **not** enable the TX channel — deferred to `sound_init()`.
- `mic.h` exposes `mic_enable_tx_chan()` and `mic_get_tx_chan()` so `sound_init()` can enable and obtain the TX handle.
- `sound_init()` enables the TX channel, enables the amplifier (IO1 LOW), and starts the sound task.
- The game loop (Core 1) calls fire-and-forget `sound_*()` functions that post `SoundEvent` values to a `QueueHandle_t` (depth 8). Events dropped silently if full — no blocking.

**Music/effect interleaving (interruption mode):**

- **Music on:** task writes one 8 ms chunk of the current melody note, then polls the queue with timeout=0. If an effect event arrives, the task plays the effect to completion, then resumes the melody from the saved note position. **Known limitation:** resuming mid-note after an effect causes a minor phase discontinuity (click) due to DMA state not being preserved during the effect write. This is acceptable for a game context; seamless resume is not a requirement.
- **Music off / no pending effect:** task blocks on queue with `portMAX_DELAY` (no wasted CPU).
- **Priority is design intent, not mechanically enforced** — rapid flap events can fill the queue before a die event; this is an acceptable trade-off.

## `config.h` Changes

Move `USE_NEW_I2S_API` definition here so it is shared across `mic.cpp`, `mic.h`, and `sound.cpp`. Add `#include <esp_idf_version.h>` before the guard:

```c
#include <esp_idf_version.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  define USE_NEW_I2S_API 1
#endif

#define PIN_I2S_DOUT    6
#define PIN_I2S_AMP_EN  1
```

Remove the `#if ESP_IDF_VERSION ...` / `#define USE_NEW_I2S_API` block from `mic.cpp`.

## `src/mic.h` Changes

```cpp
#pragma once
#include "config.h"

void mic_init();
bool mic_clap_ready();

#ifdef USE_NEW_I2S_API
#include <driver/i2s_std.h>
// Returns the TX channel handle allocated in mic_init() (not yet enabled).
i2s_chan_handle_t mic_get_tx_chan();
// Enables the TX channel. Called by sound_init() when ready to write.
void mic_enable_tx_chan();
#endif
```

## `src/mic.cpp` Changes

**File-scope variables (alongside existing `s_rx_chan`):**
```cpp
#ifdef USE_NEW_I2S_API
static i2s_chan_handle_t s_rx_chan = nullptr;
static i2s_chan_handle_t s_tx_chan = nullptr;  // file scope — not inside mic_init()
#endif
```

**Remove the `USE_NEW_I2S_API` definition block** — it now comes from `config.h`.

**IDF 5.x — inside `mic_init()`:**
```cpp
// Change nullptr → &s_tx_chan to allocate full-duplex pair
i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);

// ... existing RX init and i2s_channel_enable(s_rx_chan) unchanged ...

// Init TX channel with its own config (DOUT=IO6, DIN unused)
i2s_std_config_t tx_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .mclk = (gpio_num_t)PIN_I2S_MCLK,
        .bclk = (gpio_num_t)PIN_I2S_BCLK,
        .ws   = (gpio_num_t)PIN_I2S_WS,
        .dout = (gpio_num_t)PIN_I2S_DOUT,
        .din  = I2S_GPIO_UNUSED,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
};
i2s_channel_init_std_mode(s_tx_chan, &tx_cfg);
// Do NOT enable TX here — deferred to sound_init() to avoid DMA noise.
```

**New accessor functions (IDF 5.x):**
```cpp
#ifdef USE_NEW_I2S_API
i2s_chan_handle_t mic_get_tx_chan() { return s_tx_chan; }
void mic_enable_tx_chan() { if (s_tx_chan) i2s_channel_enable(s_tx_chan); }
#endif
```

**IDF 4.x — `i2s_config_t` changes:**
```cpp
.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
.tx_desc_auto_clear = true,       // silence TX DMA when idle (was false)
// i2s_pin_config_t:
.data_out_num       = PIN_I2S_DOUT,   // was I2S_PIN_NO_CHANGE
```
On IDF 4.x, `mic_get_tx_chan()` / `mic_enable_tx_chan()` are not compiled in; `sound.cpp` uses `i2s_write(I2S_NUM_0, ...)` directly.

## New Files

### `src/sound.h`

```cpp
#pragma once

void sound_init();          // call in setup() after mic_init()
void sound_flap();
void sound_score();
void sound_die();
void sound_menu_tap();
void sound_music_start();   // call on STATE_PLAYING entry
void sound_music_stop();    // call on STATE_DEAD transition
```

### `src/sound.cpp`

**Includes:**
```
"sound.h", "config.h", "mic.h", <Arduino.h>
#ifdef USE_NEW_I2S_API: <driver/i2s_std.h>
#else: <driver/i2s.h>
```

**`SoundEvent` enum:**
```cpp
enum SoundEvent : uint8_t {
    SND_FLAP, SND_SCORE, SND_DIE, SND_MENU_TAP,
    SND_MUSIC_START, SND_MUSIC_STOP
};
```

**Melody table:**
```cpp
struct Note { uint16_t freq_hz; uint16_t dur_ms; };
// freq_hz = 0 → rest (silence)
static const Note MELODY[] = {
    {523, 100}, {659, 100}, {784, 100}, {659, 100},  // C5 E5 G5 E5
    {523, 100}, {587, 100}, {659, 200}, {  0,  50},  // C5 D5 E5 rest
    {784, 100}, {659, 100}, {587, 100}, {523, 100},  // G5 E5 D5 C5
    {698, 100}, {659, 100}, {587, 100}, {523, 300},  // F5 E5 D5 C5
};
static constexpr int MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);
```

**State:**
```cpp
static QueueHandle_t s_queue   = nullptr;
static bool          s_soundOk = false;
static int32_t       s_buf[128];    // 512 bytes, file scope — no stack allocation

// Music state (owned by sound_task only — no cross-task access)
struct MusicState {
    bool    on              = false;
    int     note_idx        = 0;    // current note in MELODY[]
    int     samples_rem     = 0;    // samples remaining in current note
    float   half_period_f   = 0.f;  // 16000/(2*freq); 0.f when freq==0 (rest)
    int     hp_counter      = 0;    // samples into current half-period; resets at zero-crossing
    int32_t sign            = 1;    // current square-wave polarity
};
```

**Note loading procedure** (called on `SND_MUSIC_START` and when `samples_rem` reaches 0):
```
note = MELODY[note_idx]
samples_rem   = (note.dur_ms * 16000) / 1000
half_period_f = (note.freq_hz == 0) ? 0.f : 16000.0f / (2.0f * note.freq_hz)
hp_counter    = 0
sign          = 1
```

**Square-wave synthesis (shared by effects and music):**

For frequency `f` Hz at 16000 Hz sample rate:
- `half_period_f = 16000.0f / (2.0f * f)`, sampled once per half-period (at zero-crossing)
- `hp_counter` increments per sample; when `hp_counter >= (int)half_period_f`: toggle `sign`, reset `hp_counter = 0`, resample `half_period_f`
- `half_period_f == 0.f` (rest note, `freq_hz == 0`): output 0 for all samples; skip oscillator logic entirely (no divide-by-zero)
- Amplitude: `sign * (INT32_MAX / 4)`

**`sound_die()` sweep:**
```
progress = (float)samples_written / total_samples
half_period_f = hp_start + (hp_end - hp_start) * progress
```
Resampled at each zero-crossing. `hp_start = 16000/(2×220) ≈ 36.36`, `hp_end = 16000/(2×110) ≈ 72.72`.

**`sound_task` loop (Core 0, 4 KB stack):**
```
loop:
  if music.on:
    fill one chunk (up to 128 samples) of current melody note into s_buf:
      for each sample slot:
        if half_period_f == 0.f: s_buf[i] = 0   // rest
        else:
          s_buf[i] = sign * (INT32_MAX / 4)
          if ++hp_counter >= (int)half_period_f:
            sign = -sign; hp_counter = 0; resample half_period_f
        samples_rem--
        if samples_rem == 0:
          note_idx = (note_idx + 1) % MELODY_LEN
          load next note (note loading procedure above)
          break chunk early if desired (simplest: continue filling with next note)
    write chunk to I2S (IDF5: i2s_channel_write / IDF4: i2s_write), timeout 200 ms
    poll queue with timeout=0:
      SND_MUSIC_STOP  → music.on = false; memset s_buf to 0; write silent chunk to flush DMA
      SND_MUSIC_START → ignore (already on)
      effect event    → play effect to completion (inner write loop), then resume music
                        (MusicState preserved; minor phase click at resume is accepted)
  else:
    xQueueReceive(s_queue, &evt, portMAX_DELAY)   // block until something arrives
    SND_MUSIC_START → note_idx=0; load note 0; music.on=true
    SND_MUSIC_STOP  → ignore (already off)
    effect event    → play effect to completion
```

**Effect playback (sub-routine used in both branches):**
```
total_samples = 16000 * dur_ms / 1000
init synthesis state for the effect frequency
while samples_written < total_samples:
  fill s_buf, write to I2S
```

**`sound_init()`:**
```cpp
void sound_init() {
    if (s_soundOk) return;   // double-init guard
#ifdef USE_NEW_I2S_API
    // mic_init() has multiple early-return error paths; TX handle may be null
    i2s_chan_handle_t tx = mic_get_tx_chan();
    if (!tx) { Serial.println("[sound] no TX chan — mic_init failed?"); return; }
    mic_enable_tx_chan();
#endif
    pinMode(PIN_I2S_AMP_EN, OUTPUT);
    digitalWrite(PIN_I2S_AMP_EN, LOW);
    s_queue = xQueueCreate(8, sizeof(SoundEvent));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 1, nullptr, 0);
    s_soundOk = true;
    Serial.println("[sound] OK");
}
```

**`sound_*()` functions:**
```cpp
static void post(SoundEvent e) { if (s_soundOk) xQueueSend(s_queue, &e, 0); }
void sound_flap()         { post(SND_FLAP); }
void sound_score()        { post(SND_SCORE); }
void sound_die()          { post(SND_DIE); }
void sound_menu_tap()     { post(SND_MENU_TAP); }
void sound_music_start()  { post(SND_MUSIC_START); }
void sound_music_stop()   { post(SND_MUSIC_STOP); }
```

## Sound Events

| Function | Frequency | Duration | Note | Character |
|---|---|---|---|---|
| `sound_flap()` | 880 Hz | 60 ms | A5 | Short chirp |
| `sound_score()` | 1047 Hz | 80 ms | C6 | Blip |
| `sound_die()` | 220 → 110 Hz | 300 ms | A3→A2 | Descending sweep |
| `sound_menu_tap()` | 660 Hz | 40 ms | E5 | Click |

**Melody (C major loop, ~2 s):**

| Notes | Freq (Hz) | Dur (ms) |
|---|---|---|
| C5 E5 G5 E5 | 523 659 784 659 | 100 each |
| C5 D5 E5 rest | 523 587 659 0 | 100 100 200 50 |
| G5 E5 D5 C5 | 784 659 587 523 | 100 each |
| F5 E5 D5 C5 | 698 659 587 523 | 100 100 100 300 |

## `src/main.cpp` Changes

- `#include "sound.h"`
- `setup()`: call `sound_init()` after `mic_init()`
- `bird.flap()` call sites → also `sound_flap()`
- Score increment (pipe passed) → also `sound_score()`
- `STATE_DEAD` transition (both pipe-hit and out-of-bounds) → `sound_die()` then `sound_music_stop()`
- `STATE_PLAYING` entry — **two call sites**, both must call `sound_music_start()`:
  1. `STATE_SPLASH` path: `if (touched || micReady) { resetGame(); state = STATE_PLAYING; sound_music_start(); }`
  2. `STATE_DEAD` retry path: `resetGame(); state = STATE_PLAYING; sound_music_start();`
- `handleMenuTouch()` game button only → `sound_menu_tap()`; calibrate button: no sound
- `STATE_DEAD → STATE_MENU` (hold path): no additional stop call needed — `sound_music_stop()` is already called at the `STATE_DEAD` transition entry, so music is already off before the player navigates to menu.

## Call Order Requirement

```
setup():
  mic_init()    ← allocates both TX and RX I2S channels; does NOT enable TX
  sound_init()  ← enables TX channel, enables amp, starts task
```

## Non-Goals

- No volume control.
- No WAV/MP3 playback.
- No mixing (effects interrupt music; music resumes after).
