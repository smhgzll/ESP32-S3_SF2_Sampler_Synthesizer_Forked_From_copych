#pragma once

#include "lfo.h"
#include "config.h"

class FxChorus {

public:
    FxChorus() {
        sampleRate = SAMPLE_RATE;
        writeIndex = 0;
        lfoPhase = 0.0f;
        updateCounter = 0;
        currentLfoValue = 0.0f;
        
        for(int i = 0; i < MAX_DELAY; ++i) {
            bufferL[i] = 0.0f;
            bufferR[i] = 0.0f;
        }

        setLfoFreq(5.0f);
        setDepth(0.002f);
        setBaseDelay(0.03f);
        setWetDryMix(0.5f);
    }

    void process(float* left, float* right) {
        // Обновляем LFO только когда достигнут интервал
        if(++updateCounter > LFO_UPDATE_INTERVAL ) {
            updateCounter = 0;
          
            // Вычисляем новое значение LFO
            currentLfoValue = fast_sin(TWOPI * lfoPhase);
            
            // Обновляем фазу
            lfoPhase += lfoFreq * LFO_UPDATE_INTERVAL / sampleRate;
            if(lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            
            //currentLfoValue = lfo.getSample();
        }
        /*
        float t = (float)updateCounter / LFO_UPDATE_INTERVAL;
        float interpolatedLfo = previousLfoValue * (1.0f - t) + currentLfoValue * t;
        */

        // Используем сохраненное значение LFO
        float delayOffsetL = (baseDelay + depth * currentLfoValue) * sampleRate;
        float delayOffsetR = (baseDelay + depth * -currentLfoValue) * sampleRate;

        // Чтение из буфера
        float readIndexL = writeIndex - delayOffsetL;
        float readIndexR = writeIndex - delayOffsetR;
        
        // Обработка сигнала
        float delayedL = getInterpolatedSample(bufferL, readIndexL);
        float delayedR = getInterpolatedSample(bufferR, readIndexR);

        // Смешивание
        float outL = (*left * dry) + (delayedL * wet);
        float outR = (*right * dry) + (delayedR * wet);

        // Запись в буфер
        bufferL[writeIndex] = *left;
        bufferR[writeIndex] = *right;
        
        writeIndex = (writeIndex + 1) % MAX_DELAY;

        *left = outL;
        *right = outR;
    } 
    // Функции для настройки параметров
    void setLfoFreq(float freq) { lfoFreq = freq; lfo.setFreq(freq); }
    void setDepth(float d) { depth = d; }
    void setBaseDelay(float delay) { baseDelay = delay; }
    void setWetDryMix(float mix) { wet = mix; dry = 1.0f - mix; }

private:
    static const int MAX_DELAY = 4096;
    static const int LFO_UPDATE_INTERVAL = 16; // Обновлять LFO каждые 64 сэмпла
    
    float bufferL[MAX_DELAY] = {0};
    float bufferR[MAX_DELAY] = {0};
    int writeIndex = 0;
    float lfoPhase = 0.0f;
    float currentLfoValue = 0.0f;
    float previousLfoValue = 0.0f;
    int updateCounter = 0;
    
    // Параметры
    float sampleRate;
    float lfoFreq = 5.0f;
    float depth = 0.2f;
    float baseDelay = 0.1f;
    float wet = 0.5f;
    float dry = 0.5f;
    LFO lfo;
 
    float getInterpolatedSample(float* buffer, float index) {
        while(index < 0) index += MAX_DELAY;
        int indexInt = static_cast<int>(index);
        float frac = index - indexInt;
        int indexNext = (indexInt + 1) % MAX_DELAY;
        return (1.0f - frac) * buffer[indexInt] + frac * buffer[indexNext];
    }
    

};