#pragma GCC optimize ("O2")

static const char* TAG = "Main";

#define FORMAT_LITTLEFS_IF_FAILED

#include <Arduino.h>
#include <float.h>

#include "config.h"
#include "synth.h"
#include <MIDI.h>
#include "SF2Parser.h"
#include "adsr.h"
#include "voice.h"
#include "fx_chorus.h"
#include "fx_reverb.h"

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


//static const char* SF2_PATH = "/example.sf2";
// static const char* SF2_PATH = "/piano.sf2";
// static const char* SF2_PATH = "/2MBGMGS.SF2";
static const char* SF2_PATH = "/YAMAHA1.SF2";

// MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
USBMIDI_CREATE_INSTANCE(0, MIDI); 

// Global devices
I2S_Audio   DRAM_ATTR   AudioPort;
SF2Parser   DRAM_ATTR   parser(SF2_PATH);
Synth       DRAM_ATTR   synth(parser);
FxChorus    DRAM_ATTR   chorus;
FxReverb    DRAM_ATTR   reverb;

inline float limited(float val) {
    if (val < -1.0f) return -1.0f;
    if (val > 1.0f) return 1.0f;
    return val;
}

float pitchBendRatio(int value, float range = 2.0f) {
    return pow(2.0f, (range * (value - PITCH_BEND_CENTER) / 8192.0f) / 12.0f);
}

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
    ESP_LOGI("MIDI", "Program change on channel %u → program %u", ch, program);
    synth.programChange(ch-1, program);
}

void handleSystemExclusive( uint8_t* data, size_t len) {
        synth.handleSysEx( data,  len); 
}

/* 
 * Core Tasks ************************************************************************************************************************
*/
 
// Core0 task -- AUDIO
static void IRAM_ATTR audio_task1(void *userData) {
    vTaskDelay(20); 
    ESP_LOGI(TAG, "Starting Task1");

    float WORD_ALIGNED_ATTR mixL = 0.0f;
    float WORD_ALIGNED_ATTR mixR = 0.0f;

    while (true) {        
        synth.renderLR(&mixL, &mixR);
   //     chorus.process(&mixL, &mixR);
        reverb.process(&mixL, &mixR);
//      Delay.Process(&mixL, &mixR);
        mixL = limited(mixL);
        mixR = limited(mixR);
        AudioPort.putSamples(mixL, mixR);
    }
}

// task for Core1, which tipically runs user's code on ESP32
// static void IRAM_ATTR audio_task2(void *userData) {
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
    }
}

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
    

    LittleFS.begin();

    if (!parser.parse()) {
        ESP_LOGE(TAG, "SF2 load failed");
        while (true);
    } else {
        ESP_LOGI(TAG, "SF2 load success"); 
        //parser.dumpPresetStructure();
    }

#ifdef ENABLE_RGB_LED
    setupLed();
    ESP_LOGI(TAG, "RGB LED started");
#endif

    AudioPort.init(I2S_Audio::MODE_IN_OUT);
    ESP_LOGI(TAG, "I2S Audio port started");

    reverb.init();
    ESP_LOGI(TAG, "Reverb started");

    xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 5000, NULL, 5, &Task1, 0 );
    xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 5000, NULL, 5, &Task2, 1 );
    vTaskDelay(30);
    ESP_LOGI(TAG, "SF2 Synth ready");
}

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
