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
 * Purpose: Voice generator struct for SF2 synthesizer
 * ----------------------------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"
#include "channel.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "biquad2.h"

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    UNUSED = 2,
    SUSTAIN_LOOP = 3,
    PING_PONG_LOOP = 4 // never used
};

struct Voice {
    float   phase = 0.0f; 
    float   velocityVolume = 1.0f;
    float   panL = 1.0f, panR = 1.0f;
    float   score = 0.0f;
    float   reverbAmount = 0.0f;
    float   chorusAmount = 0.0f;
    float   expression = 0.0f;
    float   volume = 1.0f;
    uint32_t exclusiveClass = 0;
    size_t  samplesRun = 0;
    size_t  lastSamplesRun = 0;

#ifdef ENABLE_IN_VOICE_FILTERS
    BiquadFilterInternalCoeffs filter;
    float filterCutoff = 20000.0f;
    float filterResonance = 0.0f;
#endif

#ifdef ENABLE_CH_FILTER_M
    BiquadFilterSharedCoeffs chFilter;
#endif

    // Channel mod kaynakları
    float*     modWheel = nullptr;
    float*     modVolume = nullptr;
    float*     modExpression = nullptr;
    float*     modPitchBendFactor = nullptr;
    float*     modPan = nullptr;
    float*     modPortaTime = nullptr;
    uint32_t*  modPortamento = nullptr;
    uint32_t*  modSustain = nullptr;
    uint32_t   noteHeld = false;
    
    float modFactor = 1.0f;
    float vibFactor = 1.0f;
    float pitchMod  = 1.0f;

    // LFO state
    float    vibLfoPhase = 0.0f;
    float    vibLfoPhaseIncrement = 0.0f;
    uint32_t vibLfoCounter = 0;
    uint32_t vibLfoDelaySamples = 0;
    bool     vibLfoActive = false;
    float    vibLfoToPitch = 50.0f;

    // Pitch/step
    float basePhaseIncrement      = 1.0f;   // from note + tuning
    float targetPhaseIncrement    = 0.0f;   // pitch bend hedefi
    float currentPhaseIncrement   = 0.0f;   // anlık (geçişte)
    float effectivePhaseIncrement = 0.0f;   // render’da kullanılan
    float portamentoLogDelta      = 0.0f;   // (eski; kalabilir)
    float div_basePhaseIncrement  = 0.0f;

    // Portamento
    float     portamentoFactor         = 1.0f;   // anlık çarpan
    float     targetPortamentoFactor   = 1.0f;
    float     portamentoRate           = 0.0005f;
    float     portamentoTime           = 0.0f;   // sec
    float     portamentoSpeed          = 0.0f;
    uint32_t  portamentoActive         = false;
    float     portaLog2PerSample       = 0.0f;   // <<< YENİ: log2(freqRatio)/totalSamples

    // Sample/loop
    uint32_t  length     = 0;
    uint32_t  loopStart  = 0;
    uint32_t  loopEnd    = 0;
    uint32_t  loopLength = 0;
    uint32_t  active     = false; 
    uint32_t  forward    = true; // ping-pong
    LoopType  loopType   = NO_LOOP;
    SampleHeader* sample = nullptr;
    Zone       zone = {};

    const int16_t* data = nullptr; // <<< YENİ: const

    Adsr     ampEnv;

    uint32_t note = 0;
    uint32_t velocity = 0;
    uint32_t channel = 0;

    // Envelope cache (control tarafında skor için)
    float envLast = 0.0f; // <<< YENİ

    // API
    void prepareStart(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan);
    void startNew(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan);
    void startLegato(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan, bool retrig);

    void stop();
    void kill();
    void die();
    bool  isRunning() const;
    float nextSample();
    void  renderBlock(float* block);
    void  init();
    static int usage; // = 0
    int   id = 0;

    void updateScore();
    void updatePortamento();
    void updatePitchOnly(uint8_t newNote, ChannelState* chan);

    inline __attribute__((always_inline)) void updatePan() {
        float pZone = zone.pan;
        float pMod  = modPan ? (*modPan * 2.0f - 1.0f) : 0.0f;

        float p = fclamp(pZone + pMod, -1.0f, 1.0f);  // [-1,1]
        p = 0.5f * (p + 1.0f);                        // [0,1]

        // Equal-power panning yerine basit linear panning
        panL = 1.0f - p * 0.5f;
        panR = 0.5f + p * 0.5f;
    }

    void updatePitchFactors();

    inline void __attribute__((always_inline)) updatePitch() {
        effectivePhaseIncrement = basePhaseIncrement * (*modPitchBendFactor) * portamentoFactor * pitchMod;
    }
    
    void setPortamentoTarget(float targetNoteRatio);
    void printState();
    bool isLegato = false;
};
