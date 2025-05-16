/*
* ESP32-S3 SF2 Sampler
* An SF2 (SoundFont 2) based synthesizer designed specifically for the ESP32-S3 microcontroller.
* This project leverages the enhanced memory capabilities of the ESP32-S3 (with PSRAM)
* to efficiently load and play SoundFont samples, providing a compact and powerful sampler solution.
* The ESP32-S3 SF2 Sampler is a sampler firmware that runs exclusively on the ESP32-S3 variant
* due to its improved PSRAM and memory management compared to the original ESP32. 
* It supports external DACs like the PCM5102 for high-quality audio output and 
* uses the built-in USB hardware of the ESP32-S3 to function as a USB MIDI device.
* 
* Libraries used:
* Arduino MIDI library https://github.com/FortySevenEffects/arduino_midi_library
* Optional. Using RGB LEDs requires FastLED library https://github.com/FastLED/FastLED
*
* (c) Copych 2025, License: MIT https://github.com/copych/SF2_Sampler?tab=MIT-1-ov-file#readme
* 
* More info:
* https://github.com/copych/SF2_Sampler
*/

#pragma GCC optimize ("O2")

static const char* TAG = "Main";


#define FORMAT_LITTLEFS_IF_FAILED

#include <Arduino.h>
#include <float.h>
#include "config.h"

#include <FS.h>
#include "SD_MMC.h"
#include <LittleFS.h>
#include <MIDI.h>
#include "synth.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "voice.h"
#include "button.h"
#include "fx_chorus.h"
#include "fx_reverb.h"
#include "fx_delay.h"

#include "esp_log.h"
#ifdef ENABLE_RGB_LED
    #include "rgb_led.h"
#endif
#include "src/usbmidi/src/USB-MIDI.h" 
#include "i2s_in_out.h" 

// tasks for Core0 and Core1
TaskHandle_t Task1;
TaskHandle_t Task2;

int Voice::usage;
 

// ========================== MIDI Instance ===============================================================================================
// MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
USBMIDI_CREATE_INSTANCE(0, MIDI); 

// ========================== Global devices ===============================================================================================
I2S_Audio   DRAM_ATTR   AudioPort;
SF2Parser   DRAM_ATTR   parser(SF2_PATH);
Synth       DRAM_ATTR   synth(parser);
FxChorus    DRAM_ATTR   chorus;
FxReverb    DRAM_ATTR   reverb;
FxDelay     DRAM_ATTR   delayfx;

// ========================== Button ===============================================================================================
MuxButton myButton;
uint8_t SW; // a variable that will receive a reading result for latter processing
void myBtnHandler(int id, MuxButton::btnEvents evt) { // a function that will receive an ID of a fired button, and the event
  static bool sysSD = true;
  switch (evt){
    case MuxButton::EVENT_CLICK:
        ESP_LOGI("button", "CLICK");
        synth.loadNextSf2();
        break;
    case MuxButton::EVENT_LONGPRESS: 
        ESP_LOGI("button", "LONG PRESS");
        if (sysSD) { 
            synth.setFileSystem(FileSystemType::SD);       // Use SD card
            ESP_LOGI(TAG, "\nUse SD card");
        } else {
            synth.setFileSystem(FileSystemType::LITTLEFS); // Use internal flash
            ESP_LOGI(TAG, "\nUse LITTLEFS");
        }
        synth.scanSf2Files();
        sysSD = !sysSD;
        break;
  }
}

// ========================== Misc ===============================================================================================


float pitchBendRatio(int value, float range = 2.0f) {
    return pow(2.0f, (range * (value - PITCH_BEND_CENTER) * DIV_8192) * DIV_12 );
}

// ========================== MIDI handlers ===============================================================================================
void handleNoteOn(byte ch, byte note, byte vel) {
#ifdef ENABLE_RGB_LED
    triggerLedFlash();
#endif
    //synth.printState();
    synth.noteOn(ch-1, note, vel);
}

void handleNoteOff(byte ch, byte note, byte vel) {
    synth.noteOff(ch-1, note);
}

void handlePitchBend(byte ch, int bend) {
    synth.pitchBend(ch-1, bend);
}

void handleControlChange(byte ch, byte control, byte value) {
    synth.controlChange(ch-1, control, value);
}

void handleProgramChange(uint8_t ch, uint8_t program) {
    ESP_LOGI("MIDI", "Program change on channel 0%u → program %u", ch-1, program);
    synth.programChange(ch-1, program);
}

void handleSystemExclusive( uint8_t* data, size_t len) {
        synth.handleSysEx( data,  len); 
}

// ========================== Core 0 Task ===============================================================================================
 
// Core0 task -- AUDIO
static void IRAM_ATTR audio_task1(void *userData) {
    vTaskDelay(20); 
    ESP_LOGI(TAG, "Starting Task1");

    alignas(16) float blockL[DMA_BUFFER_LEN];
    alignas(16) float blockR[DMA_BUFFER_LEN];

    while (true) {
        synth.renderLRBlock(blockL, blockR);

        // Optional: limit output per sample
        for (size_t i = 0; i < DMA_BUFFER_LEN; ++i) {
            blockL[i] = saturate_cubic(blockL[i]);
            blockR[i] = saturate_cubic(blockR[i]);
        }

        AudioPort.writeBuffers(blockL, blockR);
    }
}


// ========================== Core 1 Task ===============================================================================================
static void IRAM_ATTR audio_task2(void *userData) { 
    vTaskDelay(20);
    ESP_LOGI(TAG, "Starting Task2");
    
    while (true) { 
        MIDI.read();
        synth.updateScores();
    #ifdef ENABLE_RGB_LED
        updateLed();
    #endif
        vTaskDelay(1);
        taskYIELD();
        
        SW = digitalRead(0); // read GPIO0
        myButton.process();
    }
}

// ========================== SETUP ===============================================================================================
void setup() {
    if (!psramFound()) {
      ESP_LOGE(TAG, "PSRAM not found!");
      vTaskDelay(10);
      while(true);
    }
    btStop(); 

  // Change USB Device Descriptor Parameter
    
    USB.VID(0x1209);
    USB.PID(0x1304);
    USB.productName("S3 SF2 sampler");
    USB.manufacturerName("copych");
    //USB.serialNumber("0000");
    //USB.firmwareVersion(0x0000);
    USB.usbVersion(0x0200);
    //USB.usbClass(0x01);       // Audio (MIDI falls under class 0x01)
    //USB.usbSubClass(0x03);    // MIDI Streaming subclass
    //USB.usbProtocol(0x00);
    
    USB.usbClass(TUSB_CLASS_AUDIO);
    USB.usbSubClass(0x00);
    USB.usbProtocol(0x00);
    
    USB.usbAttributes(0x80);

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.setHandleNoteOn(handleNoteOn);
    MIDI.setHandleNoteOff(handleNoteOff);
    MIDI.setHandlePitchBend(handlePitchBend);
    MIDI.setHandleControlChange(handleControlChange);    
    MIDI.setHandleProgramChange(handleProgramChange);
    MIDI.setHandleSystemExclusive(handleSystemExclusive);

    delay(800);
    ESP_LOGI(TAG, "MIDI started");

  //  SDMMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3)
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3);
    if (!SD_MMC.begin()) {
        ESP_LOGE(TAG, "SD init failed");
    } else {
        ESP_LOGE(TAG, "SD initialized");
    }

    if (!LittleFS.begin()) {
        ESP_LOGE(TAG, "LittleFS init failed");
    } else {
        ESP_LOGE(TAG, "LittleFS initialized");
    }

    reverb.init();
    delayfx.Init();
    synth.begin();

#ifdef ENABLE_RGB_LED
    setupLed();
    ESP_LOGI(TAG, "RGB LED started");
#endif

    AudioPort.init(I2S_Audio::MODE_IN_OUT);
    ESP_LOGI(TAG, "I2S Audio port started");

    ESP_LOGI(TAG, "Reverb started");

    pinMode(0, SIG_INPUT_MODE);
    myButton.bind(0, &SW, myBtnHandler);

    xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 5000, NULL, 5, &Task1, 0 );
    xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 5000, NULL, 5, &Task2, 1 );
    vTaskDelay(30);
    ESP_LOGI(TAG, "SF2 Synth ready");
}


// ====================== LOOP ================= KILL IT OR NOT =========================================================
void loop() {
    /*
    for (int i = 0; i < MAX_VOICES; i++) {
        handleNoteOn(0, 32 + i * 2, 90);
        delay(500);        
    }
    delay(10000);
        synth.printState();
    */
     vTaskDelete(NULL);
}
