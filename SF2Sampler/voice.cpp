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
 * File: voice.cpp
 * Purpose: Voice generation routines
 * ----------------------------------------------------------------------------
 *
 * NOT: voice.h içinde `updatePitchFactors()` SADECE deklare edilmiş olmalı
 * (inline gövde olursa "redefinition" hatası alırsın).
 */

#include "voice.h"
#include "misc.h"
#include <math.h>
#include <esp_dsp.h>

static const char* TAG = "Voice";

#ifndef HOT
  #define HOT __attribute__((hot))
#endif
#ifndef FORCE_INLINE
  #define FORCE_INLINE __attribute__((always_inline)) inline
#endif
#ifndef LIKELY
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// 1: LFO/portamento'yu her örnekte (nextSample sonunda) ilerlet
// 0: Blok sonunda (Synth::renderLRBlock içinde) ilerlet
#ifndef PITCH_FACTORS_PER_SAMPLE
  #define PITCH_FACTORS_PER_SAMPLE 0
#endif

static FORCE_INLINE float velocityToGain(uint32_t velocity) {
    return velocity * DIV_127;           // ucuz ve yeterli (lineer)
    // alternatifler:
    // return velocity * velocity * DIV_127 * DIV_127;
    // return powf(velocity, 0.6f);
}

void Voice::prepareStart(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan) {
    zone   = z;
    sample = zone.sample;

    // Parser sample->data'yı sample->start'a göre hizalı veriyor → ekstra offsetleme yok.
    data = reinterpret_cast<const int16_t*>(__builtin_assume_aligned(sample->data, 4));

    const int startNote = chan->portaCurrentNote;

    note        = note_;
    velocity    = vel;
    channel     = ch;
    forward     = true;

    // İlk örnekte s0 = data[0] okunabilsin diye 1.0f'tan başlıyoruz (branchless)
    phase       = 1.0f;

    noteHeld    = true;
    samplesRun  = 0;
    lastSamplesRun = 0;
    exclusiveClass = zone.exclusiveClass;

    // Mod kaynak pointer'ları (hot-path'te kopya yok)
    modWheel            = &chan->modWheel;
    modVolume           = &chan->volume;
    modExpression       = &chan->expression;
    modPitchBendFactor  = &chan->pitchBendFactor;
    modPan              = &chan->pan;
    modSustain          = &chan->sustainPedal;
    modPortaTime        = &chan->portaTime;
    modPortamento       = &chan->portamento;

#ifdef ENABLE_CH_FILTER_M
    chFilter.setCoeffs(&chan->filterCoeffs);
    chFilter.resetState();
#endif

    velocityVolume = velocityToGain(velocity) * zone.attenuation;

    // Statik mod-env pitch katkısı (başlangıçta)
    const float modEnvStaticTune =
        (zone.modAttackTime < 1.0f)
            ? (1.0f - zone.modSustainLevel) * zone.modEnvToPitch * 0.01f
            : 0.0f;

    const int   rootKey   = (zone.rootKey >= 0) ? zone.rootKey : sample->originalPitch;
    const float semi      = float(note_ - rootKey)
                          + (sample->pitchCorrection * 0.01f)
                          + zone.coarseTune + zone.fineTune
                          + chan->tuningSemitones;
    const float noteRatio = exp2f((modEnvStaticTune + semi) * DIV_12);
    const float baseStep  = float(sample->sampleRate) * DIV_SAMPLE_RATE;
    basePhaseIncrement    = baseStep * noteRatio;   // pitch bend / LFO / porta ile güncellenecek

    // Vibrato LFO
    vibLfoPhase          = 0.0f;
    vibLfoPhaseIncrement = zone.vibLfoFreq * DIV_SAMPLE_RATE;
    vibLfoToPitch        = (zone.vibLfoToPitch == 0.0f) ? 50.0f : zone.vibLfoToPitch;
    vibLfoDelaySamples   = zone.vibLfoDelay * SAMPLE_RATE;
    vibLfoCounter        = 0;
    vibLfoActive         = false;
    pitchMod             = 1.0f;

    // Portamento (log-domain step tanımı)
    portamentoActive = (modPortamento && *modPortamento);
    if (portamentoActive) {
        const float noteDiff     = float(note_ - startNote);
        const float freqRatio    = exp2f(noteDiff * DIV_12);
        const float timeSec      = 0.01f + (*modPortaTime) * 0.5f;
        const float totalSamples = fmaxf(1.0f, timeSec * SAMPLE_RATE);

        portamentoLogDelta   = exp2f(log2f(freqRatio) / totalSamples);   // örnek başına çarpan tabanı
        portamentoFactor     = 1.0f / freqRatio;                         // önceki sesten başla
    } else {
        portamentoLogDelta = 1.0f;
        portamentoFactor   = 1.0f;
    }

    updatePitch();
    updatePan();

    reverbAmount = zone.reverbSend * chan->reverbSend;
    chorusAmount = zone.chorusSend * chan->chorusSend;

    // Envelope
    ampEnv.setAttackTime (zone.attackTime  * chan->attackModifier);
    ampEnv.setDecayTime  (zone.decayTime);
    ampEnv.setHoldTime   (zone.holdTime);
    ampEnv.setSustainLevel(zone.sustainLevel);
    ampEnv.setReleaseTime(zone.releaseTime * chan->releaseModifier);

    // Döngü bilgileri (phase ile aynı referans: sample->start)
    const int32_t loopStartOffset = zone.loopStartOffset + (zone.loopStartCoarseOffset << 15);
    const int32_t loopEndOffset   = zone.loopEndOffset   + (zone.loopEndCoarseOffset   << 15);
    length     = sample->end - sample->start;
    loopStart  = sample->startLoop + loopStartOffset - sample->start;
    loopEnd    = sample->endLoop   + loopEndOffset   - sample->start;
    loopLength = loopEnd - loopStart;
    loopType   = static_cast<LoopType>(zone.sampleModes & 0x0003);
    if (loopType == UNUSED || loopStart < 0 || loopEnd > length || loopLength <= 0) {
        loopType = NO_LOOP;
    }

#ifdef ENABLE_IN_VOICE_FILTERS
    filterCutoff    = fclamp(zone.filterFc, 10.0f, 20000.0f);
    filterResonance = (zone.filterQ <= 0.0f) ? 0.707f : 1.0f / powf(10.0f, zone.filterQ / 20.0f);
    filter.resetState();
    filter.setFreqAndQ(filterCutoff, filterResonance);
#endif

    envLast = 0.0f; // skor için cache
}

void Voice::startNew(uint8_t ch, uint8_t note_, uint8_t vel, const Zone& z, ChannelState* chan) {
    prepareStart(ch, note_, vel, z, chan);
    ampEnv.retrigger(Adsr::END_NOW);
    active = true;
}

void Voice::updatePitchOnly(uint8_t newNote, ChannelState* chan) {
    const int   rootKey   = (zone.rootKey >= 0) ? zone.rootKey : sample->originalPitch;
    const float semi      = float(newNote - rootKey)
                          + (sample->pitchCorrection * 0.01f)
                          + zone.coarseTune + zone.fineTune;
    const float noteRatio = exp2f(semi * DIV_12);
    basePhaseIncrement    = float(sample->sampleRate) * DIV_SAMPLE_RATE * noteRatio;

    portamentoActive = (modPortamento && *modPortamento);
    if (portamentoActive) {
        const float noteDiff     = float(newNote - chan->portaCurrentNote);
        const float freqRatio    = exp2f(noteDiff * DIV_12);
        const float timeSec      = 0.01f + (*modPortaTime) * 0.5f;
        const float totalSamples = fmaxf(1.0f, timeSec * SAMPLE_RATE);
        portamentoLogDelta       = exp2f(log2f(freqRatio) / totalSamples);
        // portamentoFactor ses iş parçacığında 1.0'a yürüyecek
    } else {
        portamentoFactor   = 1.0f;
        portamentoLogDelta = 1.0f;
    }

    note = newNote;
    updatePitch();
}

void Voice::stop() {
    if (!(modSustain && *modSustain)) {
        noteHeld = false;
        ampEnv.end(Adsr::END_REGULAR);
    }
}

void Voice::kill() {
    ampEnv.end(Adsr::END_NOW);
    noteHeld = false;
    active = false;
}

void Voice::die() {
    noteHeld = false;
    ampEnv.end(Adsr::END_FAST);
}

bool Voice::isRunning() const {
    return ampEnv.isRunning();
}

// ---- HOT PATH: tek örnek üretimi ----
float HOT IRAM_ATTR Voice::nextSample() {
    if (UNLIKELY(!sample)) {
        active = false;
        return 0.0f;
    }

    // Bu örnek sabit increment ile üretilecek
    //updatePitch();

    // Örneği ürettikten sonra zaman tabanını ilerleteceğiz (aşağıda)
    // -> jitter/metalik bozulmayı önler

    // Örnek alma + lineer enterpolasyon (güvenli)
    const uint32_t idx  = (uint32_t)phase;
    if (UNLIKELY(idx >= (uint32_t)length)) {   // emniyet
        active = false;
        return 0.0f;
    }

    const float frac = phase - (float)idx;
    const uint32_t i0 = (idx > 0u) ? (idx - 1u) : 0u;

    const float s0 = (float)data[i0];
    const float s1 = (float)data[idx];
    const float interp = s0 + (s1 - s0) * frac;
    const float smp    = interp * ONE_DIV_32768;

    // Envelope sadece audio thread'de ilerler
    const float env = ampEnv.process();
    envLast         = env;

    float val = smp * velocityVolume * env * (*modVolume) * (*modExpression);

#ifdef ENABLE_IN_VOICE_FILTERS
    val = filter.process(val);
#endif
#ifdef ENABLE_CH_FILTER_M
    val = chFilter.process(val);
#endif

    // Faz/döngü
    switch (loopType) {
        case FORWARD_LOOP:
            phase += effectivePhaseIncrement;
            if (UNLIKELY(phase >= loopEnd)) phase -= loopLength;
            break;

        case SUSTAIN_LOOP:
            phase += effectivePhaseIncrement;
            if (noteHeld) {
                if (UNLIKELY(phase >= loopEnd)) phase -= loopLength;
            } else {
                loopType = NO_LOOP;
                if (UNLIKELY(phase >= length)) { active = false; return 0.0f; }
            }
            break;

        case PING_PONG_LOOP:
            if (forward) {
                phase += effectivePhaseIncrement;
                if (UNLIKELY(phase >= loopEnd)) {
                    phase   = 2.0f * loopEnd - phase;
                    forward = false;
                }
            } else {
                phase -= effectivePhaseIncrement;
                if (UNLIKELY(phase <= loopStart)) {
                    phase   = 2.0f * loopStart - phase;
                    forward = true;
                }
            }
            break;

        case NO_LOOP:
        default:
            phase += effectivePhaseIncrement;
            if (UNLIKELY(phase >= length)) { active = false; return 0.0f; }
            break;
    }

    if (UNLIKELY(ampEnv.isIdle())) {
        active = false;
        return 0.0f;
    }

    // Zaman sayaçları: SONDA (bir sonraki örnek için)
    samplesRun++;
#if PITCH_FACTORS_PER_SAMPLE
    updatePitchFactors();   // vibrato/porta ilerlemesi burada → stabil ses
#endif

    return val;
}

void Voice::renderBlock(float* block) {
    for (uint32_t i = 0; i < DMA_BUFFER_LEN; ++i) {
        block[i] = nextSample();
    }
}

// RT dışı: skor (envelope ilerletmez)
void Voice::updateScore() {
    if (UNLIKELY(!active || !sample)) {
        score = 0.0f;
        return;
    }

    float env = ampEnv.process();
    score = env * velocityVolume;

    if (!isRunning() || ampEnv.isIdle()) {
        score *= 0.1f;
    }
}

void __attribute__((always_inline))  Voice::updatePitchFactors() {
    // not clean if cross-threaded, but it's granular anyway ;-)
    float deltaSamplesRun = samplesRun - lastSamplesRun;
    lastSamplesRun = samplesRun;

    // Vibrato LFO
    if (!vibLfoActive) {
        vibLfoCounter += deltaSamplesRun;
        if (vibLfoCounter >= vibLfoDelaySamples)
            vibLfoActive = true;
        pitchMod = 1.0f;
    } else {
        vibLfoPhase += vibLfoPhaseIncrement * deltaSamplesRun;
        if (vibLfoPhase >= 1.0f) vibLfoPhase -= 1.0f;

        float lfo = sin_lut(vibLfoPhase);
        float cents = lfo * (*modWheel) * vibLfoToPitch;
        pitchMod = fastExp2(cents * (1.0f * DIV_1200));
    }

    // Portamento 
    if (portamentoActive) {
        portamentoFactor *= powf(portamentoLogDelta, deltaSamplesRun);
        currentPhaseIncrement = basePhaseIncrement * portamentoFactor;

        // Stop when close enough
        if ( (portamentoLogDelta >= 1.0f && portamentoFactor >= 1.0f) ||
            (portamentoLogDelta <= 1.0f && portamentoFactor <= 1.0f) ) {
            portamentoFactor = 1.0f;
            portamentoActive = false;
            currentPhaseIncrement = targetPhaseIncrement;
        }
        
    }

    // Mod LFO 
    // modFactor = ...
}


void Voice::init() {
    active         = false;
    panL           = 1.0f;
    panR           = 1.0f;
    velocityVolume = 1.0f;
    sample         = nullptr;
    envLast        = 0.0f;
    ampEnv.init(SAMPLE_RATE);
    id = usage;
    usage++;
    ESP_LOGD(TAG, "id=%d sr=%d", id, SAMPLE_RATE);
}

void Voice::printState() {
    ESP_LOGI(TAG, "id=%d seg=%s val=%.5f", id, ampEnv.getCurrentSegmentStr(), ampEnv.getVal());
}
