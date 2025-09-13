/*
* ESP32-S3 SF2 Sampler
* An SF2 (SoundFont 2) based synthesizer designed specifically for the ESP32-S3 microcontroller.
* This project leverages the enhanced memory capabilities of the ESP32-S3 (with PSRAM)
* to efficiently load and play SoundFont samples, providing a compact and powerful sampler solution.
* The ESP32-S3 SF2 Sampler is a sampler firmware that runs exclusively on the ESP32-S3 variant
* due to its improved PSRAM and memory management compared to the original ESP32. 
* It supports external DACs like the PCM5102 for high-quality audio output and 
* uses the built-in USB hardware of the ESP32-S3 to function as a USB MIDI device.
* GM/GS/XG support is partlially implemented (i.e. with 2MBGMGS.sf2 bank).
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
#pragma packed(16)
#pragma GCC optimize ("O3")
#pragma GCC optimize ("fast-math")
#pragma GCC optimize ("unsafe-math-optimizations")
#pragma GCC optimize ("no-math-errno")

static const char* TAG = "Main";


#define FORMAT_LITTLEFS_IF_FAILED

#include <Arduino.h>
#include "esp_task_wdt.h"
#include <float.h>
#include "config.h"
#include "misc.h" 
#include "esp_log.h"
#include <FS.h>
#include <LittleFS.h>
#include <MIDI.h>
#include "synth.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "voice.h"
#include "SynthState.h"
#include <SdFat.h>
#include "esp_pm.h"       // PM lock (CPU frekansını sabitlemek için)
#include "esp_timer.h"    // esp_timer_get_time()
#include <WiFi.h>   // Wi-Fi off için

static esp_pm_lock_handle_t s_pm_lock = nullptr;

SdFs SD;

#define SD_MMC SD

#ifdef ENABLE_RGB_LED
    #include "rgb_led.h"
#endif

#if MIDI_IN_DEV == USE_USB_MIDI_DEVICE
    #include "src/usbmidi/src/USB-MIDI.h"
#endif

// I2S örnekleme oranı. Projede farklıysa burayı aynı yap.
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE SAMPLE_RATE
#endif

#include "i2s_in_out.h" 

bool g_audio_ok = false;

// tasks for Core0 and Core1 (statik)
TaskHandle_t Task1;
TaskHandle_t Task2;
TaskHandle_t Task3;
static StaticTask_t audio_tcb;
static StaticTask_t control_tcb;
#ifdef ENABLE_GUI
static StaticTask_t gui_tcb;
#endif
// Stack'leri internal RAM'de tut (boyutları projene göre ayarla)
static StackType_t  audio_stack[8192/sizeof(StackType_t)]  DRAM_ATTR;
static StackType_t  control_stack[8192/sizeof(StackType_t)] DRAM_ATTR;
#ifdef ENABLE_GUI
static StackType_t  gui_stack[5000/sizeof(StackType_t)]    DRAM_ATTR;
#endif

int Voice::usage; // counts voices internally


// ========================== MIDI Instance ===============================================================================================
#if MIDI_IN_DEV == USE_MIDI_STANDARD
    MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
#endif

#if MIDI_IN_DEV == USE_USB_MIDI_DEVICE
    USBMIDI_CREATE_INSTANCE(0, MIDI); 
#endif

// ========================== Global devices ===============================================================================================
#ifdef ENABLE_CHORUS
    #include "fx_chorus.h"
    FxChorus    DRAM_ATTR   chorus;
#endif

#ifdef ENABLE_REVERB
    #include "fx_reverb.h"
    FxReverb    DRAM_ATTR   reverb;
#endif

#ifdef ENABLE_DELAY
    #include "fx_delay.h"
    FxDelay     DRAM_ATTR   delayfx;
#endif



I2S_Audio   AudioPort(I2S_Audio::MODE_OUT);
SF2Parser   parser(SF2_PATH);
Synth       synth(parser);

// ========================== Synth Settings ===================================================================================
SynthState state {
  synth.channels
#ifdef ENABLE_REVERB
  , reverb
#endif
#ifdef ENABLE_DELAY
  , delayfx
#endif
#ifdef ENABLE_CHORUS
  , chorus
#endif
};

// ========================== GUI ==============================================================================================
#ifdef ENABLE_GUI
    #include "TextGUI.h"
    TextGUI gui(synth, state);
#endif

static void lockCpu240() {
  // CPU frekansını maksimumda kilitle (dinamik düşmeyi engeller)
  if (!s_pm_lock) {
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "audio", &s_pm_lock);
  }
  if (s_pm_lock) {
    esp_pm_lock_acquire(s_pm_lock);
  }
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

#ifdef TASK_BENCHMARKING
// globals to be unsafely shared between tasks
    uint32_t DRAM_ATTR t0,t1,t2,t3,t4,t5,t6;
    uint32_t DRAM_ATTR dt1,dt2,dt3,dt4,dt5,dt6;
    uint32_t DRAM_ATTR total_render = 0;
    uint32_t DRAM_ATTR total_write  = 0;
#endif

    volatile uint32_t DRAM_ATTR frame_count  = 0;
    float DRAM_ATTR blockL[DMA_BUFFER_LEN];
    float DRAM_ATTR blockR[DMA_BUFFER_LEN];

// ========================== Core 0 Task 1 ===============================================================================================
// Core0 task -- AUDIO
static void IRAM_ATTR audio_task(void *userData) {
    vTaskDelay(20); 
    ESP_LOGI(TAG, "Starting Task1");

    while (true) {
        
#ifdef TASK_BENCHMARKING
        t0 = esp_cpu_get_cycle_count();
#endif

        synth.renderLRBlock(blockL, blockR);

#ifdef TASK_BENCHMARKING
        t1 = esp_cpu_get_cycle_count();
#endif

        AudioPort.writeBuffers(blockL, blockR);

#ifdef TASK_BENCHMARKING
        t2 = esp_cpu_get_cycle_count();

        total_render += (t1 - t0);
        total_write  += (t2 - t1);
#endif

        frame_count++;
    }
}

// ========================== Core 1 Task 2 ===============================================================================================
static void IRAM_ATTR control_task(void *userData) { 
    vTaskDelay(50);
    ESP_LOGI(TAG, "Starting Task2");
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1); // 1ms kontrol periyodu (gerekirse 2–5ms)
    
    for (;;) {
        for (int k = 0; k < 64 && MIDI.read(); ++k) { /* drain MIDI */ }
        // MIDI.read();
        synth.updateScores();

#ifdef ENABLE_GUI
        if (__builtin_expect((gui_blocker == 0), 1)) {
            // Read GUI input
            gui.encA = digitalRead(ENC0_A_PIN);
            gui.encB = digitalRead(ENC0_B_PIN);
            gui.btnState = digitalRead(BTN0_PIN);
            
            gui.process();
        } else {
            gui_blocker--;
            if (gui_blocker < 0) { gui_blocker = 0; }
        }
#endif
        
        if (frame_count >= 64) {

#ifdef TASK_BENCHMARKING
            uint32_t avg_render = total_render / frame_count;
            uint32_t avg_write  = total_write  / frame_count;

            ESP_LOGI(TAG, "Avg cycles over %u frames: render = %u, write = %u",
                     frame_count, avg_render, avg_write);

            total_render = 0;
            total_write  = 0;
#endif
            synth.updateActivity();
            frame_count  = 0;
        }

        vTaskDelayUntil(&last, period);
    }
}

#ifdef ENABLE_GUI
// ========================== Core 1 Task 3 ===============================================================================================
static void IRAM_ATTR gui_task(void *userData) { 
    vTaskDelay(50);
    ESP_LOGI(TAG, "Starting Task3");
    
    while (true) {
        if (gui_blocker == 0) {
            gui.draw();
        }
        taskYIELD();
    }

}
#endif

// ========================== SETUP ===============================================================================================
void setup() {
    lockCpu240();
    if (!psramFound()) {
      ESP_LOGE(TAG, "PSRAM not found!");
      vTaskDelay(10);
      while(true);
    }
    btStop(); 
    WiFi.mode(WIFI_OFF);    // RF yüke girmesin
    esp_log_level_set("*", ESP_LOG_WARN); 
    
#if MIDI_IN_DEV == USE_USB_MIDI_DEVICE
  // Change USB Device Descriptor Parameter
    USB.VID(0x1209);
    USB.PID(0x1304);
    USB.productName("S3 SF2 Synth");
    USB.manufacturerName("copych");
    USB.usbVersion(0x0200);
    USB.usbClass(TUSB_CLASS_AUDIO);
    USB.usbSubClass(0x00);
    USB.usbProtocol(0x00);
    USB.usbAttributes(0x80);
#endif

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.setHandleNoteOn(handleNoteOn);
    MIDI.setHandleNoteOff(handleNoteOff);
    MIDI.setHandlePitchBend(handlePitchBend);
    MIDI.setHandleControlChange(handleControlChange);    
    MIDI.setHandleProgramChange(handleProgramChange);
    MIDI.setHandleSystemExclusive(handleSystemExclusive);

    delay(800);
    ESP_LOGI(TAG, "MIDI started");

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    SdSpiConfig sdCfg(SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI);

    if (!SD.begin(sdCfg)) {
        ESP_LOGE(TAG, "SD init failed (SdFat)");
    } else {
        ESP_LOGI(TAG, "SD initialized (SdFat)");
    }

#ifdef ENABLE_GUI
    gui.begin();
    gui.busyMessage( "Synth Loading...");
    ESP_LOGI(TAG, "GUI splash");
#endif

#ifdef ENABLE_REVERB
    reverb.init();
    ESP_LOGI(TAG, "Reverb FX started");
#endif

#ifdef ENABLE_DELAY
    delayfx.init();
    ESP_LOGI(TAG, "Delay FX started");
#endif
 

    synth.begin();
    ESP_LOGI(TAG, "Synth is starting");

#ifdef ENABLE_GUI
    gui.startMenu();
    ESP_LOGI(TAG, "GUI started");
#endif

  AudioPort.init(I2S_Audio::MODE_OUT);
  g_audio_ok = true;
  ESP_LOGI(TAG, "I2S init: %s", g_audio_ok ? "OK" : "FAILED");

#ifdef ENABLE_RGB_LED
    setupLed();
    ESP_LOGI(TAG, "RGB LED started");
#endif

    Task1 = xTaskCreateStaticPinnedToCore(
        audio_task, "SynthTask",
        sizeof(audio_stack)/sizeof(StackType_t),
        nullptr, 11, audio_stack, &audio_tcb, 1); // Core1, prio 11
   
    Task2 = xTaskCreateStaticPinnedToCore(
        control_task, "ControlTask",
        sizeof(control_stack)/sizeof(StackType_t),
        nullptr, 6, control_stack, &control_tcb, 0); // Core0, prio 6

    // xTaskCreatePinnedToCore(audio_task,   "SynthTask",   8192, NULL, 11, &Task1, 1); // Core1
    // xTaskCreatePinnedToCore(control_task, "ControlTask", 8192, NULL,  8, &Task2, 0); // Core0
#ifdef ENABLE_GUI
    Task3 = xTaskCreateStaticPinnedToCore(
        gui_task, "GUITask",
        sizeof(gui_stack)/sizeof(StackType_t),
        nullptr, 3, gui_stack, &gui_tcb, 0); // Core0, prio 3

    // xTaskCreatePinnedToCore(gui_task,     "GUITask",     5000, NULL,  3, &Task3, 0);
#endif

    vTaskDelay(30);

    ESP_LOGI(TAG, "SF2 Synth ready");
}


// ====================== LOOP ================= KILL IT OR NOT =========================================================
void loop() {
    vTaskDelete(NULL);
}
