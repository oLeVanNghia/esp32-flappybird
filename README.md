# Flabby Bird

A Flappy Bird clone for the ESP32-S3 with a 2.8" ILI9341 touchscreen display. Flap by tapping the screen or clapping near the onboard MEMS microphone. Features chiptune background music and sound effects via the onboard I2S amplifier.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue) ![Framework](https://img.shields.io/badge/framework-Arduino-teal) ![Build](https://img.shields.io/badge/build-PlatformIO-orange)

## Hardware

| Peripheral | Part | Connection |
|---|---|---|
| MCU | ESP32-S3 N16R8 | 16 MB Octal flash, 8 MB OPI PSRAM |
| Display | 2.8" ILI9341 320×240 | SPI — CS=10, DC=46, MOSI=11, SCLK=12, MISO=13, BL=45 |
| Touch | FT6336G | I2C — SDA=16, SCL=15, RST=18, INT=17, addr=0x38 |
| Microphone | Onboard MEMS | I2S — BCLK=5, WS=7, DATA=8 |
| Speaker amp | Onboard I2S amp | I2S — DOUT=6, AMP_EN=1 (active LOW) |

> Display RST is shared with the ESP32-S3 EN pin — software reset is disabled.

## Controls

- **Tap** anywhere on the touchscreen to flap
- **Clap** near the board to flap via the MEMS microphone

## Features

- Double-buffered rendering via `TFT_eSprite` (320×240, 16-bit) pushed from PSRAM — smooth ~30 FPS
- Clap detection running on Core 0 (FreeRTOS task), game loop on Core 1
- High score persisted across reboots using NVS (`Preferences` library)
- Increasing difficulty — pipes speed up every 20 scored pipes
- Chiptune background music (C major loop) during gameplay via onboard I2S amplifier
- Square-wave sound effects: flap chirp, score blip, death sweep, menu tap

## Build & Flash

Requires [PlatformIO](https://platformio.org/).

```bash
# Build and flash
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor
```

## Project Structure

```
platformio.ini   — board config, flash/PSRAM settings, build flags
src/
  config.h       — all pin definitions, game constants, colour macros
  touch.h/.cpp   — FT6336G I2C driver (rising-edge press detection)
  mic.h/.cpp     — I2S MEMS driver + FreeRTOS clap-detection task (full-duplex)
  sound.h/.cpp   — square-wave audio engine: effects + chiptune music (Core 0)
  main.cpp       — game state machine, physics, rendering
```

## Tunable Constants (`src/config.h`)

| Constant | Default | Description |
|---|---|---|
| `MIC_THRESHOLD` | 400000 | Amplitude to trigger clap (~5% full scale); increase to reduce false triggers |
| `MIC_COOLDOWN_MS` | 300 | Minimum ms between clap events |
| `PIPE_GAP` | 80 | Vertical gap between pipes (px) |
| `PIPE_SPACING` | 175 | Horizontal distance between pipes (px) |
| `GRAVITY` | 0.6 | Downward acceleration (px/frame²) |
| `FLAP_VEL` | -5.5 | Upward velocity on flap (px/frame) |
| `INITIAL_SPEED` | 2.0 | Starting pipe scroll speed (px/frame) |
| `SPEED_INC` | 0.6 | Speed added every `SPEED_STEP` (20) pipes |
