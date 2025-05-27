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
 */


#include "voice.h"
#include "misc.h"

#include <esp_dsp.h>

static const char* TAG = "Voice";

inline float velocityToGain(uint32_t velocity) {
    //    return velocity * velocity * DIV_127 * DIV_127; // square velocity
    // return velocity * DIV_127; // linear velocity
    return velocity * DIV_127; // linear velocity
    //return powf(velocity, 0.6f); // 0.6 = 60% powerlaw, approximating sqrt()
}

void Voice::start(uint8_t channel_, uint8_t note_, uint8_t velocity_, SampleHeader* s_, Zone zone_, ChannelState* ch) {

    zone = std::move(zone_);
    samplesRun = lastSamplesRun = 0;

    modWheel            = &ch->modWheel;
    modVolume           = &ch->volume;
    modExpression       = &ch->expression;
    modPitchBendFactor  = &ch->pitchBendFactor;
    modPan              = &ch->pan;
    modSustain          = &ch->sustainPedal;
    modPortaTime        = &ch->portaTime;
    modPortamento       = &ch->portamento;

    int startNote = ch->portaCurrentNote;
    ch->portaCurrentNote = note_;    

    chFilter.setCoeffs(&ch->filterCoeffs);
    chFilter.resetState();
    note     = note_;
    velocity = velocity_;
    channel  = channel_;
    sample   = s_;
    forward  = true;
    phase    = 0.0f;
    noteHeld = true;

    data = reinterpret_cast<int16_t*>(__builtin_assume_aligned(sample->data, 4));

    exclusiveClass = zone.exclusiveClass;

    // Precompute velocity-dependent gain
    velocityVolume = velocityToGain(velocity) * zone.attenuation;



    // Root key calculation
    // cheat for SF2 , as we are not processing modEnvelope, but want to be in tune:
    float modEnvStaticTune =  (zone.modAttackTime < 1.0f) ? ( 1.0f - zone.modSustainLevel) * zone.modEnvToPitch * 0.01f : 0.0f;
    // float modEnvStaticTune =  zone.modEnvToPitch * 0.01f ;

    int rootKey = (zone.rootKey >= 0) ? zone.rootKey : s_->originalPitch;
    float semi = float(note_ - rootKey) + (s_->pitchCorrection * 0.01f) + zone.coarseTune + zone.fineTune;

    float noteRatio = exp2f((modEnvStaticTune + semi) * DIV_12);
    float baseStep = float(sample->sampleRate) * DIV_SAMPLE_RATE;
    basePhaseIncrement = baseStep * noteRatio;
    
    portamentoActive = modPortamento && *modPortamento;

    if (portamentoActive) {
        float noteDiff = float( note_ - startNote);              // in semitones
        float freqRatio = exp2f(noteDiff * DIV_12);              // frequency ratio

        float timeSec = 0.01f + (*modPortaTime) * 0.5f;         // avoid zero
        float totalSamples = timeSec * SAMPLE_RATE;
        currentPhaseIncrement = basePhaseIncrement / freqRatio;
        portamentoFactor = 1.0f / freqRatio;
        portamentoLogDelta = exp2f(log2f(freqRatio) / totalSamples);
    } else {
        currentPhaseIncrement = basePhaseIncrement;
        portamentoLogDelta = 1.0f;
        portamentoFactor = 1.0f;
    } 

    // Vibrato LFO setup (zone-scoped params)
    vibLfoPhase = 0.0f;
    vibLfoPhaseIncrement = zone.vibLfoFreq *  DIV_SAMPLE_RATE;
    vibLfoToPitch = zone.vibLfoToPitch;  // in cents
    if (vibLfoToPitch == 0.0f) vibLfoToPitch = 50.0f; // prevent zero effect
    vibLfoDelaySamples = zone.vibLfoDelay * SAMPLE_RATE;
    vibLfoCounter = 0;
    vibLfoActive = false;

    updatePitch();

    // Final reverb and chorus sends
    reverbAmount  = zone.reverbSend  * ch->reverbSend;
    chorusAmount  = zone.chorusSend  * ch->chorusSend;

    // Apply pan from modPan (pointer to ChannelState pan)
    updatePan();

    // Setup ADSR envelope
    ampEnv.setAttackTime(zone.attackTime);
    ampEnv.setDecayTime(zone.decayTime);
    ampEnv.setHoldTime(zone.holdTime);
    ampEnv.setSustainLevel(zone.sustainLevel);
    ampEnv.setReleaseTime(zone.releaseTime);
    ampEnv.retrigger(Adsr::END_NOW);

    // Loop setup
    int32_t loopStartOffset = zone.loopStartOffset + (zone.loopStartCoarseOffset << 15); // x32768
    int32_t loopEndOffset   = zone.loopEndOffset   + (zone.loopEndCoarseOffset   << 15); // x32768

    length    = sample->end - sample->start;
    loopType  = static_cast<LoopType>(zone.sampleModes & 0x0003);
    loopStart = sample->startLoop + loopStartOffset - sample->start;
    loopEnd   = sample->endLoop   + loopEndOffset   - sample->start;
    loopLength = loopEnd - loopStart;

    if (loopType == NO_LOOP || loopStart < 0 || loopEnd > length || loopLength <= 0) {
        loopType = NO_LOOP;
    }


#ifdef ENABLE_IN_VOICE_FILTERS
	// Filter cutoff and resonance from zone
	filterCutoff = zone.filterFc;
	filterResonance = (zone.filterQ <= 0.0f) ? 0.707f : 1.0f / powf(10.0f, zone.filterQ / 20.0f);
    filterCutoff = fclamp(filterCutoff, 10.0f, 20000.0f)
	filter.resetState();
	filter.setFreqAndQ(filterCutoff, filterResonance);
#endif

    active = true;

    ESP_LOGD(TAG, "ch=%d note=%d atk=%.5f hld=%.5f dcy=%.5f sus=%.3f rel=%.5f loopStart=%u loopEnd=%u loopType=%d",
        channel, note, zone.attackTime, zone.holdTime, zone.decayTime, zone.sustainLevel, zone.releaseTime,
        static_cast<uint32_t>(loopStart), static_cast<uint32_t>(loopEnd), loopType);

    ESP_LOGD(TAG, "ch=%d reverb=%.5f chorus=%.5f delay=%.5f", channel, reverbAmount, chorusAmount, ch->delaySend);

    ESP_LOGD(TAG, "modToPitch=%.3f modEnvSustain=%.5f coarseTune=%.3f fineTune=%.3f", zone.modEnvToPitch, zone.modSustainLevel, zone.coarseTune, zone.fineTune);
}

void Voice::stop() {
    if (!(modSustain && *modSustain) ) {
        ampEnv.end(Adsr::END_REGULAR);
    }
}

void Voice::kill() {
    ampEnv.end(Adsr::END_NOW);
    active = false;
}

void Voice::die() {
    noteHeld = false;
    ampEnv.end(Adsr::END_FAST);
}

bool Voice::isRunning() const {
    return ampEnv.isRunning();
}

float __attribute__((always_inline)) Voice::nextSample() {
    if (!sample) {
        active = false;
        return 0.0f;
    }

    updatePitch();
    // for syncin time-based functions (like LFOs)
    samplesRun++;

    // Phase wrapping / voice lifetime control
    bool endReached = false;
    switch (loopType) {
        case FORWARD_LOOP:
            if (phase >= loopEnd)
                phase -= loopLength;
            break;

        case SUSTAIN_LOOP:
            if (ampEnv.getCurrentSegment() != Adsr::ADSR_SEG_RELEASE) {
                if (phase >= loopEnd)
                    phase -= loopLength;
            } else if (phase >= length) {
                endReached = true;
            }
            break;

        case PING_PONG_LOOP:
            if (forward) {
                if (phase >= loopEnd) {
                    phase = 2.0f * loopEnd - phase;
                    forward = false;
                }
            } else {
                if (phase <= loopStart) {
                    phase = 2.0f * loopStart - phase;
                    forward = true;
                }
            }
            break;

        case NO_LOOP:
        default:
            if (phase >= length)
                endReached = true;
            break;
    }

    if (endReached) {
        active = false;
        return 0.0f;
    }

    // Sample fetch + linear interpolation (unrolled and minimal branching)
    uint32_t idx = (uint32_t)phase;
    float frac = phase - (float)idx;

    float s0 = data[(idx > 0) ? (idx - 1) : 0];
    float s1 = data[idx];

    // smp = (s0 + frac * (s1 - s0)) * ONE_DIV_32768;
    float interp = __builtin_fmaf((s1 - s0), frac, s0);  // s0 + (s1 - s0) * frac
    float smp = interp * ONE_DIV_32768;

    // Phase advance
    if (loopType == PING_PONG_LOOP) {
        phase += (forward ? effectivePhaseIncrement : -effectivePhaseIncrement);
    } else {
        phase += effectivePhaseIncrement;
    }

    // Envelope process
    float env = ampEnv.process();

    if (ampEnv.isIdle()) {
        active = false;
        currentPhaseIncrement = targetPhaseIncrement;  // reset immediately
        return 0.0f;
    }
    
    float val = smp * velocityVolume * env * (*modVolume) * (*modExpression);

#ifdef ENABLE_IN_VOICE_FILTERS
    val = filter.process(val);
#endif

#ifdef ENABLE_CH_FILTER_M
    val = chFilter.process(val);
#endif

    return val;

}

void Voice::renderBlock(float* block) {
    for (uint32_t i = 0; i < DMA_BUFFER_LEN; i++) {
        block[i] = nextSample() ;
    }
}

void Voice::updateScore() {
    if (!active || !sample) {
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
    active = false;
    panL = 1.0f;
    panR = 1.0f;
    velocityVolume = 1.0f;
    sample = nullptr;
    ampEnv.init(SAMPLE_RATE);
    id = usage;
    usage++;
    ESP_LOGD(TAG, "id=%d sr=%d", id, SAMPLE_RATE);
}

void Voice::printState() {
    ESP_LOGI(TAG, "id=%d seg=%s val=%.5f", id, ampEnv.getCurrentSegmentStr(), ampEnv.getVal() );
}