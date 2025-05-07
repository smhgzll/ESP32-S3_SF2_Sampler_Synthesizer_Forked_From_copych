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