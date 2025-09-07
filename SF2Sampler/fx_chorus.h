/*
* FxChorus - Chorus audio effect (fixed/optimized)
* May 2025
* Author: Evgeny Aslovskiy AKA Copych
* License: MIT
*
* - Safe circular indexing with power-of-two mask
* - Clamp delay (in samples) into [1 .. MAX_DELAY-2]
* - Wet/Dry mix (default 0.35f)
* - Lightweight LFO update cadence
*/

#pragma once
#include "config.h"
#include <math.h>
#include "esp_attr.h" // EXT_RAM_ATTR

class FxChorus {
public:
    FxChorus() {
        sampleRate = SAMPLE_RATE;
        writeIndex = 0;
        lfoPhase = 0.0f;
        updateCounter = 0;
        currentLfoValue = 0.0f;

        for (int i = 0; i < MAX_DELAY; ++i) {
            bufferL[i] = 0.0f;
            bufferR[i] = 0.0f;
        }

        setLfoFreq(0.5f);      // Hz
        setDepth(0.002f);      // seconds (±depth)
        setBaseDelay(0.03f);   // seconds
        setMix(0.35f);         // 0=dry, 1=wet
    }

    inline void __attribute__((hot,always_inline)) IRAM_ATTR processBlock(float* left, float* right) {
        for (int n = 0; n < DMA_BUFFER_LEN; ++n) {
            // LFO'yu seyrek güncelle (CPU tasarrufu)
            if (++updateCounter >= LFO_UPDATE_INTERVAL) {
                updateCounter = 0;
                lfoPhase += lfoFreq * (float)LFO_UPDATE_INTERVAL / sampleRate; // faz 0..1
                if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
                currentLfoValue = fast_sin(TWOPI * lfoPhase); // -1..+1
            }

            // Hedef gecikme: saniyeden örneğe çevir
            float delaySamplesL = (baseDelay + depth *  currentLfoValue) * sampleRate;
            float delaySamplesR = (baseDelay + depth * -currentLfoValue) * sampleRate;

            // interpolasyon için güvenli aralık [1 .. MAX_DELAY-2]
            if (delaySamplesL < 1.0f) delaySamplesL = 1.0f;
            if (delaySamplesR < 1.0f) delaySamplesR = 1.0f;
            if (delaySamplesL > (float)(MAX_DELAY - 2)) delaySamplesL = (float)(MAX_DELAY - 2);
            if (delaySamplesR > (float)(MAX_DELAY - 2)) delaySamplesR = (float)(MAX_DELAY - 2);

            // Dairesel okuma konumları
            float readIndexL = (float)writeIndex - delaySamplesL;
            float readIndexR = (float)writeIndex - delaySamplesR;

            // Interpolasyonlu gecikmiş örnekler
            float delayedL = getInterpolatedSample(bufferL, readIndexL);
            float delayedR = getInterpolatedSample(bufferR, readIndexR);

            // Girişi yaz
            const int wi = writeIndex;
            bufferL[wi] = left[n];
            bufferR[wi] = right[n];

            // Wet/Dry mix
            left[n]  = (1.0f - mix) * left[n]  + mix * delayedL;
            right[n] = (1.0f - mix) * right[n] + mix * delayedR;

            // Hızlı wrap (MAX_DELAY = 2^k)
            writeIndex = (wi + 1) & MASK;
        }
    }

    // Parametre set/get
    void  setLfoFreq(float freq)   { lfoFreq = freq; }
    void  setDepth(float d_sec)    { depth = d_sec; }          // seconds (±depth)
    void  setBaseDelay(float sec)  { baseDelay = sec; }        // seconds
    void  setMix(float m01)        { if (m01 < 0) m01 = 0; else if (m01 > 1) m01 = 1; mix = m01; }

    inline float getLfoFreq()   const { return lfoFreq; }
    inline float getDepth()     const { return depth; }
    inline float getBaseDelay() const { return baseDelay; }
    inline float getMix()       const { return mix; }

private:
    // 4096 = 2^12 → &MASK ile wrap
    static constexpr int   MAX_DELAY = 4096;
    static constexpr int   MASK      = MAX_DELAY - 1; // 0x0FFF
    static constexpr int   LFO_UPDATE_INTERVAL = 16; // örnek adedi

    EXT_RAM_ATTR float bufferL[MAX_DELAY];
    EXT_RAM_ATTR float bufferR[MAX_DELAY];

    int   writeIndex = 0;
    float lfoPhase = 0.0f;
    float currentLfoValue = 0.0f;
    int   updateCounter = 0;

    float sampleRate = 48000.0f;
    float lfoFreq    = 0.5f;     // Hz
    float depth      = 0.002f;   // seconds (±depth)
    float baseDelay  = 0.03f;    // seconds
    float mix        = 0.35f;    // 0..1

    // Güvenli dairesel, lineer interpolasyon
    inline float __attribute__((hot,always_inline)) IRAM_ATTR
    getInterpolatedSample(const float* buffer, float index) const {
        // Negatif indexleri de düzgün sarmak için floor tabanlı yaklaşım
        float i_floor = floorf(index);
        int i0 = ((int)i_floor) & MASK;       // wrap
        int i1 = (i0 + 1) & MASK;             // bir sonraki
        float frac = index - i_floor;         // [0..1)
        float s0 = buffer[i0];
        float s1 = buffer[i1];
        return s0 + (s1 - s0) * frac;
    }
};
