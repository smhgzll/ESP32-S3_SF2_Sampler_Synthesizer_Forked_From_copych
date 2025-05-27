/*
 * ----------------------------------------------------------------------------
 * ESP32-S3 SF2 Synthesizer Firmware
 * 
 * Description:
 *   Real-time SF2 (SoundFont) compatible wavetable synthesizer with USB MIDI, I2S audio,
 *   multi-layer voice allocation, per-channel filters, reverb, chorus and delay.
 *   GM/GS/XG support is partly implemented
 * 
 * Hardware:
 *   - ESP32-S3 with PSRAM
 *   - I2S DAC output (44100Hz stereo, 16-bit PCM)
 *   - USB MIDI input
 *   - Optional SD card and/or LittleFS
 * 
 * Author: Evgeny Aslovskiy AKA Copych
 * License: MIT
 * Repository: https://github.com/copych/ESP32-S3_SF2_Sampler_Synthesizer
 * 
 * File: rgb_led.cpp
 * Purpose: RGB LED indication control
 * ----------------------------------------------------------------------------
 */
 #pragma once
#include <FastLED.h>

#define LED_PIN     48
#define LED_COUNT   1
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define BRIGHTNESS  50

CRGB leds[LED_COUNT];
bool ledFlash = false;

void setupLed() {
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, LED_COUNT);
    FastLED.setBrightness(BRIGHTNESS);
    leds[0] = CRGB(0, 0, 20); // тёмно-синий idle
    FastLED.show();
}

void triggerLedFlash() {
    ledFlash = true;
}

void updateLed() {
    if (ledFlash) {
        leds[0] = CRGB::White;
        ledFlash = false;
    } else {
        leds[0] = CRGB(0, 0, 20); // возврат в idle
    }
    FastLED.show();
}