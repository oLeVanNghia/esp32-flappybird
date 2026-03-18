#include "mic.h"
#include "config.h"
#include <Arduino.h>

// USE_NEW_I2S_API is defined in config.h based on ESP-IDF version.
#ifdef USE_NEW_I2S_API
#  include <driver/i2s_std.h>
static i2s_chan_handle_t s_rx_chan = nullptr;
static i2s_chan_handle_t s_tx_chan = nullptr;   // full-duplex TX — enabled by sound_init()
#else
#  include <driver/i2s.h>
#endif

static volatile bool     s_clapReady  = false;
static volatile uint32_t s_lastClapMs = 0;
static bool              s_micOk      = false;  // set to true only if init succeeded

// ── Background task (Core 0) ──────────────────────────────────────────────────
static void mic_task(void* /*param*/) {
    static int32_t buf[64];

    while (true) {
        size_t bytesRead = 0;

#ifdef USE_NEW_I2S_API
        i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytesRead, pdMS_TO_TICKS(100));
#else
        i2s_read(I2S_NUM_0, buf, sizeof(buf), &bytesRead, portMAX_DELAY);
#endif

        const int samples = (int)(bytesRead / sizeof(int32_t));
        int32_t peak = 0;
        for (int i = 0; i < samples; i++) {
            // MEMS mic: 18/24-bit sample packed into the MSBs of a 32-bit word.
            // Arithmetic-shift right 8 to get a signed ±2²³ value.
            int32_t s = buf[i] >> 8;
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }

        if (peak > MIC_THRESHOLD) {
            uint32_t now = millis();
            if (now - s_lastClapMs > MIC_COOLDOWN_MS) {
                s_lastClapMs = now;
                s_clapReady  = true;
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void mic_init() {
    esp_err_t err;

#ifdef USE_NEW_I2S_API
    // ── IDF 5.x new-style I2S (full-duplex) ──────────────────────────────────
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Allocate both TX and RX handles in one call — the only i2s_new_channel() call.
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        Serial.printf("[mic] i2s_new_channel failed: %s\n", esp_err_to_name(err));
        return;
    }

    // ── RX channel (microphone) ───────────────────────────────────────────────
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PIN_I2S_MCLK,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)PIN_I2S_DATA,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &rx_cfg);
    if (err != ESP_OK) {
        Serial.printf("[mic] RX init failed: %s\n", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan); s_rx_chan = nullptr;
        i2s_del_channel(s_tx_chan); s_tx_chan = nullptr;
        return;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        Serial.printf("[mic] RX enable failed: %s\n", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan); s_rx_chan = nullptr;
        i2s_del_channel(s_tx_chan); s_tx_chan = nullptr;
        return;
    }

    // ── TX channel (speaker) — init but do NOT enable yet ────────────────────
    // sound_init() calls mic_enable_tx_chan() when the sound task is ready,
    // preventing DMA noise before any audio data is written.
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
        // RX is still running; only discard the TX handle
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        // Continue — mic still works without sound output
    }

#else
    // ── IDF 4.x legacy I2S (full-duplex) ────────────────────────────────────
    const i2s_config_t i2s_cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate          = 16000,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 4,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,   // silence TX DMA when not writing
        .fixed_mclk           = 0,
        .mclk_multiple        = I2S_MCLK_MULTIPLE_DEFAULT,
        .bits_per_chan         = I2S_BITS_PER_CHAN_DEFAULT,
    };
    const i2s_pin_config_t pin_cfg = {
        .mck_io_num   = PIN_I2S_MCLK,
        .bck_io_num   = PIN_I2S_BCLK,
        .ws_io_num    = PIN_I2S_WS,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num  = PIN_I2S_DATA,
    };

    err = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[mic] i2s_driver_install failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = i2s_set_pin(I2S_NUM_0, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[mic] i2s_set_pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_NUM_0);
        return;
    }
#endif

    s_micOk = true;
    // 4 KB stack — i2s_read/i2s_channel_read uses ~1.5 KB internally
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, nullptr, 1, nullptr, 0);
    Serial.println("[mic] OK");
}

bool mic_clap_ready() {
    if (s_micOk && s_clapReady) {
        s_clapReady = false;
        return true;
    }
    return false;
}

#ifdef USE_NEW_I2S_API
i2s_chan_handle_t mic_get_tx_chan()  { return s_tx_chan; }
void mic_enable_tx_chan() {
    if (s_tx_chan) i2s_channel_enable(s_tx_chan);
}
#endif
