#pragma once

// ===================== AUDIO ======================================================================================
#define   DMA_BUFFER_NUM        2     // number of internal DMA buffers
#define   DMA_BUFFER_LEN        32    // length of each buffer in samples
#define   CHANNEL_SAMPLE_BYTES  2     // can be 1, 2, 3 or 4 (2 and 4 only supported yet)
#define   SAMPLE_RATE           44100

// ===================== SYNTHESIZER ===============================================================================
#define MAX_VOICES 16
#define MAX_VOICES_PER_NOTE 2
#define PITCH_BEND_CENTER 8192
static const char* SF2_PATH = "/"; 

// ===================== I2S PINS ======================================================================================
#define I2S_BCLK_PIN    5       // I2S BIT CLOCK pin (BCL BCK CLK)
#define I2S_DOUT_PIN    6       // MCU Data Out: connect to periph. DATA IN (DIN D DAT)
#define I2S_WCLK_PIN    7       // I2S WORD CLOCK pin (WCK WCL LCK)
#define I2S_DIN_PIN     15      // MCU Data In: connect to periph. DATA OUT (DOUT D SD)

// ===================== SD MMC PINS ================================================================================
// ESP32S3 allows almost any GPIOs for any particular needs
#define SDMMC_CMD 38
#define SDMMC_CLK 39
#define SDMMC_D0  10
#define SDMMC_D1  11
#define SDMMC_D2  12
#define SDMMC_D3  13

// ===================== OLED DISPLAY PINS ==========================================================================
#define DISPLAY_SDA 8 // SDA GPIO
#define DISPLAY_SCL 9 // SCL GPIO

// ===================== OLED DISPLAY CONFIG ========================================================================
#define DISPLAY_CONTROLLER SH1106
// #define DISPLAY_CONTROLLER SSD1306

#define DISPLAY_W 128
#define DISPLAY_H 64
#define DISPLAY_ROTATE 0 // can be 0, 90, 180 or 270

// ===================== RGB LED ====================================================================================
// #define ENABLE_RGB_LED
// #define FASTLED_INTERNAL                  // remove annoying pragma messages










// !!!!!!!!!!!!!=======  DO NOT CHANGE  =======!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// U8G2 CONSTRUCTOR MACROS
#if (DISPLAY_ROTATE==180)
  #define U8_ROTATE U8G2_R2
#elseif (DISPLAY_ROTATE==90)
  #define U8_ROTATE U8G2_R1
#elseif (DISPLAY_ROTATE==270)
  #define U8_ROTATE U8G2_R3
#else
  #define U8_ROTATE U8G2_R0
#endif

#define W_H_DIV X
#define _U8_CONCAT(ctrl, w, div, h) U8G2_ ## ctrl ## _ ## w ## div ## h ## _NONAME_F_HW_I2C
#define U8_CONCAT(ctrl, w, div, h) _U8_CONCAT(ctrl, w, div, h)
#define U8_OBJECT U8_CONCAT(DISPLAY_CONTROLLER, DISPLAY_W, W_H_DIV, DISPLAY_H)