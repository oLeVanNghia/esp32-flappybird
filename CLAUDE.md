# Flabby Bird — ESP32-S3 Flappy Bird Clone

## Hardware

| Peripheral | Details |
|---|---|
| MCU | ESP32-S3 N16R8 (16 MB Octal flash, 8 MB OPI PSRAM) |
| Display | 2.8" ILI9341 320×240, SPI — CS=10, DC=46, MOSI=11, SCLK=12, MISO=13, BL=45 |
| Display RST | Shared with EN — software reset disabled (`TFT_RST=-1`) |
| Touch | FT6336G I2C — SDA=16, SCL=15, RST=18, INT=17, addr=0x38 |
| Microphone | Onboard MEMS I2S — BCLK=5, WS=7, DATA=6 |

All pin constants live in `src/config.h`. Do not scatter pin numbers elsewhere.

## Build

```bash
pio run --target upload   # build + flash
pio device monitor        # serial at 115200
```

Platform: PlatformIO / Arduino framework (`espressif32`).
TFT_eSPI is configured **entirely via `platformio.ini` build flags** — do not add a `User_Setup.h`.

## Architecture

- **Double-buffer rendering** — a `TFT_eSprite` (320×240, 16-bit) is allocated in PSRAM and `pushSprite()`-d to the display every frame (~30 FPS). All drawing goes to the sprite, never directly to `tft`.
- **Game loop** runs on Core 1 (Arduino default). Target frame time = 33 ms.
- **Mic task** (`mic.cpp`) runs on Core 0 via `xTaskCreatePinnedToCore`. It sets a `volatile bool` flag when a clap is detected; the game loop reads it with `mic_clap_ready()`.
- **Touch** is polled (not interrupt-driven). `touch_was_pressed()` returns `true` once per finger-down rising edge.
- **High score** persisted in NVS via the `Preferences` library (namespace `"flappy"`, key `"hi"`).

## Key constants (src/config.h)

| Constant | Default | Effect |
|---|---|---|
| `MIC_THRESHOLD` | 6000 | Peak amplitude to trigger clap; lower = more sensitive |
| `MIC_COOLDOWN_MS` | 300 | Minimum ms between clap events |
| `PIPE_GAP` | 80 | Vertical gap between pipes (px) |
| `PIPE_SPACING` | 175 | Horizontal distance between consecutive pipes (px) |
| `GRAVITY` | 0.35 | Downward acceleration (px/frame²) |
| `FLAP_VEL` | -7.5 | Upward velocity on flap (px/frame) |
| `INITIAL_SPEED` | 2.0 | Starting pipe scroll speed (px/frame) |
| `SPEED_INC` | 0.4 | Speed added every `SPEED_STEP` (10) pipes |

## File map

```
platformio.ini   — board, flash/PSRAM, TFT_eSPI build flags
src/
  config.h       — all pins, game constants, compile-time C565() colour macros
  touch.h/.cpp   — FT6336G I2C driver (rising-edge press only)
  mic.h/.cpp     — I2S MEMS driver + FreeRTOS clap-detection task
  main.cpp       — game state machine, physics, all drawing routines
```

## Colour macros

Use the compile-time `C565(r, g, b)` macro defined in `config.h` for new colours.
Named colour constants (prefixed `CLR_`) are also defined there.
