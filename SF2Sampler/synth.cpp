#include "synth.h"
#include <float.h>
#include <math.h>
static const char* TAG = "Synth";

float pitchBendRatioFromValue(int bendValue, float semitoneRange) {
    float norm = (bendValue - 8192) * DIV_8192; // Normalize to [-1, 1]
    float semis = norm * semitoneRange;
    return exp2f(semis * DIV_12);  // 2^(semitones/12)
}


Synth::Synth(SF2Parser& parserRef) : parser(parserRef) {
    // Initialize all 16 MIDI channels with default values
    for (int i = 0; i < 16; ++i) {
        channels[i] = ChannelState();  // Default-initialized
    }

    // Initialize all voices
    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i].init();
    }
}

void Synth::noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (ch >= 16 || vel == 0) return;
    
    ChannelState* chan = &channels[ch];
    Zone* zone = parser.getZoneForNote(note, vel, chan->getBank(), chan->program);
    
    if (!zone || !zone->sample) return;

    uint32_t exclusiveClass = zone->exclusiveClass;
    float newVelocityVolume = vel * DIV_127;
    float newScore = newVelocityVolume;

    Voice* v = findWeakestVoiceOnNote(ch, note, newScore, exclusiveClass);
    if (!v) v = findWorstVoice();

    ESP_LOGD(TAG, "ch %d note %d bend %f vol %f score %f id=%d",
             ch, note, chan->pitchBend, newVelocityVolume, newScore, v ? v->id : -1);

    if (v) {
        // Start the voice with channel context
        v->start(ch, note, vel, zone->sample, zone, chan);
    }
}


void Synth::noteOff(uint8_t ch, uint8_t note) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch && v.note == note) {
            {
                v.stop();
            }
        }
    }
}

void Synth::pitchBend(uint8_t ch, int value) {
    if (ch >= 16) return;
    channels[ch].pitchBend = value;

    float norm = (value - 8192) * DIV_8192;
    float semis = norm * channels[ch].pitchBendRange;
    channels[ch].pitchBendFactor = exp2f(semis * DIV_12);
}


void Synth::controlChange(uint8_t ch, uint8_t ctrl, uint8_t val) {

    if (ch >= 16) return;

    auto& state = channels[ch];
    auto& rpn = rpnState[ch];
    float fval = val * DIV_127;

    switch (ctrl) {

        case 0:  // Bank Select MSB
            state.bankMSB = val;
            break;
        case 32: // Bank Select LSB
            state.bankLSB = val;
            break;
        case 5:  // Portamento Time
            state.portaTime = fval;  
            break;
        case 7:  // Channel Volume
            state.volume = fval;
            break;
        case 11: // Expression
            state.expression = fval;
            break;
        case 10: // Pan
            state.pan = fval;
            for (Voice& v : voices) {
                if ( v.channel == ch) {
                    v.updatePan(); 
                }
            }
            break;
        case 64: // Sustain Pedal
            {
                bool sustainOn = val >= 64;
                state.sustainPedal = sustainOn;
                if (!sustainOn) {
                    // Release all sustained voices on this channel
                    for (Voice& v : voices) {
                        if (v.active && v.channel == ch) {
                            v.stop(); 
                        }
                    }
                }
            }
            break;
        case 91: // Reverb Send
            state.reverbSend = fval;
            break;
        case 93: // Chorus Send
            state.chorusSend = fval;
            break; 
        case 101: rpn.msb = val; break; // RPN MSB
        case 100: rpn.lsb = val; break; // RPN LSB
        case 120: // All Sound Off
            soundOff(ch);
            break;
        case 121: // Reset All Controllers
            state.reset(); // Reset modulation values for channel
            break;
        case 123: // All Notes Off
            allNotesOff(ch);
            break;
        case 6: // Data Entry MSB
            if (rpn.msb == 0 && rpn.lsb == 0) { // Pitch Bend Range
                state.pitchBendRange = static_cast<float>(val); // in semitones
            }
            break;
        case 38: // Data Entry LSB — cents, optional
            // (can be added if high resolution needed)
            break;
        default:
            break;
    }
}

void Synth::programChange(uint8_t ch, uint8_t program) {
    if (ch >= 16) return;

    auto& state = channels[ch];
    state.program = program;

    uint16_t bank = state.getBank();

    // Primary attempt
    if (parser.hasPreset(bank, program)) {
        ESP_LOGI(TAG, "Ch%u: Program=%u, Bank=%u", ch, program, bank);
        return;
    }

    // Fallback to Bank 0, same program
    ESP_LOGW(TAG, "Ch%u: Program %u not found in Bank %u, falling back to Bank 0", ch, program, bank);
    uint16_t fallbackBank = (bank & 0x007F);  // Keep LSB, zero MSB
    if (parser.hasPreset(fallbackBank, program)) {
        state.setBank(fallbackBank);
        ESP_LOGI(TAG, "Ch%u: Fallback succeeded: Program=%u, Bank=%u", ch, program, fallbackBank);
        return;
    }

    // Final fallback to Program 0 in Bank 0
    ESP_LOGW(TAG, "Ch%u: Program %u not found in Bank 0, falling back to Program 0", ch, program);
    state.program = 0;
    state.setBank(0);

    if (parser.hasPreset(0, 0)) {
        ESP_LOGI(TAG, "Ch%u: Final fallback succeeded: Program=0, Bank=0", ch);
    } else {
        ESP_LOGE(TAG, "Ch%u: Final fallback failed: No valid preset at Program=0, Bank=0", ch);
    }
}


void Synth::renderLR(float* sampleL, float* sampleR) {
    *sampleL = 0.0f;
    *sampleR = 0.0f;

    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice& v = voices[i];
        if (!v.active) continue;

        float smp = v.nextSample(); // Already includes pitch, envelope, etc.
        float vol = (*v.modVolume) * (*v.modExpression) * v.velocityVolume;

        *sampleL += smp * vol * v.panL;
        *sampleR += smp * vol * v.panR;
    }
}


Voice* Synth::findWeakestVoiceOnNote(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass) {
    Voice* weakest = nullptr;
    float weakestScore = FLT_MAX;
    int count = 0;

    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice& v = voices[i];
        if (v.active && v.channel == ch) { 
            if (v.exclusiveClass > 0 && v.exclusiveClass == exclusiveClass) {
                v.die();
                ESP_LOGI(TAG, "Killing voice %d", v.id);
            }
            if (v.note == note) {
                count++;
                v.updateScore();
                if (v.score < weakestScore) {
                    weakestScore = v.score;
                    weakest = &v;
                }
            }
        }
    }

    if (count >= MAX_VOICES_PER_NOTE && weakest && weakestScore < newScore)
        return weakest;

    return nullptr;
}

Voice* Synth::findWorstVoice() {
    Voice* worst = nullptr;
    float minScore = FLT_MAX;

    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i].updateScore();
        if (!voices[i].active || !voices[i].isRunning()) return &voices[i];

        if (voices[i].score < minScore) {
            minScore = voices[i].score;
            worst = &voices[i];
        }
    }

    return worst;
}

void Synth::updateScores() {
    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i].updateScore();
    }
}


void Synth::reset() {
    for (int ch = 0; ch < 16; ++ch) {
        channels[ch].reset();   
    }

    for (Voice& v : voices) {
        v.kill();
    }
}

void Synth::soundOff(uint8_t ch) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch) {
            v.kill();
        }
    }
}

void Synth::allNotesOff(uint8_t ch) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch) {
            if (*v.modSustain) {
                v.sustainHeld = true; // wait until pedal release
            } else {
                v.stop();
            }
        }
    }
}

void Synth::GMReset() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        auto& state = channels[ch];

        // Reset controllers
        state.volume = 1.0f;         // CC#7
        state.pan = 0.5f;            // CC#10
        state.expression = 1.0f;     // CC#11
        state.pitchBend = 0;         // Center
        state.pitchBendRange = 2;     // Default
        state.pitchBendFactor = 1.0f; // No pitch bend
        state.modWheel = 0.0f;
        state.reverbSend = 0.1f;
        state.chorusSend = 0.0f;

        // Reset program
        state.program = 0;

        // Select bank
        if (ch == 9) {
            state.setBank(128);  // Drum bank for channel 10
        } else {
            state.setBank(0);   // General MIDI bank for all other channels
        }
        
        allNotesOff(ch);
        soundOff(ch);
    }

    ESP_LOGI(TAG, "General MIDI Reset complete, Channel 10 set to drum bank\n");
}

bool Synth::handleSysEx(const uint8_t* data, size_t len) {
    if (len == 6 &&
        data[0] == 0xF0 &&
        data[1] == 0x7E &&
        //data[2] is device ID (can be 0x7F for "all devices") */
        data[3] == 0x09 &&
        data[4] == 0x01 &&
        data[5] == 0xF7) {
        
        GMReset();
        ESP_LOGI(TAG, "Received GM System On SysEx");
        return true;
    }
    return false;
}

void Synth::printState() {
    int activeCount = 0;
    for(int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) activeCount++;
        ESP_LOGD(TAG, "%d: id=%d seg=%s val=%.5f target=%.5f", i, voices[i].id, voices[i].ampEnv.getCurrentSegmentStr(), voices[i].ampEnv.getVal(),voices[i].ampEnv.getTarget() );
    }
    ESP_LOGI(TAG, "active %d/%d ", activeCount, MAX_VOICES);

}