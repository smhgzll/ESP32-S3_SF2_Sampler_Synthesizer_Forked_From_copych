# ESP32-S3 SF2 Sampler Synthesizer (Fork)

This is a **fork** of the original project by **Evgeny Aslovskiy (Copych)**:  
> *ESP32-S3 SF2 Sampler Synthesizer* – MIT licensed.  
All credit for the design, structure and most of the code goes to the original author.  
This fork focuses on **SdFat-based storage**, **PlatformIO** build flow, and several **real-time / stability fixes** for audio and tasks on ESP32-S3.

> **License:** MIT (same as upstream).  
> **Upstream repository:** `https://github.com/copych/ESP32-S3_SF2_Sampler_Synthesizer` (refer there for history and original docs/features).

---

## What’s new in this fork

- **Storage:** Switched to **SdFat** for SD access. Works alongside **LittleFS** (Flash).
- **PlatformIO**: Project migrated to PlatformIO with a reproducible environment.
- **I2S DMA safety:** Output buffer is allocated from **internal DMA-capable RAM** (uses `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).  
  This removes the “_Couldn't allocate memory for _output_buf_” class of failures in common setups.
- **Audio stability:** Added periodic yields in RT tasks, safe circular buffering, and safer chorus interpolation (wrap/clip).  
  These changes reduce the chance of **Task Watchdog** resets under heavy load.
- **Config clean-up:** Centralized parameters in `config.h` (DMA sizes, pins, features), with sane defaults for ESP32-S3 + PCM5102A.

---

## Overview

An **SF2 (SoundFont 2)** compatible wavetable synth for **ESP32-S3** with:
- **USB-MIDI** device mode
- **I2S** stereo audio out (16-bit PCM @ 44.1 kHz by default)
- **Per-voice** and **per-channel** filters (optional)
- **Effects:** Reverb, Chorus, Delay (toggleable)
- **Dual-core** scheduling tuned for real-time audio
- **LittleFS / SD** runtime switch (BOOT button behavior retained)

**BOOT button**:
- Short-press cycles through available SF2 files in the active filesystem.
- Long-press toggles **LittleFS ↔ SD** at runtime.

<img src="./media/prototype.jpg?raw=true" alt="Prototype photo" />

---

## Hardware

### Minimum
- **ESP32-S3 module/board** with **PSRAM** (OPI PSRAM recommended)
- **I2S DAC** (e.g., **PCM5102A**)
- **USB** for power + MIDI (device)

### Optional
- **microSD** card (**SdFat**; SPI wiring in this fork)
- **0.96\" OLED** (U8g2), rotary encoder + button (simple GUI)

---

## Pinout (defaults in `config.h`)

### I2S (PCM5102A)
| Signal | GPIO |
|---|---|
| BCK  | 5 |
| DOUT | 6 |
| WCK  | 7 |
| DIN  | -1 *(unused)* |

> Change in `config.h` if needed.

### SD (SPI mode, SdFat)
| Signal | GPIO |
|---|---|
| CS   | 10 |
| MOSI | 11 |
| SCK  | 12 |
| MISO | 13 |

> Change in `config.h` if needed.

### GUI / Inputs
| Item | GPIO |
|---|---|
| Button | 14 |
| Encoder A | 15 |
| Encoder B | 16 |
| OLED SDA | 8 |
| OLED SCL | 9 |

> `U8g2` is required for the optional OLED GUI. Controller defaults to `SH1106` (see `config.h`).

---

## Key Configuration (excerpt from `config.h`)

```cpp
// Audio
#define DMA_BUFFER_NUM        8      // number of DMA blocks
#define DMA_BUFFER_LEN        256    // samples per block
#define CHANNEL_SAMPLE_BYTES  2      // 16-bit PCM
#define SAMPLE_RATE           44100  // Hz

// MIDI
#define USE_USB_MIDI_DEVICE   1
#define USE_MIDI_STANDARD     2
#define MIDI_IN_DEV           USE_USB_MIDI_DEVICE
#define NUM_MIDI_CHANNELS     16

// Synth / features
#define MAX_VOICES            19
#define ENABLE_IN_VOICE_FILTERS
#define ENABLE_CHORUS
#define ENABLE_CH_FILTER_M    // mono per-channel filter before stereo split
// #define ENABLE_REVERB
// #define ENABLE_DELAY
// #define ENABLE_OVERDRIVE

// Filesystems
static const char* SF2_PATH = "/sf2";
#define DEFAULT_CONFIG_FILE "/default_config.bin"

// I2S pins (see tables above)
#define I2S_BCLK_PIN    5
#define I2S_DOUT_PIN    6
#define I2S_WCLK_PIN    7
#define I2S_DIN_PIN    -1

// SD (SPI) pins (SdFat)
#define SD_CS   10
#define SD_SCK  12
#define SD_MOSI 11
#define SD_MISO 13
```

> If you see *buffer allocation* issues: reduce `DMA_BUFFER_LEN` (e.g., 256 → 192 → 128), keep `DMA_BUFFER_NUM` ≥ 6, and ensure PSRAM is enabled (but the **I2S DMA buffer must live in internal DMA-capable RAM**; this fork enforces that).

---

## Building with PlatformIO

**`platformio.ini`** (used by this fork):

```ini
[platformio]
default_envs = s3_n16r8
src_dir = SF2Sampler

[env:s3_n16r8]
platform = espressif32@6.11.0
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
monitor_filters = esp32_exception_decoder, colorize

; 16MB Flash + OPI PSRAM
board_build.flash_mode = qio
board_upload.flash_size = 16MB
board_build.filesystem = littlefs
board_build.arduino.memory_type = qio_opi
board_build.psram_type = opi
# board_build.psram_speed = 120MHz

build_unflags = -std=gnu++11
build_flags =
  -O3
  -ffast-math
  -fno-math-errno
  -fstrict-aliasing
  -std=gnu++17
  -DBOARD_HAS_PSRAM
  -DCORE_DEBUG_LEVEL=5
  -DARDUINO_USB_MODE=0
  -DARDUINO_USB_CDC_ON_BOOT=0
  -DENABLE_TEST_TONE=0
  -DTEST_TONE_FREQ_HZ=880.0

lib_deps =
  olikraus/U8g2 @ ^2.36.12
  FortySevenEffects/MIDI Library @ ^5.0.2
  greiman/SdFat @ ^2.3.1
```

**Commands:**
```bash
# Build
pio run

# Flash & monitor (USB)
pio run -t upload
pio device monitor
```

> By default, sources live under `SF2Sampler/` (see `src_dir`).

---

## Building with Arduino IDE (optional)

- **Board:** ESP32-S3 DevKit (enable **OPI PSRAM**)
- **USB Mode:** TinyUSB (or keep CDC off at boot if you prefer)
- **Partition Scheme:** choose one with enough space for your LittleFS needs
- **Core Debug Level:** *Info* or lower (higher levels may affect USB)
- **SF2 on Flash:** use an Arduino LittleFS uploader plugin to upload SF2 files to LittleFS  
  *(or place SF2s on SD; this fork uses SdFat for SD access)*

---

## Usage

1. Wire the **PCM5102A** (or similar) and optional **SD**/OLED as per the tables above.
2. Put your `.sf2` files under `/sf2` on **LittleFS** or **SD**.
3. Connect USB: the board enumerates as a **USB-MIDI device**.
4. Play via any MIDI source. Use **BOOT** to cycle SF2s (short), or switch FS (long).
5. Optional GUI (OLED + encoder + button) provides simple navigation.

---

## Troubleshooting

- **`Couldn't allocate memory for _output_buf`**  
  Reduce `DMA_BUFFER_LEN`, keep `DMA_BUFFER_NUM` >= 6–8, ensure PSRAM is enabled **but** I2S buffer stays in **internal DMA-capable RAM** (this fork sets the proper caps). Avoid putting the I2S DMA buffer in PSRAM.

- **Watchdog resets / “stalls” after tasks start**  
  Make sure you are on this fork’s code: periodic `vTaskDelay(1)` in audio/control loops and safer FX should be present. Disable heavy FX (e.g., set chorus mix low) and confirm improvements.

- **Audio crackles under heavy polyphony**  
  Try lowering polyphony, shrinking block size, or reducing effect load. Ensure the external DAC wiring and clocks are clean.

---

## Project Structure (fork)

```
SF2Sampler/          <- sources (set by src_dir)
  ├─ config.h        <- all tunables (pins, DMA, features)
  ├─ main.cpp
  ├─ i2s_in_out.*    <- I2S path (DMA-capable buffer allocation)
  ├─ fx_chorus.h     <- safer wrap/clip + wet/dry
  ├─ ...
platformio.ini
README.md            <- this file
```

---

## Credits & Attribution

- Original author: **Evgeny Aslovskiy (Copych)** – *ESP32-S3 SF2 Sampler Synthesizer*, MIT License.  
- This fork: SdFat integration, PlatformIO build, DMA & FX stability tweaks, and documentation updates.  
- Libraries: **U8g2**, **MIDI Library**, **SdFat**, **ESP32 Arduino core**.

> This fork is **not** affiliated with the original author. Please consider starring and crediting the upstream project.

---

## Roadmap (fork)

- Optional **I2S V3** path selection & buffer auto-tuning
- More FX presets (bounded CPU modes)
- Config persistence for user parameters
- Continuous audio profiling and benchmark hooks

---

Enjoy your **ESP32-S3** powered SoundFont sampler! If you run into issues, open an issue with your **board model**, **PSRAM type**, **config.h** excerpt (DMA + pins), and a **serial log**.
