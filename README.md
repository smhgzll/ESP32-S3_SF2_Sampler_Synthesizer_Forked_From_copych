# ESP32S3 SF2 Sampler
ESP32-S3 SF2 based sampler 
Due to slow PSRAM and different memory management, this sampler only runs on an S3 version of ESP32.
You only need a PSRAM-equipped ESP32-S3 and an external DAC (like PCM5102) to build this project. 

Connections:
- BCK GPIO5
- DTA GPIO6
- WCK GPIO7

By default it is configured as a USB MIDI DEVICE.

In ArduinoIDE tools:

- PSRAM: OPI PSRAM

- Partition: select one with the most available SPDIFF

- USB MODE: TINY USB

note that core debug level higher than info will probably block USB functionality. 
