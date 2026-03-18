# Sound System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add square-wave sound effects (flap, score, die, menu tap) and looping chiptune background music to Flabby Bird using the onboard I2S amplifier on the ESP32-S3 board.

**Architecture:** A new `sound.h/cpp` module runs a FreeRTOS sound task on Core 0. The existing I2S mic channel is upgraded to full-duplex (TX+RX) in `mic_init()`; `sound_init()` retrieves the TX handle via `mic_get_tx_chan()` and enables it. Music plays chunk-by-chunk with effects preempting mid-chunk; the task resumes music after each effect finishes.

**Tech Stack:** Arduino/PlatformIO on ESP32-S3, FreeRTOS `xTaskCreatePinnedToCore` + `xQueueCreate`, IDF 5.x `driver/i2s_std.h` (+ IDF 4.x legacy fallback), `Adafruit_ILI9341`.

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `src/config.h` | Modify | Add `USE_NEW_I2S_API` guard, `PIN_I2S_DOUT=6`, `PIN_I2S_AMP_EN=1` |
| `src/mic.h` | Modify | Add `mic_get_tx_chan()` and `mic_enable_tx_chan()` declarations (IDF 5.x) |
| `src/mic.cpp` | Modify | Upgrade to full-duplex I2S; add TX init, accessors; IDF 4.x TX mode |
| `src/sound.h` | Create | Public API: `sound_init/flap/score/die/menu_tap/music_start/music_stop` |
| `src/sound.cpp` | Create | Sound task, square-wave synthesis, melody loop, effect playback |
| `src/main.cpp` | Modify | Wire sound calls into game state transitions and flap/score/death events |

---

## Task 1: Update `config.h` — shared IDF version guard and new pin constants

**Files:**
- Modify: `src/config.h` (top of file, before other includes)

**Context:** `USE_NEW_I2S_API` is currently defined only inside `mic.cpp`, making it invisible to `mic.h` and `sound.cpp`. Moving it to `config.h` (which every source file already includes) makes it a shared compile-time switch. `PIN_I2S_DOUT` (IO6, speaker data out) and `PIN_I2S_AMP_EN` (IO1, amplifier enable, active LOW) are new hardware constants.

- [ ] **Step 1: Add IDF version guard and pin constants to `config.h`**

Open `src/config.h`. At the very top, before the `#pragma once` line is already there — add these lines immediately after `#pragma once`:

```c
#pragma once
#include <cstdint>
#include <esp_idf_version.h>

// IDF ≥ 5.0 uses driver/i2s_std.h (new API); IDF < 5.0 uses legacy driver/i2s.h
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  define USE_NEW_I2S_API 1
#endif
```

Then add the two new pin constants in the microphone section (after `PIN_I2S_DATA`):

```c
#define PIN_I2S_DOUT     6    // I2S data output → onboard speaker amp
#define PIN_I2S_AMP_EN   1    // Amplifier enable, active LOW (IO1)
```

The existing file already has `#pragma once` and `#include <cstdint>` — only add the lines that are missing. Do not duplicate them.

- [ ] **Step 2: Build to verify no compile errors**

```bash
cd /Users/obito/Projects/NGHIALV/esp32-flappybird
pio run 2>&1 | tail -20
```

Expected: `SUCCESS` (or existing errors only — no new errors from config.h changes).

- [ ] **Step 3: Commit**

```bash
git add src/config.h
git commit -m "feat: add USE_NEW_I2S_API guard and audio pin constants to config.h"
```

---

## Task 2: Update `mic.h` and `mic.cpp` — full-duplex I2S with TX channel

**Files:**
- Modify: `src/mic.h`
- Modify: `src/mic.cpp`

**Context:** Currently `mic_init()` allocates only an RX channel (`i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan)`). We need to pass `&s_tx_chan` as the first output param to get a full-duplex pair from the same `I2S_NUM_0` peripheral. The TX channel is initialised (so the hardware pins are claimed) but **not enabled** here — that is deferred to `sound_init()` to avoid DMA noise before the sound task exists. Two accessor functions let `sound.cpp` retrieve and enable the handle.

### Step 2a: Update `mic.h`

- [ ] **Step 1: Replace `src/mic.h` with the new version**

```cpp
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
```

### Step 2b: Update `mic.cpp`

- [ ] **Step 2: Remove the `USE_NEW_I2S_API` definition block from `mic.cpp`**

The current file has these lines near the top:
```cpp
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#  define USE_NEW_I2S_API 1
#  include <driver/i2s_std.h>
   static i2s_chan_handle_t s_rx_chan = nullptr;
#else
#  include <driver/i2s.h>
#endif
```

Replace with (the definition now comes from `config.h`):
```cpp
#ifdef USE_NEW_I2S_API
#  include <driver/i2s_std.h>
static i2s_chan_handle_t s_rx_chan = nullptr;
static i2s_chan_handle_t s_tx_chan = nullptr;   // full-duplex TX — enabled by sound_init()
#else
#  include <driver/i2s.h>
#endif
```

- [ ] **Step 3: Change `i2s_new_channel` to allocate full-duplex pair (IDF 5.x)**

Find this line inside `mic_init()`:
```cpp
err = i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan);
```

Change to:
```cpp
err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
```

- [ ] **Step 4: Add TX channel init after RX init (IDF 5.x, inside `mic_init()`)**

After the existing `i2s_channel_enable(s_rx_chan)` call (and its error check), add:

```cpp
    // ── TX channel init (speaker out on IO6) ──────────────────────────────
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
    err = i2s_channel_init_std_mode(s_tx_chan, &tx_cfg);
    if (err != ESP_OK) {
        Serial.printf("[mic] TX init failed: %s\n", esp_err_to_name(err));
        // TX unavailable; s_tx_chan left non-null but uninitialised — sound_init() will detect null handle
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
    }
    // Do NOT enable s_tx_chan here — sound_init() calls mic_enable_tx_chan().
```

- [ ] **Step 5: Add accessor functions at the bottom of the `#ifdef USE_NEW_I2S_API` block (after `mic_clap_ready()`)**

```cpp
#ifdef USE_NEW_I2S_API
i2s_chan_handle_t mic_get_tx_chan()  { return s_tx_chan; }
void mic_enable_tx_chan() {
    if (s_tx_chan) i2s_channel_enable(s_tx_chan);
}
#endif
```

- [ ] **Step 6: Update IDF 4.x path — add TX mode and output pin**

Find the `i2s_config_t` struct in the `#else` branch. Change:
```cpp
.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
...
.tx_desc_auto_clear   = false,
```
To:
```cpp
.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
...
.tx_desc_auto_clear   = true,    // silence DMA when not writing
```

Find the `i2s_pin_config_t` struct. Change:
```cpp
.data_out_num = I2S_PIN_NO_CHANGE,
```
To:
```cpp
.data_out_num = PIN_I2S_DOUT,
```

- [ ] **Step 7: Build**

```bash
pio run 2>&1 | tail -20
```

Expected: `SUCCESS`. If there are errors in `mic.cpp`, fix before continuing.

- [ ] **Step 8: Commit**

```bash
git add src/mic.h src/mic.cpp
git commit -m "feat: upgrade I2S to full-duplex; add TX channel init and accessors"
```

---

## Task 3: Create `sound.h` and `sound.cpp`

**Files:**
- Create: `src/sound.h`
- Create: `src/sound.cpp`

**Context:** This is the entire audio engine. The sound task runs on Core 0 (same core as `mic_task`). It handles both music (continuous looping melody) and effects (one-shot tones). Effects preempt music: when an effect arrives while music is playing, the task plays the effect to completion then resumes music from the saved note position.

### Step 3a: Create `sound.h`

- [ ] **Step 1: Create `src/sound.h`**

```cpp
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
void sound_music_start(); // begin looping melody (call on STATE_PLAYING entry)
void sound_music_stop();  // stop melody (call on STATE_DEAD transition)
```

### Step 3b: Create `sound.cpp`

- [ ] **Step 2: Create `src/sound.cpp`**

```cpp
#include "sound.h"
#include "config.h"
#include "mic.h"
#include <Arduino.h>

#ifdef USE_NEW_I2S_API
#  include <driver/i2s_std.h>
#else
#  include <driver/i2s.h>
#endif

// ── Types ─────────────────────────────────────────────────────────────────────

enum SoundEvent : uint8_t {
    SND_FLAP, SND_SCORE, SND_DIE, SND_MENU_TAP,
    SND_MUSIC_START, SND_MUSIC_STOP
};

struct EffectDef { uint16_t freq_hz; uint16_t dur_ms; };

struct Note { uint16_t freq_hz; uint16_t dur_ms; };

struct MusicState {
    bool    on            = false;
    int     note_idx      = 0;
    int     samples_rem   = 0;
    float   half_period_f = 0.f;
    int     hp_counter    = 0;
    int32_t sign          = 1;
};

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int     SAMPLE_RATE = 16000;
static constexpr int32_t AMPLITUDE   = INT32_MAX / 4;
static constexpr int     CHUNK       = 128;   // samples per I2S write

static const EffectDef EFFECTS[] = {
    { 880, 60  },   // SND_FLAP      = 0
    { 1047, 80 },   // SND_SCORE     = 1
    { 0,   300 },   // SND_DIE       = 2 — freq unused; sweep handled in play_effect
    { 660, 40  },   // SND_MENU_TAP  = 3
};
// EFFECTS[] indices must stay in sync with SoundEvent values 0-3.
static_assert((int)SND_MENU_TAP == 3, "EFFECTS[] index must match SoundEvent values 0-3");
static_assert(sizeof(EFFECTS)/sizeof(EFFECTS[0]) == 4, "EFFECTS[] size mismatch with SoundEvent");

static const Note MELODY[] = {
    {523, 100}, {659, 100}, {784, 100}, {659, 100},  // C5 E5 G5 E5
    {523, 100}, {587, 100}, {659, 200}, {  0,  50},  // C5 D5 E5 rest
    {784, 100}, {659, 100}, {587, 100}, {523, 100},  // G5 E5 D5 C5
    {698, 100}, {659, 100}, {587, 100}, {523, 300},  // F5 E5 D5 C5
};
static constexpr int MELODY_LEN = (int)(sizeof(MELODY) / sizeof(MELODY[0]));

// ── State (all owned by sound_task — no cross-task access after init) ─────────

static QueueHandle_t s_queue   = nullptr;
static bool          s_soundOk = false;
static int32_t       s_buf[CHUNK];   // file scope — never on the stack

// ── I2S write helper ──────────────────────────────────────────────────────────

static void i2s_write_buf(int samples) {
    size_t written = 0;
    const size_t bytes = (size_t)samples * sizeof(int32_t);
#ifdef USE_NEW_I2S_API
    extern i2s_chan_handle_t mic_get_tx_chan();
    i2s_channel_write(mic_get_tx_chan(), s_buf, bytes, &written, pdMS_TO_TICKS(200));
#else
    i2s_write(I2S_NUM_0, s_buf, bytes, &written, pdMS_TO_TICKS(200));
#endif
}

// ── Note loading ──────────────────────────────────────────────────────────────

static void load_note(MusicState& m) {
    const Note& n = MELODY[m.note_idx];
    m.samples_rem   = (n.dur_ms * SAMPLE_RATE) / 1000;
    m.half_period_f = (n.freq_hz == 0) ? 0.f : (float)SAMPLE_RATE / (2.f * n.freq_hz);
    m.hp_counter    = 0;
    m.sign          = 1;
}

// ── Effect playback ───────────────────────────────────────────────────────────

static void play_effect(SoundEvent evt) {
    const int total = (int)((uint32_t)EFFECTS[evt].dur_ms * SAMPLE_RATE / 1000);

    // Die: descending sweep 220 → 110 Hz
    const bool is_die = (evt == SND_DIE);
    const float hp_start = (float)SAMPLE_RATE / (2.f * 220.f);
    const float hp_end   = (float)SAMPLE_RATE / (2.f * 110.f);
    float hp  = (is_die) ? hp_start : (float)SAMPLE_RATE / (2.f * EFFECTS[evt].freq_hz);
    int   hpc = 0;
    int32_t sign = 1;
    int written_total = 0;

    while (written_total < total) {
        int chunk = (total - written_total < CHUNK) ? (total - written_total) : CHUNK;
        for (int i = 0; i < chunk; i++) {
            if (hp == 0.f) {
                s_buf[i] = 0;
            } else {
                s_buf[i] = sign * AMPLITUDE;
                if (++hpc >= (int)hp) {
                    sign = -sign; hpc = 0;
                    if (is_die) {
                        float progress = (float)(written_total + i) / (float)total;
                        hp = hp_start + (hp_end - hp_start) * progress;
                    }
                }
            }
        }
        i2s_write_buf(chunk);
        written_total += chunk;
    }
}

// ── Music chunk fill ──────────────────────────────────────────────────────────

// Fills up to CHUNK samples of the current melody note into s_buf.
// Advances MusicState; note boundaries are handled mid-chunk transparently.
// Returns number of samples written.
static int fill_music_chunk(MusicState& m) {
    int filled = 0;
    while (filled < CHUNK) {
        if (m.samples_rem <= 0) {
            m.note_idx = (m.note_idx + 1) % MELODY_LEN;
            load_note(m);
        }
        int32_t sample = (m.half_period_f == 0.f) ? 0 : (m.sign * AMPLITUDE);
        s_buf[filled++] = sample;
        if (m.half_period_f > 0.f && ++m.hp_counter >= (int)m.half_period_f) {
            m.sign = -m.sign; m.hp_counter = 0;
        }
        m.samples_rem--;
    }
    return CHUNK;
}

// ── Sound task ────────────────────────────────────────────────────────────────

static void sound_task(void* /*param*/) {
    MusicState music;
    SoundEvent evt;

    for (;;) {
        if (music.on) {
            // Render one chunk of music, then check for events
            fill_music_chunk(music);
            i2s_write_buf(CHUNK);

            if (xQueueReceive(s_queue, &evt, 0) == pdTRUE) {
                if (evt == SND_MUSIC_STOP) {
                    music.on = false;
                    memset(s_buf, 0, sizeof(s_buf));
                    i2s_write_buf(CHUNK);   // flush DMA with silence
                } else if (evt != SND_MUSIC_START) {
                    play_effect(evt);       // MusicState preserved; resumes after
                }
                // SND_MUSIC_START while already on: ignore
            }
        } else {
            // Blocked wait — no CPU used when silent
            if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) continue;

            if (evt == SND_MUSIC_START) {
                music.note_idx = 0;
                load_note(music);
                music.on = true;
            } else if (evt != SND_MUSIC_STOP) {
                play_effect(evt);
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void sound_init() {
    if (s_soundOk) return;

#ifdef USE_NEW_I2S_API
    i2s_chan_handle_t tx = mic_get_tx_chan();
    if (!tx) {
        Serial.println("[sound] no TX chan — mic_init failed?");
        return;
    }
    mic_enable_tx_chan();
#endif

    pinMode(PIN_I2S_AMP_EN, OUTPUT);
    digitalWrite(PIN_I2S_AMP_EN, LOW);   // active LOW — enable amp

    s_queue = xQueueCreate(8, sizeof(SoundEvent));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 1, nullptr, 0);
    // If stack overflow occurs (CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER crash),
    // increase stack to 6144 and check uxTaskGetStackHighWaterMark().
    s_soundOk = true;
    Serial.println("[sound] OK");
}

static void post(SoundEvent e) { if (s_soundOk) xQueueSend(s_queue, &e, 0); }

void sound_flap()        { post(SND_FLAP); }
void sound_score()       { post(SND_SCORE); }
void sound_die()         { post(SND_DIE); }
void sound_menu_tap()    { post(SND_MENU_TAP); }
void sound_music_start() { post(SND_MUSIC_START); }
void sound_music_stop()  { post(SND_MUSIC_STOP); }
```

- [ ] **Step 3: Build**

```bash
pio run 2>&1 | tail -30
```

Expected: `SUCCESS`. Common errors to fix:
- The `extern i2s_chan_handle_t mic_get_tx_chan();` inside `i2s_write_buf` is redundant (mic.h is already included); remove it if it causes a redeclaration warning
- `INT32_MAX` undefined → add `#include <climits>` or use `0x7FFFFFFF`

- [ ] **Step 4: Commit**

```bash
git add src/sound.h src/sound.cpp
git commit -m "feat: add sound module with effects and chiptune music"
```

---

## Task 4: Wire sound calls into `main.cpp`

**Files:**
- Modify: `src/main.cpp`

**Context:** Six integration points. The game already has clear state transitions — each needs one or two extra function calls. No logic changes, only additions.

- [ ] **Step 1: Add `#include "sound.h"` near the top of `main.cpp`**

After the existing includes (around line 16 where `mic.h` is included):
```cpp
#include "sound.h"
```

- [ ] **Step 2: Call `sound_init()` in `setup()` after `mic_init()`**

Find in `setup()`:
```cpp
    Serial.println("[6] mic_init");
    mic_init();
```

Add immediately after:
```cpp
    Serial.println("[7] sound_init");
    sound_init();
```

Renumber the subsequent `Serial.println("[7] preferences")` to `[8]` and `[8] resetGame` to `[9]` (and `[OK]` stays as is).

- [ ] **Step 3: Add `sound_flap()` at every `bird.flap()` call site**

There are two call sites in `loop()`:

```cpp
if (s_pending)       { bird.flap(); s_pending = false; }
if (checkMicFlap())  { bird.flap(); }
```

Change to:
```cpp
if (s_pending)       { bird.flap(); sound_flap(); s_pending = false; }
if (checkMicFlap())  { bird.flap(); sound_flap(); }
```

- [ ] **Step 4: Add `sound_score()` at the score increment**

Find in `updateGame()`:
```cpp
            score++;
            if (score % SPEED_STEP == 0) pipeSpeed += SPEED_INC;
```

Change to:
```cpp
            score++;
            sound_score();
            if (score % SPEED_STEP == 0) pipeSpeed += SPEED_INC;
```

- [ ] **Step 5: Add `sound_die()` and `sound_music_stop()` at both death transitions**

Find the two death sites in `updateGame()`:

```cpp
        if (pipes[i].hitsBird(bird.y)) {
            if (score > hiScore) { hiScore = score; saveHiScore(); }
            deadSince = millis(); state = STATE_DEAD; return;
        }
    }
    if (bird.outOfBounds()) {
        if (score > hiScore) { hiScore = score; saveHiScore(); }
        deadSince = millis(); state = STATE_DEAD;
    }
```

Change to:
```cpp
        if (pipes[i].hitsBird(bird.y)) {
            if (score > hiScore) { hiScore = score; saveHiScore(); }
            sound_die(); sound_music_stop();
            deadSince = millis(); state = STATE_DEAD; return;
        }
    }
    if (bird.outOfBounds()) {
        if (score > hiScore) { hiScore = score; saveHiScore(); }
        sound_die(); sound_music_stop();
        deadSince = millis(); state = STATE_DEAD;
    }
```

- [ ] **Step 6: Add `sound_music_start()` at both STATE_PLAYING entry points**

**Entry point 1** — from STATE_SPLASH (inside `case STATE_SPLASH:` in the switch):
```cpp
    case STATE_SPLASH:
        drawSplash();
        if (touched || micReady) { resetGame(); state = STATE_PLAYING; }
        break;
```
Change to:
```cpp
    case STATE_SPLASH:
        drawSplash();
        if (touched || micReady) { resetGame(); state = STATE_PLAYING; sound_music_start(); }
        break;
```

**Entry point 2** — from STATE_DEAD retry (inside `loop()`, the hold-timer block):
```cpp
            } else if (!touch_finger_down() && elapsed >= 30) {
                s_tracking = false;
                resetGame(); state = STATE_PLAYING;
            }
```
Change to:
```cpp
            } else if (!touch_finger_down() && elapsed >= 30) {
                s_tracking = false;
                resetGame(); state = STATE_PLAYING; sound_music_start();
            }
```

Also the mic-retry path in STATE_DEAD:
```cpp
        if (checkMicFlap()) { s_tracking = false; resetGame(); state = STATE_PLAYING; }
```
Change to:
```cpp
        if (checkMicFlap()) { s_tracking = false; resetGame(); state = STATE_PLAYING; sound_music_start(); }
```

- [ ] **Step 7: Add `sound_menu_tap()` in `handleMenuTouch()` — game button only**

Find the game button branch in `handleMenuTouch()`:
```cpp
        if (tx >= bx && tx < bx + bw && ty >= by && ty < by + bh) {
            resetGame();
            state = STATE_SPLASH;
            return;
        }
```

Change to:
```cpp
        if (tx >= bx && tx < bx + bw && ty >= by && ty < by + bh) {
            sound_menu_tap();
            resetGame();
            state = STATE_SPLASH;
            return;
        }
```

Do **not** add a sound call to the calibrate button branch.

- [ ] **Step 8: Build**

```bash
pio run 2>&1 | tail -20
```

Expected: `SUCCESS`.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire sound effects and music into game state transitions"
```

---

## Task 5: Flash and manual hardware verification

**Context:** This is an embedded project — there is no way to unit-test I2S audio output in CI. Verification is done by flashing and listening.

- [ ] **Step 1: Flash**

```bash
pio run --target upload 2>&1 | tail -20
```

Expected: `Leaving...  Hard resetting via RTS pin...`

- [ ] **Step 2: Open serial monitor and check boot log**

```bash
pio device monitor
```

Expected boot sequence (confirm all lines present):
```
[boot] IDF x.x.x  heap ... B  PSRAM ... B
[1] backlight
[2] tft.begin
[3] canvas OK  heap ... B
[4] sky gradient
[5] touch_init
[6] mic_init
[mic] OK
[7] sound_init
[sound] OK
[8] preferences
[9] resetGame
[OK] setup complete
```

If `[sound] no TX chan` appears, the TX channel init in `mic_init()` failed. Check the `[mic] TX init failed:` error message and fix before continuing.

- [ ] **Step 3: Verify background music**

Tap the "Flabby Bird" game button on the menu → splash screen appears. Tap again to start gameplay. **Expected:** looping chiptune melody starts immediately.

- [ ] **Step 4: Verify flap sound**

During gameplay, tap the screen or clap. **Expected:** short high-pitched chirp on each flap, music continues underneath (resumes after a minor click).

- [ ] **Step 5: Verify score sound**

Let the bird pass through a pipe gap. **Expected:** short blip sound on each scored pipe.

- [ ] **Step 6: Verify die sound and music stop**

Let the bird hit a pipe or the ground. **Expected:** descending sweep sound, then silence (music stopped). Game-over screen should be quiet.

- [ ] **Step 7: Verify retry restarts music**

From the game-over screen, tap RETRY. **Expected:** music restarts from the beginning of the melody.

- [ ] **Step 8: Verify menu tap**

From the game-over screen, hold MENU. On the main menu, tap the Flabby Bird button. **Expected:** short click sound on the tap.

- [ ] **Step 9: Final commit tag**

```bash
git tag v1.1-sound
```

---

## Troubleshooting Reference

| Symptom | Likely cause | Fix |
|---|---|---|
| No sound at all | Amp enable not firing; IO1 still HIGH | Check `digitalWrite(PIN_I2S_AMP_EN, LOW)` in `sound_init()` |
| Constant loud noise on boot | TX DMA enabled before sound task | Confirm `i2s_channel_enable(s_tx_chan)` is in `mic_enable_tx_chan()`, not `mic_init()` |
| Mic stops detecting claps | I2S RX broken by TX init | Confirm `i2s_new_channel` error path deletes `s_tx_chan` only, not `s_rx_chan` |
| Music doesn't start | `sound_music_start()` missing at one entry point | Check both STATE_SPLASH and STATE_DEAD retry paths |
| Music keeps playing after death | `sound_music_stop()` missing at one death site | Check both `hitsBird` and `outOfBounds` branches |
| Stack overflow crash on Core 0 | sound task stack too small | Increase `xTaskCreatePinnedToCore` stack arg from 4096 to 6144 |
