#pragma once
#include "config.h"
/*
 * reverb stuff taken from Marcel Licence
 *
 * This is a port of the public code from YetAnotherElectronicsChannel
 * Based on main.c I've created this module
 *
 * src: https://github.com/YetAnotherElectronicsChannel/STM32_DSP_Reverb/blob/master/code/Src/main.c
 *
 * The explanation of the original module can be found here: https://youtu.be/nRLXNmLmHqM
 *
 * Changes:
 * - optimized for buffer processing
 * - added interface to set the level
 * - added dynamic buffer allocation in RAM/PSRAM
 * - cleaned and heavily refactered code
 */ 

#ifdef BOARD_HAS_PSRAM 
  #define REV_MULTIPLIER 1.8f
  #define MALLOC_CAP        MALLOC_CAP_SPIRAM
#else
  #define REV_MULTIPLIER 0.35f
  #define MALLOC_CAP        MALLOC_CAP_INTERNAL
#endif
 

constexpr int NUM_COMBS = 4;
constexpr int NUM_ALLPASSES = 3;

const float comb_lengths[NUM_COMBS] = {3604.0f, 3112.0f, 4044.0f, 4492.0f};
const float comb_gains[NUM_COMBS]   = {0.805f, 0.827f, 0.783f, 0.764f};

const float allpass_lengths[NUM_ALLPASSES] = {500.0f, 168.0f, 48.0f};
const float allpass_gains[NUM_ALLPASSES]   = {0.7f, 0.7f, 0.7f};

class FxReverb {
public:
  FxReverb() {}

  inline void init() {
    for (int i = 0; i < NUM_COMBS; ++i) {
      int size = int(comb_lengths[i] * REV_MULTIPLIER);
      combBuf[i] = (float*)heap_caps_aligned_alloc(4, sizeof(float) * size, MALLOC_CAP);
      combSize[i] = size;
      combPtr[i] = 0;

      if (!combBuf[i]) {
        ESP_LOGE("Reverb", "No memory for combBuf[%d]", i);
      } else {
        memset(combBuf[i], 0, sizeof(float) * size);
        ESP_LOGI("Reverb", "combBuf[%d] allocated: %d bytes", i, size * sizeof(float));
      }
    }

    for (int i = 0; i < NUM_ALLPASSES; ++i) {
      int size = int(allpass_lengths[i] * REV_MULTIPLIER);
      allpassBuf[i] = (float*)heap_caps_aligned_alloc(4, sizeof(float) * size, MALLOC_CAP);
      allpassSize[i] = size;
      allpassPtr[i] = 0;

      if (!allpassBuf[i]) {
        ESP_LOGE("Reverb", "No memory for allpassBuf[%d]", i);
      } else {
        memset(allpassBuf[i], 0, sizeof(float) * size);
        ESP_LOGI("Reverb", "allpassBuf[%d] allocated: %d bytes", i, size * sizeof(float));
      }
    }

    SetLevel(0.1f);
    SetTime(0.5f);
  }

  inline void SetTime(float value) {
    rev_time = 0.92f * value + 0.02f;
    for (int i = 0; i < NUM_COMBS; ++i)
      combLim[i] = int(rev_time * combSize[i]);
    for (int i = 0; i < NUM_ALLPASSES; ++i)
      allpassLim[i] = int(rev_time * allpassSize[i]);

    ESP_LOGI("Reverb", "Reverb time set to %0.3f", value);
  }

  inline void SetLevel(float value) {
    rev_level = value;
    ESP_LOGI("Reverb", "Reverb level set to %0.3f", value);
  }

  inline void process(float* signal_l, float* signal_r) {
    float inSample = 0.125f * (*signal_l + *signal_r);

    float sum = 0.0f;
    for (int i = 0; i < NUM_COMBS; ++i)
      sum += doComb(i, inSample);
    float outSample = sum / NUM_COMBS;

    for (int i = 0; i < NUM_ALLPASSES; ++i)
      outSample = doAllpass(i, outSample);

    outSample *= rev_level;

    *signal_l += outSample;
    *signal_r += outSample;
  }

private:
  float rev_time = 0.5f;
  float rev_level = 0.5f;

  float* combBuf[NUM_COMBS] = {};
  int combSize[NUM_COMBS] = {};
  int combPtr[NUM_COMBS] = {};
  int combLim[NUM_COMBS] = {};

  float* allpassBuf[NUM_ALLPASSES] = {};
  int allpassSize[NUM_ALLPASSES] = {};
  int allpassPtr[NUM_ALLPASSES] = {};
  int allpassLim[NUM_ALLPASSES] = {};

  inline float doComb(int idx, float in) {
    int& p = combPtr[idx];
    float g = comb_gains[idx];
    float* buf = combBuf[idx];
    float out = buf[p];
    buf[p] = out * g + in;
    p = (p + 1 >= combLim[idx]) ? 0 : p + 1;
    return out;
  }

  inline float doAllpass(int idx, float in) {
    int& p = allpassPtr[idx];
    float g = allpass_gains[idx];
    float* buf = allpassBuf[idx];
    float out = buf[p];
    float v = out * g + in;
    buf[p] = v;
    out = out - g * in;
    p = (p + 1 >= allpassLim[idx]) ? 0 : p + 1;
    return out;
  }
};

