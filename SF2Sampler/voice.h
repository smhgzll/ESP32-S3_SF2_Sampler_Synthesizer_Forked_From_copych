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
 * File: voice.h
 * Purpose: Voice generator class for SF2 synthesizer
 * ----------------------------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "biquad2.h"

struct ChannelState {
    // Bank and Program
    
    float dryL[DMA_BUFFER_LEN] = {0};
    float dryR[DMA_BUFFER_LEN] = {0};

    float portaTime = 0.2f;  // CC#5 value, mapped from 0.0 to 1.0
    float volume = 1.0f;        // CC#7, 0.0–1.0
    float expression = 1.0f;    // CC#11, 0.0–1.0
    float pan = 0.5f;           // CC#10, 0.0 = left, 1.0 = right
        
    float modWheel = 0.0f;       // CC#1, 0.0–1.0
    
    int portaCurrentNote = 60;
    
    // Effects
    float reverbSend = 0.0f;    // CC#91, 0.0–1.0
    float chorusSend = 0.0f;    // CC#93, 0.0–1.0
    float delaySend = 0.0f;     // CC#95, 0.0-1.0

    // Pitch bend
    float pitchBend = 0.0f;     // -1.0 to +1.0 (centered)
    float pitchBendRange = 2.0f; // default ±2 semitones
    float pitchBendFactor = 1.0f;  // derived from pitchBend and pitchBendRange

    // Pedals
    uint32_t sustainPedal = false;  // CC#64
    uint32_t portamento = false ; // CC#65

    uint32_t  bankMSB = 0;     // CC#0
    uint32_t  bankLSB = 0;     // CC#32
    uint32_t  program = 0;     // Program Change
    
    // Utility
    inline uint16_t getBank() const {
        return (bankMSB << 7) | bankLSB;
    }
    
    inline void setBank(uint16_t bank) {
        bankMSB = 0b01111111 & (bank >> 7);
        bankLSB = 0b01111111 & bank; 
    }

    inline float getEffectiveVolume() const {
        return volume * expression;
    }

#ifdef ENABLE_CH_FILTER
    BiquadFilterInternalCoeffs filter;

    // Filter parameters
    float filterCutoff = 20000.0f;  // default no filtering
    float filterResonance = 0.707f;
    
    inline void updateFilter(float cutoff, float resonance) {
      filterCutoff = cutoff;
      filterResonance = resonance;
      filter.setFreqAndQ(cutoff, resonance);
    }
	
    inline void recalcFilter() {
        filter.setFreqAndQ(filterCutoff, filterResonance);
    }
    
    inline void resetFilter() {
      updateFilter(20000.0f, 0.707f);
    }
#elif defined(ENABLE_CH_FILTER_M)
    
    BiquadCalc::Coeffs filterCoeffs;
    
    // Filter parameters
    float filterCutoff = 20000.0f;  // default no filtering
    float filterResonance = 0.707f;

    inline void updateFilter(float cutoff, float resonance) {
        filterCutoff = cutoff;
        filterResonance = resonance;
        filterCoeffs = BiquadCalc::calcCoeffs(cutoff, resonance, BiquadCalc::LowPass);
    }
	
    inline void recalcFilter() {
        filterCoeffs = BiquadCalc::calcCoeffs(filterCutoff, filterResonance, BiquadCalc::LowPass);
    }
    
    inline void resetFilter() {
      updateFilter(20000.0f, 0.707f);
    }
#endif



    inline void reset() {
      volume = 1.0f;         // CC#7
      pan = 0.0f;            // CC#10
      expression = 1.0f;     // CC#11
      pitchBend = 0.0f;         // Center
      pitchBendRange = 2.0f;     // Default
      pitchBendFactor = 1.0f; // No pitch bend
      modWheel = 0.0f;
      reverbSend = 0.05f; // CC#91
      chorusSend = 0.0f;  // CC#93
      delaySend = 0.0f;   // CC#95
      sustainPedal = false; // CC#64
      portaTime = 0.2f;      // CC#5
      portamento = false;    // CC#65

#if defined(ENABLE_CH_FILTER) || defined(ENABLE_CH_FILTER_M)
      resetFilter();
#endif
      
    }
};

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    SUSTAIN_LOOP = 2,
    PING_PONG_LOOP = 3
};

struct Voice {
    float phase = 0.0f; 
    float velocityVolume = 1.0f;
    float panL = 1.0f, panR = 1.0f;
    float score = 0.0f;
    float reverbAmount = 0.0f;
    float chorusAmount = 0.0f;
    float expression = 0.0f;
    float volume = 1.0f;
    uint32_t exclusiveClass = 0;
    size_t samplesRun = 0;
    size_t lastSamplesRun = 0;

#ifdef ENABLE_IN_VOICE_FILTERS
    BiquadFilterInternalCoeffs filter;
    float filterCutoff = 20000.0f;
    float filterResonance = 0.0f;
#endif

#ifdef ENABLE_CH_FILTER_M
    BiquadFilterSharedCoeffs chFilter;

#endif
    float* modWheel = nullptr; // Pointer to channel mod wheel
    float* modVolume = nullptr;
    float* modExpression = nullptr;
    float* modPitchBendFactor = nullptr;
    float* modPan = nullptr;
    float* modPortaTime = nullptr;
    uint32_t* modPortamento = nullptr;
    uint32_t* modSustain = nullptr;
    uint32_t noteHeld = false;
    

    float modFactor = 1.0f;
    float vibFactor = 1.0f;
    float pitchMod = 1.0f;
 

    // LFO state
    float vibLfoPhase = 0.0f;
    float vibLfoPhaseIncrement = 0.0f;
    uint32_t vibLfoCounter = 0;
    uint32_t vibLfoDelaySamples = 0;
    bool vibLfoActive = false;
    float vibLfoToPitch = 50.0f;

    float basePhaseIncrement = 1.0f;     // from note and tuning
    float targetPhaseIncrement = 0.0f;     // updated by pitch bend
    float currentPhaseIncrement = 0.0f;  // final value used in phase stepping
    float effectivePhaseIncrement = 0.0f; // current value used in phase stepping
    float portamentoLogDelta = 0.0f;
    float div_basePhaseIncrement = 0.0f;

    // Portamento
    float portamentoFactor = 1.0f;         // current factor applied
    float targetPortamentoFactor = 1.0f;   // target to glide to
    float portamentoRate = 0.0005f;        // tweak as needed for glide speed (smaller = slower)  float targetPhaseIncrement;
    float portamentoTime = 0.0f; // in seconds
    float portamentoSpeed = 0.0f; // computed based on time
    uint32_t portamentoActive = false;

    uint32_t  length = 0;
    uint32_t  loopStart = 0;
    uint32_t  loopEnd = 0;
    uint32_t  loopLength = 0;
    uint32_t  active = false; 
    uint32_t  forward = true; // for ping-pong
    LoopType  loopType = NO_LOOP;
    SampleHeader* sample = nullptr;
    Zone zone = {};
  //  ChannelState* ch = nullptr; 

    int16_t* data;

    Adsr ampEnv;

    uint32_t note = 0;
    uint32_t velocity = 0;
    uint32_t channel = 0;

    void start(uint8_t channel, uint8_t note_, uint8_t velocity_,  SampleHeader* s_,  Zone zone_,  ChannelState* ch);
    void stop();
    void kill();
    void die();
    bool isRunning() const;
    float nextSample();
    void renderBlock(float* block);
    void init();
    static int usage; // = 0
    int id = 0;

    void updateScore();
    void updatePortamento();

    inline void  __attribute__((always_inline)) updatePan() {
        float pZone = zone.pan;
        float pMod  = modPan ? (*modPan * 2.0f - 1.0f) : 0.0f;

        float p = fclamp(pZone + pMod, -1.0f, 1.0f);  // clamp to [-1, 1]
        p = 0.5f * (p + 1.0f);                        // remap to [0, 1]

        // Equal-power pan could be used instead here if needed
        panL = 1.0f - p * 0.5f;
        panR = 0.5f + p * 0.5f;
    }

    void updatePitchFactors();

    inline void __attribute__((always_inline)) updatePitch() {
        effectivePhaseIncrement = basePhaseIncrement * (*modPitchBendFactor) * portamentoFactor * pitchMod;
                        //        * modFactor
    }
    
    void setPortamentoTarget(float targetNoteRatio) ;
    void printState();
    bool isLegato = false;
};

