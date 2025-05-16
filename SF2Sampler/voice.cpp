#include "voice.h"
#include "misc.h"
static const char* TAG = "Voice";

inline float velocityToGain(uint32_t velocity) {
    return velocity * velocity * DIV_127 * DIV_127;  // square = common choice
}

void Voice::start(uint8_t channel_, uint8_t note_, uint8_t velocity_, SampleHeader* s_, Zone* z_,  ChannelState* ch) {
    modVolume    = &ch->volume;
    modExpression = &ch->expression; 
    modPitchBendFactor = &ch->pitchBendFactor;
    modPan       = &ch->pan;
    modSustain   = &ch->sustainPedal;
    modPortaTime = &ch->portaTime;
    modPortamento = &ch->portamento;

    note     = note_;
    velocity = velocity_;
    channel = channel_;
    sample   = s_;
    zone     = z_;
    forward  = true;
    phase = 0.0f;
    exclusiveClass = z_->exclusiveClass;
    noteHeld = true;
    
    velocityVolume = velocityToGain(velocity) * z_->attenuation;
    data = reinterpret_cast<int16_t*>(__builtin_assume_aligned(sample->data, 4));
    int rootKey = (z_->rootKey >= 0) ? z_->rootKey : s_->originalPitch;
    float semi = (note_ - rootKey) + (s_->pitchCorrection * 0.01f) + z_->coarseTune + z_->fineTune;
    float noteRatio = exp2f(semi * DIV_12);

    float baseStep = static_cast<float>(sample->sampleRate) * DIV_SAMPLE_RATE;

    basePhaseIncrement = baseStep * noteRatio;
 
    // Pitch bend and portamento will modulate basePhaseIncrement 

    portamentoActive = *modPortamento && modPortamento;
    
    // Portamento time control
    if (portamentoActive) { 
        float timeInSeconds = 0.01f + ch->portaTime * 0.5f;  // e.g. 10ms to 510ms
        portamentoRate = 1.0f / (timeInSeconds * SAMPLE_RATE);
        setPortamentoTarget(basePhaseIncrement * (*modPitchBendFactor));

        // If not legato, start directly at the target
        if (!isLegato) {
            currentPhaseIncrement = targetPhaseIncrement;
        }
    }
    reverbAmount = z_->reverbSend * ch->reverbSend;
    chorusAmount =  z_->chorusSend * ch->chorusSend;
    
    updatePan();

    ampEnv.setAttackTime(z_->attackTime);
    ampEnv.setDecayTime(z_->decayTime);
    ampEnv.setHoldTime(z_->holdTime);
    ampEnv.setSustainLevel(z_->sustainLevel);
    ampEnv.setReleaseTime(z_->releaseTime);
    ampEnv.retrigger(Adsr::END_NOW);
 

    length = sample->end - sample->start;

    loopType = static_cast<LoopType>(z_->sampleModes & 0x0003);

    loopStart = sample->startLoop + z_->loopStartOffset + z_->loopStartCoarseOffset * 32768;
    loopEnd   = sample->endLoop   + z_->loopEndOffset   + z_->loopEndCoarseOffset * 32768;
    loopStart -= sample->start;
    loopEnd   -= sample->start;
    loopLength = loopEnd - loopStart;

    if (loopType == NO_LOOP || loopStart < 0 || loopEnd > length || loopLength <= 0) {
        loopType = NO_LOOP;
    }

    active = true;

    ESP_LOGD(TAG, "ch=%d note=%d attack=%.5f hold=%.5f decay=%.5f sustain=%.3f release=%.5f loopStart=%u loopEnd=%u loopType=%d",
        channel_, note_, z_->attackTime, z_->holdTime, z_->decayTime, z_->sustainLevel, z_->releaseTime, loopStart, loopEnd, loopType);
    ESP_LOGI(TAG, "ch=%d reverb=%.5f chorus=%.5f", channel, reverbAmount, chorusAmount);
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

float Voice::nextSample() {
  // already checked in synth::render  if (!active) return 0.0f;
    if (!sample) {
      active = false;
      return 0.0f;
    }
    updatePitch();
    float vol = (*modVolume) * (*modExpression);

    float WORD_ALIGNED_ATTR s0, s1, out;

    if (portamentoActive) {
        float delta = targetPhaseIncrement - currentPhaseIncrement;
        if (fabsf(delta) <= portamentoRate) {
            currentPhaseIncrement = targetPhaseIncrement;
            portamentoActive = false;
        } else {
            currentPhaseIncrement += (delta > 0 ? +portamentoRate : -portamentoRate);
        }
    }

    // Handle phase bounds and wrapping
    switch (loopType) {
        case FORWARD_LOOP:
            if (phase >= loopEnd) {
                phase -= loopLength;
            }
            
            s0 = data[(uint32_t)phase-1]  ;
            s1 = data[(uint32_t)phase]  ;
            out = lin_interpolate(s0, s1, phase) * ONE_DIV_32768 ;
            phase += currentPhaseIncrement;
            break;
            
        case SUSTAIN_LOOP: {
            const Adsr::eSegment_t seg = ampEnv.getCurrentSegment();
          //  if (seg != Adsr::ADSR_SEG_RELEASE && seg != Adsr::ADSR_SEG_IDLE) {
                if (seg != Adsr::ADSR_SEG_RELEASE ) {
                    if (phase >= loopEnd) {
                        phase -= loopLength;
                    }
                } else if (phase >= length) {
                    active = false;
                    return 0.0f;
                }
                s0 = data[(uint32_t)phase-1]  ;
                s1 = data[(uint32_t)phase]  ;
                out = lin_interpolate(s0, s1, phase) * ONE_DIV_32768 ;
                phase += currentPhaseIncrement;
            }
            break;
            
        case PING_PONG_LOOP:
            if (forward) {
                if (phase >= loopEnd) {
                    phase = 2.0f * loopEnd - phase;
                    forward = false;
                }
                s0 = data[(uint32_t)phase-1]  ;
                s1 = data[(uint32_t)phase]  ;
                out = lin_interpolate(s0, s1, phase) * ONE_DIV_32768 ;
                phase += currentPhaseIncrement;
            } else {
                if (phase <= loopStart) {
                    phase = 2.0f * loopStart - phase;
                    forward = true;
                }
                s0 = data[(uint32_t)phase-1]  ;
                s1 = data[(uint32_t)phase]  ;
                out = lin_interpolate(s0, s1, phase) * ONE_DIV_32768 ;
                phase -= currentPhaseIncrement;
            }
            break;
            
        case NO_LOOP:
        default:
            if (phase >= length) {
                phase = length - 1;  
                active = false;
                return 0.0f;
            }
            s0 = data[(uint32_t)phase-1]  ;
            s1 = data[(uint32_t)phase]  ;
            out = lin_interpolate(s0, s1, phase) * ONE_DIV_32768 ;
            phase += currentPhaseIncrement;
    }
 
    // Apply envelope
    float env = ampEnv.process();

    if (ampEnv.isIdle()) {
        active = false;
        currentPhaseIncrement = targetPhaseIncrement;
    }

    return out * env * velocityVolume;
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