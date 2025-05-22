# ESP32-S3 SoundFont (SF2) Sampler Synthesizer

An SF2 (SoundFont 2) based synth designed specifically for the ESP32-S3 microcontroller. This project leverages the enhanced memory capabilities of the ESP32-S3 (with PSRAM) to efficiently load and play SoundFont samples, providing a compact and powerful sampler solution.

---

## Overview

The ESP32-S3 SF2 Sampler is a sampler firmware that runs exclusively on the ESP32-S3 variant due to its improved PSRAM and memory management compared to the original ESP32. It supports external DACs like the PCM5102 for high-quality audio output and uses the built-in USB hardware of the ESP32-S3 to function as a USB MIDI device.

---

## Features

- **SF2 playback**: Load SoundFont2 banks up to PSRAM size.
- **Filesystem**: Runtime switch between LittleFS and fast 4-bit SD_MMC.
- **USB MIDI**: Plug-and-play MIDI device support.
- **Per-voice filters**: Optional biquad LPF (configurable in `config.h`).
- **Per-channel filters**: Optional CC#74/71-controlled LPF/resonance.
- **Effects**: Reverb (CC#91), Chorus (CC#93), Delay (CC#95).
- **MIDI control**: GM-compatible CCs, PC, RPNs, drums on ch.10, GM reset.
- **External DAC**: Works with PCM5102 and similar I2S DACs.
- **ESP32-S3 optimized**: Dual-core, PSRAM, minimal wiring.

---

## Hardware Requirements

- **ESP32-S3 microcontroller** with PSRAM (OPI PSRAM recommended)
- **External DAC** (e.g., PCM5102)
- USB connection for MIDI and power

---

## Pin Connections

| Signal | GPIO Pin |
|--------|----------|
| BCK    | GPIO5    |
| DTA    | GPIO6    |
| WCK    | GPIO7    |

---

## Software Setup

### Arduino IDE Configuration

To build and upload this project using Arduino IDE, configure the following settings:

- **PSRAM**: Select **OPI PSRAM**
- **Partition Scheme**: Choose a partition with the most available SPDIFF space
- **USB Mode**: Select **TINY USB**
- **Core Debug Level**: Set to **Info** or lower (higher debug levels may block USB functionality)

---

## Usage

1. Connect your ESP32-S3 board with PSRAM and external DAC according to the pinout above.
2. Load your preferred SF2 SoundFont files onto the device (refer to project documentation for details on loading SF2 files).
3. Connect the ESP32-S3 via USB to your computer or MIDI host.
4. The device will enumerate as a USB MIDI device, allowing you to play samples via MIDI input.

---

## Notes

- This project is **only compatible with the ESP32-S3** due to memory and PSRAM requirements.
- Using a core debug level above **Info** may interfere with USB MIDI functionality.
- Ensure your external DAC is properly powered and connected for optimal audio quality.

---

## Contributing

Contributions, issues, and feature requests are welcome! Feel free to open a pull request or issue on the GitHub repository.

---

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

---

## Links

- [Project Repository](https://github.com/copych/SF2_Sampler)
- [ESP32-S3 Documentation](https://www.espressif.com/en/products/socs/esp32-s3)
- [SoundFont 2 Specification](https://en.wikipedia.org/wiki/SoundFont)

---

Enjoy your ESP32-S3 powered sampler!

