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
    SND_FLAP       = 0,
    SND_SCORE      = 1,
    SND_DIE        = 2,
    SND_MENU_TAP   = 3,
    SND_MUSIC_START = 4,
    SND_MUSIC_STOP  = 5,
};

struct EffectDef { uint16_t freq_hz; uint16_t dur_ms; };

struct Note { uint16_t freq_hz; uint16_t dur_ms; };

struct MusicState {
    bool    on            = false;
    int     note_idx      = 0;
    int     samples_rem   = 0;
    float   half_period_f = 0.f;  // 16000/(2*freq); 0.f when freq==0 (rest)
    int     hp_counter    = 0;    // samples into current half-period
    int32_t sign          = 1;    // square-wave polarity
};

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int     SAMPLE_RATE = 16000;
static constexpr int32_t AMPLITUDE   = INT32_MAX / 4;  // -6 dB headroom
static constexpr int     CHUNK       = 128;            // samples per I2S write (~8 ms)

// Index must match SoundEvent values 0-3.
static const EffectDef EFFECTS[] = {
    { 880,  60 },   // SND_FLAP      = 0  (A5, wing chirp)
    { 1047, 80 },   // SND_SCORE     = 1  (C6, point blip)
    { 0,   300 },   // SND_DIE       = 2  — freq unused; sweep handled in play_effect
    { 660,  40 },   // SND_MENU_TAP  = 3  (E5, UI click)
};
static_assert((int)SND_MENU_TAP == 3,
    "EFFECTS[] index must match SoundEvent values 0-3");
static_assert(sizeof(EFFECTS) / sizeof(EFFECTS[0]) == 4,
    "EFFECTS[] size mismatch with SoundEvent");

// C major loop, ~2 s. freq_hz=0 → rest (silence).
static const Note MELODY[] = {
    {523, 100}, {659, 100}, {784, 100}, {659, 100},  // C5 E5 G5 E5
    {523, 100}, {587, 100}, {659, 200}, {  0,  50},  // C5 D5 E5 rest
    {784, 100}, {659, 100}, {587, 100}, {523, 100},  // G5 E5 D5 C5
    {698, 100}, {659, 100}, {587, 100}, {523, 300},  // F5 E5 D5 C5
};
static constexpr int MELODY_LEN = (int)(sizeof(MELODY) / sizeof(MELODY[0]));

// ── Module state ──────────────────────────────────────────────────────────────

static QueueHandle_t s_queue   = nullptr;
static bool          s_soundOk = false;
static int32_t       s_buf[CHUNK];   // file scope — never allocated on the stack

// ── I2S write helper ──────────────────────────────────────────────────────────

static void i2s_write_buf(int samples) {
    size_t written = 0;
    const size_t bytes = (size_t)samples * sizeof(int32_t);
#ifdef USE_NEW_I2S_API
    i2s_channel_write(mic_get_tx_chan(), s_buf, bytes, &written, pdMS_TO_TICKS(200));
#else
    i2s_write(I2S_NUM_0, s_buf, bytes, &written, pdMS_TO_TICKS(200));
#endif
}

// ── Note loading ──────────────────────────────────────────────────────────────

static void load_note(MusicState& m) {
    const Note& n = MELODY[m.note_idx];
    m.samples_rem   = ((int)n.dur_ms * SAMPLE_RATE) / 1000;
    m.half_period_f = (n.freq_hz == 0) ? 0.f
                                       : (float)SAMPLE_RATE / (2.f * n.freq_hz);
    m.hp_counter    = 0;
    m.sign          = 1;
}

// ── Effect playback ───────────────────────────────────────────────────────────

static void play_effect(SoundEvent evt) {
    const int total = ((int)EFFECTS[evt].dur_ms * SAMPLE_RATE) / 1000;

    const bool  is_die   = (evt == SND_DIE);
    const float hp_start = (float)SAMPLE_RATE / (2.f * 220.f);  // A3
    const float hp_end   = (float)SAMPLE_RATE / (2.f * 110.f);  // A2

    float   hp   = is_die ? hp_start
                           : (float)SAMPLE_RATE / (2.f * EFFECTS[evt].freq_hz);
    int     hpc  = 0;
    int32_t sign = 1;
    int     done = 0;

    while (done < total) {
        const int chunk = (total - done < CHUNK) ? (total - done) : CHUNK;
        for (int i = 0; i < chunk; i++) {
            if (hp == 0.f) {
                s_buf[i] = 0;
            } else {
                s_buf[i] = sign * AMPLITUDE;
                if (++hpc >= (int)hp) {
                    sign = -sign;
                    hpc  = 0;
                    if (is_die) {
                        float progress = (float)(done + i) / (float)total;
                        hp = hp_start + (hp_end - hp_start) * progress;
                    }
                }
            }
        }
        i2s_write_buf(chunk);
        done += chunk;
    }
}

// ── Music chunk fill ──────────────────────────────────────────────────────────

// Fills exactly CHUNK samples of the current melody into s_buf.
// Advances MusicState; handles note boundaries transparently mid-chunk.
static void fill_music_chunk(MusicState& m) {
    for (int i = 0; i < CHUNK; i++) {
        if (m.samples_rem <= 0) {
            m.note_idx = (m.note_idx + 1) % MELODY_LEN;
            load_note(m);
        }
        if (m.half_period_f == 0.f) {
            s_buf[i] = 0;
        } else {
            s_buf[i] = m.sign * AMPLITUDE;
            if (++m.hp_counter >= (int)m.half_period_f) {
                m.sign       = -m.sign;
                m.hp_counter = 0;
            }
        }
        m.samples_rem--;
    }
}

// ── Sound task ────────────────────────────────────────────────────────────────

static void sound_task(void* /*param*/) {
    MusicState music;
    SoundEvent evt;

    for (;;) {
        if (music.on) {
            // Render one ~8 ms chunk of music, then check for new events.
            fill_music_chunk(music);
            i2s_write_buf(CHUNK);

            if (xQueueReceive(s_queue, &evt, 0) == pdTRUE) {
                if (evt == SND_MUSIC_STOP) {
                    music.on = false;
                    memset(s_buf, 0, sizeof(s_buf));
                    i2s_write_buf(CHUNK);       // flush DMA with silence
                } else if (evt == SND_MUSIC_START) {
                    // Already on — ignore
                } else {
                    play_effect(evt);           // MusicState preserved; resumes after
                    // Known: minor phase click at resume point (DMA state not preserved)
                }
            }
        } else {
            // Music off — block until something arrives (no CPU waste).
            if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) continue;

            if (evt == SND_MUSIC_START) {
                music.note_idx = 0;
                load_note(music);
                music.on = true;
            } else if (evt == SND_MUSIC_STOP) {
                // Already off — ignore
            } else {
                play_effect(evt);
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void sound_init() {
    if (s_soundOk) return;   // double-init guard

#ifdef USE_NEW_I2S_API
    // mic_init() has multiple early-return error paths; TX handle may be null.
    i2s_chan_handle_t tx = mic_get_tx_chan();
    if (!tx) {
        Serial.println("[sound] no TX chan — mic_init failed?");
        return;
    }
    mic_enable_tx_chan();
#endif

    pinMode(PIN_I2S_AMP_EN, OUTPUT);
    digitalWrite(PIN_I2S_AMP_EN, LOW);   // active LOW — enable amplifier

    s_queue = xQueueCreate(8, sizeof(SoundEvent));
    // Stack: 4 KB. If overflow occurs, increase to 6144 and check
    // uxTaskGetStackHighWaterMark() to confirm headroom.
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 1, nullptr, 0);
    s_soundOk = true;
    Serial.println("[sound] OK");
}

static void post(SoundEvent e) {
    if (s_soundOk) xQueueSend(s_queue, &e, 0);
}

void sound_flap()        { post(SND_FLAP); }
void sound_score()       { post(SND_SCORE); }
void sound_die()         { post(SND_DIE); }
void sound_menu_tap()    { post(SND_MENU_TAP); }
void sound_music_start() { post(SND_MUSIC_START); }
void sound_music_stop()  { post(SND_MUSIC_STOP); }
