# Sound System Design — Flabby Bird

**Date:** 2026-03-18
**Status:** Approved (v3 — post second spec review)

## Overview

Add square-wave sound effects to Flabby Bird using the onboard I2S amplifier on the 2.8" ESP32-S3 display board. The I2S peripheral already used for microphone input is reconfigured for full-duplex operation, adding speaker output on IO6 with no extra hardware required.

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
- A FreeRTOS sound task runs on **Core 0** alongside `mic_task`, writing square-wave PCM to I2S TX in fixed-size chunks.
- `mic_init()` allocates a full-duplex I2S channel (both TX and RX handles) but does **not** enable the TX channel — that is deferred to `sound_init()`.
- `mic.h` exposes `mic_enable_tx_chan()` and `mic_get_tx_chan()` so `sound_init()` can enable and obtain the TX handle.
- `sound_init()` enables the TX channel, enables the amplifier (IO1 LOW), and starts the sound task.
- The game loop (Core 1) calls fire-and-forget `sound_*()` functions that post to a `QueueHandle_t` (depth 4). If the queue is full the event is silently dropped — no blocking. **The priority "die > score > flap > menu tap" is a design intent description, not mechanically enforced** — rapid flap events can fill the queue before a die event is posted.

## `config.h` Changes

Move `USE_NEW_I2S_API` definition here so it is shared across `mic.cpp`, `mic.h`, and `sound.cpp`:

```c
// IDF ≥ 5.0 uses the new i2s_std.h API; IDF < 5.0 uses legacy driver/i2s.h
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  define USE_NEW_I2S_API 1
#endif

#define PIN_I2S_DOUT    6
#define PIN_I2S_AMP_EN  1
```

Remove the `#if ESP_IDF_VERSION ...` / `#define USE_NEW_I2S_API` block from `mic.cpp` (it will now come from `config.h`). Add `#include <esp_idf_version.h>` to `config.h` before the guard.

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
static i2s_chan_handle_t s_tx_chan = nullptr;  // NEW — file scope, not inside mic_init()
#endif
```

**Remove the `USE_NEW_I2S_API` definition block** — it now comes from `config.h`.

**IDF 5.x — inside `mic_init()`, after RX channel is init'd and enabled:**
```cpp
// Allocate TX+RX pair (change from nullptr to &s_tx_chan)
i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);

// ... existing RX init and i2s_channel_enable(s_rx_chan) unchanged ...

// Init TX channel (separate config struct — DOUT only, DIN unused)
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
// NOTE: do NOT call i2s_channel_enable(s_tx_chan) here.
// TX is enabled by sound_init() to avoid DMA noise before the sound task starts.
```

**New accessor functions (IDF 5.x):**
```cpp
#ifdef USE_NEW_I2S_API
i2s_chan_handle_t mic_get_tx_chan() { return s_tx_chan; }
void mic_enable_tx_chan() {
    if (s_tx_chan) i2s_channel_enable(s_tx_chan);
}
#endif
```

**IDF 4.x — `i2s_config_t` changes:**
```cpp
.mode             = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
.tx_desc_auto_clear = true,    // silence TX DMA when not writing (was false)
```
```cpp
// i2s_pin_config_t:
.data_out_num = PIN_I2S_DOUT,  // was I2S_PIN_NO_CHANGE
```
On IDF 4.x, `mic_get_tx_chan()` and `mic_enable_tx_chan()` are not compiled in. `sound.cpp` uses `i2s_write(I2S_NUM_0, ...)` directly on the IDF 4.x path.

## New Files

### `src/sound.h`

```cpp
#pragma once

void sound_init();      // call in setup() after mic_init()
void sound_flap();
void sound_score();
void sound_die();
void sound_menu_tap();
```

### `src/sound.cpp`

```
Includes: "sound.h", "config.h", "mic.h", <Arduino.h>, <esp_idf_version.h>
#ifdef USE_NEW_I2S_API: #include <driver/i2s_std.h>
#else: #include <driver/i2s.h>
```

**State:**
```cpp
static QueueHandle_t s_queue  = nullptr;
static bool          s_soundOk = false;
static int32_t       s_buf[128];   // 512 bytes, file scope — no stack allocation
```

**`SoundEvent` enum:** `SND_FLAP`, `SND_SCORE`, `SND_DIE`, `SND_MENU_TAP`

**Square-wave synthesis algorithm:**

For a tone of frequency `f` Hz at 16000 Hz sample rate:
- `half_period_f` = `16000.0f / (2.0f * f)` — float, sampled **once per half-period** (at zero-crossing reset)
- `counter` tracks samples within the current half-period; resets to 0 at each zero-crossing
- When `counter >= (int)half_period_f`: toggle sign, reset counter to 0, resample `half_period_f`
- Amplitude: `±(INT32_MAX / 4)`

For `sound_die()` sweep: `half_period_f` is resampled at each zero-crossing using:
```
progress = (float)samples_written / total_samples   // 0.0 → 1.0
half_period_f = hp_start + (hp_end - hp_start) * progress
```
Where `hp_start = 16000/(2×220)` ≈ 36.36, `hp_end = 16000/(2×110)` ≈ 72.72.
`samples_written` is the running total across all chunks, incremented per sample.

**`sound_task` (Core 0, 4 KB stack):**
```
loop:
  xQueueReceive(s_queue, &evt, portMAX_DELAY)
  determine total_samples = sample_rate * duration_ms / 1000
  initialize synthesis state (half_period_f, counter, sign, samples_written=0)
  while samples_written < total_samples:
    fill s_buf[0..127] with next chunk of square-wave samples
    write chunk via:
      IDF 5.x: i2s_channel_write(s_tx_chan, s_buf, bytes, &written, pdMS_TO_TICKS(200))
      IDF 4.x: i2s_write(I2S_NUM_0, s_buf, bytes, &written, pdMS_TO_TICKS(200))
    samples_written += written / sizeof(int32_t)
```

**`sound_init()`:**
```cpp
void sound_init() {
    if (s_soundOk) return;   // double-init guard

#ifdef USE_NEW_I2S_API
    i2s_chan_handle_t tx = mic_get_tx_chan();
    if (!tx) { Serial.println("[sound] no TX chan"); return; }
    mic_enable_tx_chan();
#endif

    pinMode(PIN_I2S_AMP_EN, OUTPUT);       // must precede digitalWrite
    digitalWrite(PIN_I2S_AMP_EN, LOW);     // enable amplifier

    s_queue = xQueueCreate(4, sizeof(SoundEvent));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 1, nullptr, 0);
    // If stack overflow occurs, increase to 6144 and check uxTaskGetStackHighWaterMark().
    s_soundOk = true;
    Serial.println("[sound] OK");
}
```

**`sound_*()` functions:**
```cpp
void sound_flap()     { SoundEvent e = SND_FLAP;     if (s_soundOk) xQueueSend(s_queue, &e, 0); }
void sound_score()    { SoundEvent e = SND_SCORE;    if (s_soundOk) xQueueSend(s_queue, &e, 0); }
void sound_die()      { SoundEvent e = SND_DIE;      if (s_soundOk) xQueueSend(s_queue, &e, 0); }
void sound_menu_tap() { SoundEvent e = SND_MENU_TAP; if (s_soundOk) xQueueSend(s_queue, &e, 0); }
```

## Sound Events

| Function | Frequency | Duration | Note | Character |
|---|---|---|---|---|
| `sound_flap()` | 880 Hz | 60 ms | A5 | Short chirp up |
| `sound_score()` | 1047 Hz | 80 ms | C6 | Short blip |
| `sound_die()` | 220 → 110 Hz | 300 ms | A3→A2 | Descending octave sweep |
| `sound_menu_tap()` | 660 Hz | 40 ms | E5 | Soft click |

## `src/main.cpp` Changes

- `#include "sound.h"`
- `setup()`: call `sound_init()` after `mic_init()`.
- `bird.flap()` call sites → also call `sound_flap()`.
- Pipe-passed score increment → also call `sound_score()`.
- Death transition (`state = STATE_DEAD`) → also call `sound_die()`.
- `handleMenuTouch()`, game button branch only → call `sound_menu_tap()`. The calibrate button does **not** trigger a sound.

## Call Order Requirement

```
setup():
  mic_init()    ← allocates both TX and RX I2S channels; does NOT enable TX
  sound_init()  ← enables TX channel, enables amp, starts task
```

## Non-Goals

- No background music.
- No volume control.
- No WAV/MP3 playback.
