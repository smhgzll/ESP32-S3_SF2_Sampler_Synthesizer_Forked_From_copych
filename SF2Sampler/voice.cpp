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

void Voice::start(uint8_t channel_, uint8_t note_, uint8_t velocity_, SampleHeader* s_, Zone* z_, ChannelState* ch) {
    modVolume        = &ch->volume;
    modExpression    = &ch->expression;
    modPitchBendFactor = &ch->pitchBendFactor;
    modPan           = &ch->pan;
    modSustain       = &ch->sustainPedal;
    modPortaTime     = &ch->portaTime;
    modPortamento    = &ch->portamento;

    note     = note_;
    velocity = velocity_;
    channel  = channel_;
    sample   = s_;
    zone     = z_;
    forward  = true;
    phase    = 0.0f;
    noteHeld = true;

    data = reinterpret_cast<int16_t*>(__builtin_assume_aligned(sample->data, 4));

    exclusiveClass = z_->exclusiveClass;

    // Precompute velocity-dependent gain
    velocityVolume = velocityToGain(velocity) * z_->attenuation;

    // Root key calculation
    int rootKey = (z_->rootKey >= 0) ? z_->rootKey : s_->originalPitch;
    float semi = float(note_ - rootKey) + (s_->pitchCorrection * 0.01f) + z_->coarseTune + z_->fineTune;

    // Pitch increment base calculation
    float noteRatio = exp2f(semi * DIV_12); // DIV_12 = 1.0f / 12.0f
    float baseStep = float(sample->sampleRate) * DIV_SAMPLE_RATE; // DIV_SAMPLE_RATE = 1.0f / SAMPLE_RATE
    basePhaseIncrement = baseStep * noteRatio;

    // Handle portamento setup
    portamentoActive = modPortamento && *modPortamento;

    currentPhaseIncrement = basePhaseIncrement * (*modPitchBendFactor);
    if (portamentoActive) {
        float timeSec = 0.01f + (*modPortaTime) * 0.5f; // Portamento time: 10ms to 510ms
        portamentoRate = 1.0f / (timeSec * SAMPLE_RATE);
        setPortamentoTarget(basePhaseIncrement * (*modPitchBendFactor));

        currentPhaseIncrement = isLegato ? currentPhaseIncrement : targetPhaseIncrement;
    }

    // Final reverb and chorus sends
    reverbAmount  = z_->reverbSend  * ch->reverbSend;
    chorusAmount  = z_->chorusSend  * ch->chorusSend;

    // Apply pan from modPan (pointer to ChannelState pan)
    updatePan();

    // Setup ADSR envelope
    ampEnv.setAttackTime(z_->attackTime);
    ampEnv.setDecayTime(z_->decayTime);
    ampEnv.setHoldTime(z_->holdTime);
    ampEnv.setSustainLevel(z_->sustainLevel);
    ampEnv.setReleaseTime(z_->releaseTime);
    ampEnv.retrigger(Adsr::END_NOW);

    // Loop setup
    int32_t loopStartOffset = z_->loopStartOffset + (z_->loopStartCoarseOffset << 15); // x32768
    int32_t loopEndOffset   = z_->loopEndOffset   + (z_->loopEndCoarseOffset   << 15); // x32768

    length    = sample->end - sample->start;
    loopType  = static_cast<LoopType>(z_->sampleModes & 0x0003);
    loopStart = sample->startLoop + loopStartOffset - sample->start;
    loopEnd   = sample->endLoop   + loopEndOffset   - sample->start;
    loopLength = loopEnd - loopStart;

    if (loopType == NO_LOOP || loopStart < 0 || loopEnd > length || loopLength <= 0) {
        loopType = NO_LOOP;
    }


#ifdef ENABLE_IN_VOICE_FILTERS
	// Filter cutoff and resonance from zone
	filterCutoff = z_->filterFc;
	filterResonance = (z_->filterQ <= 0.0f) ? 0.707f : 1.0f / powf(10.0f, z_->filterQ / 20.0f);
    filterCutoff = fclamp(filterCutoff, 10.0f, 20000.0f)
	filter.resetState();
	filter.setFreqAndQ(filterCutoff, filterResonance);
#endif

    active = true;

    ESP_LOGD(TAG, "ch=%d note=%d atk=%.5f hld=%.5f dcy=%.5f sus=%.3f rel=%.5f loopStart=%u loopEnd=%u loopType=%d",
        channel, note, z_->attackTime, z_->holdTime, z_->decayTime, z_->sustainLevel, z_->releaseTime,
        static_cast<uint32_t>(loopStart), static_cast<uint32_t>(loopEnd), loopType);

    ESP_LOGD(TAG, "ch=%d reverb=%.5f chorus=%.5f delay=%.5f",
        channel, reverbAmount, chorusAmount, ch->delaySend);
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

    // Portamento smoothing
    if (portamentoActive) {
        float delta = targetPhaseIncrement - currentPhaseIncrement;
        float absDelta = __builtin_fabsf(delta);
        if (absDelta <= portamentoRate) {
            currentPhaseIncrement = targetPhaseIncrement;
            portamentoActive = false;
        } else {
            currentPhaseIncrement += (delta >= 0.0f) ? portamentoRate : -portamentoRate;
        }
    }

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
        phase += (forward ? currentPhaseIncrement : -currentPhaseIncrement);
    } else {
        phase += currentPhaseIncrement;
    }

    // Envelope process
    float env = ampEnv.process();

    if (ampEnv.isIdle()) {
        active = false;
        currentPhaseIncrement = targetPhaseIncrement;  // reset immediately
        return 0.0f;
    }

#ifdef ENABLE_IN_CHANNEL_FILTERS
    return filter.process(smp) * env * velocityVolume;
#else
    return smp * env * velocityVolume;
#endif
}

/*

float Voice::nextSample() {
    if (!sample) {
        active = false;
        return 0.0f;
    }

    updatePitch();

    // Portamento
    if (portamentoActive) {
        float delta = targetPhaseIncrement - currentPhaseIncrement;
        if (__builtin_fabsf(delta) <= portamentoRate) {
            currentPhaseIncrement = targetPhaseIncrement;
            portamentoActive = false;
        } else {
            currentPhaseIncrement += (delta >= 0.0f) ? portamentoRate : -portamentoRate;
        }
    }

    // Loop handling
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
                active = false;
                return 0.0f;
            }
            break;

        case PING_PONG_LOOP:
            if (forward) {
                if (phase >= loopEnd) {
                    phase = __builtin_fmaf(-1.0f, phase - loopEnd, loopEnd * 2.0f);  // phase = 2*loopEnd - phase
                    forward = false;
                }
            } else {
                if (phase <= loopStart) {
                    phase = __builtin_fmaf(-1.0f, loopStart - phase, loopStart * 2.0f);  // phase = 2*loopStart - phase
                    forward = true;
                }
            }
            break;

        case NO_LOOP:
        default:
            if (phase >= length) {
                active = false;
                return 0.0f;
            }
            break;
    }

    // Sample interpolation (linear)
    uint32_t idx = (uint32_t)phase;
    float frac = phase - (float)idx;

    float s0 = data[idx > 0 ? idx - 1 : 0];
    float s1 = data[idx];
    float interp = __builtin_fmaf((s1 - s0), frac, s0);  // s0 + (s1 - s0) * frac
    float smp = interp * ONE_DIV_32768;

    // Phase increment
    phase += (loopType == PING_PONG_LOOP && !forward) ? -currentPhaseIncrement : currentPhaseIncrement;

    // Envelope
    float env = ampEnv.process();
    if (ampEnv.isIdle()) {
        active = false;
        currentPhaseIncrement = targetPhaseIncrement;
    }

#ifdef ENABLE_IN_CHANNEL_FILTERS
    return filter.process(smp) * env * velocityVolume;
#else
    return smp * env * velocityVolume;
#endif
}
*/

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

void Voice::setPortamentoTarget(float target) {
    targetPhaseIncrement = target;

    if (portamentoRate > 0.0f) {
        portamentoActive = true;
    } else {
        currentPhaseIncrement = target; // snap immediately if no glide
        portamentoActive = false;
    }
}

void Voice::updatePortamento() {
    if (!portamentoActive) return;

    float diff = targetPortamentoFactor - portamentoFactor;

    // If the difference is small, snap to the target
    if (fabsf(diff) < 0.0001f) {
        portamentoFactor = targetPortamentoFactor;
        portamentoActive = false;
    } else {
        portamentoFactor += diff * portamentoRate;
    }
    updatePitch();
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