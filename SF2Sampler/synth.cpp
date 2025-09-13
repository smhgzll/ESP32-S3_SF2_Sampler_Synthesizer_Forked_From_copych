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
 * File: synth.cpp
 * Purpose: SF2 synthesizer core logic
 * ----------------------------------------------------------------------------
 */

#include "synth.h"
#include "config.h"
#include <float.h>
#include <math.h>
#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include "TLVStorage.h"
#include <cstring>   // memset

#include <SdFat.h>
extern SdFs SD;   // main.cpp’de tanıml

#ifdef ENABLE_CH_FILTER_M
    #include "biquad2.h"
#endif

#ifdef ENABLE_CHORUS
    #include "fx_chorus.h"
    extern FxChorus chorus;
#endif

#ifdef ENABLE_REVERB
    #include "fx_reverb.h"
    extern FxReverb reverb;
#endif

#ifdef ENABLE_DELAY
    #include "fx_delay.h"
    extern FxDelay delayfx;
#endif

inline int countActiveVoicesFast(const Voice* voices, int max) {
    int c = 0;
    for (int i = 0; i < max; ++i) if (voices[i].active) ++c;
    return c;
}

static const char* TAG = "Synth";

// Basit, hızlı yumuşak sınırlayıcı (tanh benzeri)
static inline float soft_clip(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
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

    volume_scaler = 0.85f / sqrtf(MAX_VOICES);
}

bool Synth::begin() {
    if (loadSynthState()) return true;

    if (!parser.parse()) {
        ESP_LOGW(TAG, "No SF2 parsed. Auto-loading next SF2...");
        return loadNextSf2();
    }

    return true;
}

void Synth::noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (vel == 0) {
        noteOff(ch, note);
        return;
    }

#ifdef ENABLE_GUI
    block_gui();
#endif

    ChannelState* chan = &channels[ch];

    chan->activityIncrease(vel);

    bool isMono = chan->monoMode != ChannelState::Poly;
    bool retrig = chan->monoMode != ChannelState::MonoLegato;

    auto zones = parser.getZonesForNote(note, vel, chan->getBank(), chan->program);
    if (zones.empty()) return;

    chan->pushNote(note);

    if (isMono) {
        if (retrig) {
            // Kill all existing voices on this channel
            for (auto& v : voices)
                if (v.active && v.channel == ch)
                    v.die();

            // Start new voices for all zones
            for (auto& zone : zones) {
                if (!zone.sample) continue;
                float score = vel * DIV_127;
                Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass);
                if (v) v->startNew(ch, note, vel, zone, chan);
            }
        } else {
            // Legato: update pitch of ALL existing voices, or start new if none
            bool reused = false;
            for (Voice& v : voices) {
                if (  v.active && v.channel == ch) {
                    if (!v.noteHeld) {
                        v.die();
                    } else {
                        v.updatePitchOnly(note, chan);
                        reused = true;
                    }
                }
            }
            if (!reused) {
                // First note: start new voices
                for (auto& zone : zones) {
                    if (!zone.sample) continue;
                    float score = vel * DIV_127;
                    Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass);
                    if (v) v->startNew(ch, note, vel, zone, chan);
                }
            }
        }
    } else {
        // Polyphonic: start new voices normally
        for (auto& zone : zones) {
            if (!zone.sample) continue;
            float score = vel * DIV_127;
            Voice* v = allocateVoice(ch, note, score, zone.exclusiveClass);
            if (v) v->startNew(ch, note, vel, zone, chan);
        }
    }
    chan->portaCurrentNote = note;
}

void Synth::noteOff(uint8_t ch, uint8_t note) {
    if (ch >= 16) return;
    
#ifdef ENABLE_GUI
    block_gui();
#endif

    ChannelState* chan = &channels[ch];
    bool isMono = chan->monoMode != ChannelState::Poly;
    bool isRetrig = chan->monoMode == ChannelState::MonoRetrig;

    chan->removeNote(note);
    uint8_t nextNote = chan->topNote();

    for (Voice& v : voices) {
        if (!v.active || v.channel != ch) continue;

        if (isMono) {
            if (!chan->hasNotes()) {
                // No more held notes → kill all
                v.noteHeld = false;
                v.stop();
            } else if (isRetrig) {
                if (v.note == note) {
                    v.noteHeld = false;
                    v.die();
                }
            } else {
                // MonoLegato: switch pitch of ALL voices to next note
                if (v.note != nextNote) {
                    v.updatePitchOnly(nextNote, chan);
                }
            }
        } else {
            // Poly mode
            if (v.note == note) {
                v.noteHeld = false;
                v.stop();
            }
        }
    }
}


Voice*  __attribute__((always_inline))   Synth::allocateVoice(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass){
	Voice* v = findWeakestVoiceOnNote(ch, note, newScore, exclusiveClass);
	if (!v) v = findWorstVoice();
	return v;
}
 

void Synth::pitchBend(uint8_t ch, int value) {
    if (ch >= 16) return;

#ifdef ENABLE_GUI
    block_gui();
#endif

    float norm = (value - PITCH_BEND_CENTER) * DIV_8192;
    float semis = norm * channels[ch].pitchBendRange;

    channels[ch].pitchBend = norm;
    channels[ch].pitchBendFactor = exp2f(semis * DIV_12);

}


void Synth::controlChange(uint8_t ch, uint8_t ctrl, uint8_t val) {

    if (ch >= 16) return;

#ifdef ENABLE_GUI
    block_gui();
#endif

    auto& state = channels[ch];
    float fval = val * DIV_127;

    switch (ctrl) {
        case 0:  // Bank Select MSB
            state.wantBankMSB = val & 0x7F;
            break;
        case 32: // Bank Select LSB
            state.wantBankLSB = val & 0x7F;
            break;
        case 1:  // Mod Wheel
            state.modWheel = fval;  
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
                        if (v.active && v.channel == ch && !v.noteHeld) {
                            v.stop(); 
                        }
                    }
                }
            }
            break;
        case 65: // Portamento on/off
            {
                bool portamento = val >= 64;
                state.portamento = portamento;
            }
            break;
#ifdef ENABLE_CH_FILTER
        case 71: // Filter Resonance
            state.filterResonance = knob_tbl[val] * (FILTER_MAX_Q - 0.5f) + 0.5f;
            state.recalcFilter();
            break;
        case 74: // Filter Cutoff
            state.filterCutoff = knob_tbl[val] * CH_FILTER_MAX_FREQ + CH_FILTER_MIN_FREQ;
            state.recalcFilter();
            break;
#elif defined(ENABLE_CH_FILTER_M)
        case 71: // Filter Resonance        
            state.filterResonance = knob_tbl[val] * (FILTER_MAX_Q - 0.5f) + 0.5f;
            state.filterCoeffs = BiquadCalc::calcCoeffs(state.filterCutoff, state.filterResonance, BiquadCalc::LowPass);
            break;
        case 74: // Filter Cutoff
            state.filterCutoff = knob_tbl[val] * CH_FILTER_MAX_FREQ + CH_FILTER_MIN_FREQ;
            state.filterCoeffs = BiquadCalc::calcCoeffs(state.filterCutoff, state.filterResonance, BiquadCalc::LowPass);
            break;
#endif        

        case 72: // Release time modifier (64-centered)
            state.releaseModifier = knob_tbl[val] * 4.8072f; // 1.0 at val=64
            break;
        case 73: // Attack time modifier (64-centered)
            state.attackModifier = knob_tbl[val] * 4.8072f; // 1.0 at val=64
            break;
		case 84: // Portamento control
			state.portaCurrentNote = val;
            break;
        case 91: // Reverb Send
            state.reverbSend = fval;
            break;
        case 93: // Chorus Send
            state.chorusSend = fval;
            break; 
        case 95: // Delay Send
            state.delaySend = fval;
            break;
		case 99: state.nrpn.msb = val; state.rpn = {0x7F, 0x7F}; break; // NRPN MSB (disable RPN)
		case 98: state.nrpn.lsb = val; state.rpn = {0x7F, 0x7F}; break; // NRPN LSB
		case 101: state.rpn.msb = val; state.nrpn = {0x7F, 0x7F}; break; // RPN MSB (disable NRPN)
		case 100: state.rpn.lsb = val; state.nrpn = {0x7F, 0x7F}; break; // RPN LSB

		case 6: // Data Entry MSB
			if (state.rpn.msb == 0 && state.rpn.lsb == 0) {
				state.pitchBendRange = static_cast<float>(val);
			} else if (state.nrpn.msb == 0x01 && state.nrpn.lsb == 0x10) { // nrpn 0x01 0x10 to switch mono/poly ()
				switch (val) {
					case 0: setChannelMode(ch, ChannelState::MonoLegato); break;
					case 1: setChannelMode(ch, ChannelState::MonoRetrig); break;
                    default:
					case 2: setChannelMode(ch, ChannelState::Poly);
				}
			}
			break;

		case 38: // Data Entry LSB
			// Optional: support for LSB pitch bend range or future NRPNs
			break;
        case 120: // All Sound Off
            soundOff(ch);
            break;
        case 121: // Reset All Controllers
            state.reset(); // Reset modulation values for channel
            break;
        case 123: // All Notes Off
            allNotesOff(ch);
            break;
		case 126: // Mono Mode On
			// val > 0 = MonoLegato with val voices, val == 0 = MonoRetrig
            setChannelMode(ch, (val > 0) ? ChannelState::MonoLegato : ChannelState::MonoRetrig);
			ESP_LOGI(TAG, "CC126: Channel %u → %s", ch+1, (val > 0 ? "MonoLegato" : "MonoRetrig"));
			break;
		case 127: // Poly Mode On
            setChannelMode(ch, ChannelState::Poly);
			ESP_LOGI(TAG, "CC127: Channel %u → Poly", ch+1);
			break;
        default:
            break;
    }
}

void Synth::applyBankProgram(uint8_t ch) {
    if (ch >= 16) return;
	
#ifdef ENABLE_GUI
    block_gui();
#endif

    auto& state = channels[ch];
    state.clearNoteStack(); 
    const uint8_t program = state.wantProgram;
    const uint16_t bank   = state.getWantBank();

    // === Detect Drum Channel ===
    if (ch == 9 || state.wantBankMSB == 127 || state.wantBankMSB == 120 || bank == 128) {
        state.isDrum = true;
    } else {
        state.isDrum = false;
    }

    // === Try Requested Bank ===
    if (parser.hasPreset(bank, program)) {
        state.program = program;
        state.setBank(bank);
        ESP_LOGD(TAG, "Ch%u: Program=%u, Bank=%u (%s)", ch+1, program, bank, state.isDrum ? "Drum" : "Melodic");
        return;
    }

    // === Melodic fallback: try Bank 0 ===
    if (!state.isDrum && parser.hasPreset(0, program)) {
        state.program = program;
        state.setBank(0);
        ESP_LOGW(TAG, "Ch%u: Bank %u not found, fallback to Bank 0 (Program=%u)", ch+1, bank, program);
        return;
    }

    // === Final fallback: Program 0, Bank depends on drum status ===
    const uint16_t fallbackBank = state.isDrum ? 128 : 0;
    if (parser.hasPreset(fallbackBank, 0)) {
        state.program = 0;
        state.setBank(fallbackBank);
        ESP_LOGW(TAG, "Ch%u: Fallback to Program=0, Bank=%u (%s)", ch+1, fallbackBank, state.isDrum ? "Drum" : "Melodic");
    } else {
        ESP_LOGE(TAG, "Ch%u: No valid preset for Program=%u in any known bank", ch+1, program);
    }
}



void Synth::programChange(uint8_t ch, uint8_t program) {
    if (ch >= 16) return;

    auto& state = channels[ch];
    state.wantProgram = program & 0x7F;

    applyBankProgram(ch);
}



// Inline filter processing to reduce function call overhead
#define PROCESS_FILTER_LR(flt, inL, inR) { \
    float tmpL = inL, tmpR = inR;          \
    flt.processLR(&tmpL, &tmpR);           \
    inL = tmpL; inR = tmpR;                \
}


void   __attribute__((hot,always_inline)) IRAM_ATTR Synth::renderLRBlock(float* outL, float* outR) {
     float dryL[DMA_BUFFER_LEN] = {0};
     float dryR[DMA_BUFFER_LEN] = {0};
#ifdef ENABLE_CHORUS
     float choL[DMA_BUFFER_LEN] = {0}, choR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_REVERB
     float revL[DMA_BUFFER_LEN] = {0}, revR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_DELAY
     float delL[DMA_BUFFER_LEN] = {0}, delR[DMA_BUFFER_LEN] = {0};
#endif

#ifdef ENABLE_CH_FILTER
    // Clear channel dry buffers before mixing
    for (int ch = 0; ch < 16; ++ch) {
        memset(channels[ch].dryL, 0, sizeof(float)*DMA_BUFFER_LEN);
        memset(channels[ch].dryR, 0, sizeof(float)*DMA_BUFFER_LEN);
    }
#endif

    for (int v = 0; v < MAX_VOICES; ++v) {
        Voice& voice = voices[v];
        if (!voice.active) continue;

        // Çifte gain’i önlemek için sadece pan + global scaler
        float volL = volume_scaler * voice.panL;
        float volR = volume_scaler * voice.panR;

#ifdef ENABLE_CHORUS
        float cAmt = voice.chorusAmount;
#endif
#ifdef ENABLE_REVERB
        float rAmt = voice.reverbAmount;
#endif
#ifdef ENABLE_DELAY
        float dAmt = channels[voice.channel].delaySend;
#endif

#ifdef ENABLE_CH_FILTER
        // Mix into channel dry buffers for filtering
        float* dryLp = channels[voice.channel].dryL;
        float* dryRp = channels[voice.channel].dryR;
#else
        // Mix directly into global dry buffers
        float* dryLp = dryL;
        float* dryRp = dryR;
#endif

#ifdef ENABLE_CHORUS
        float* choLp = choL;
        float* choRp = choR;
#endif
#ifdef ENABLE_REVERB
        float* revLp = revL;
        float* revRp = revR;
#endif
#ifdef ENABLE_DELAY
        float* delLp = delL;
        float* delRp = delR;
#endif

        for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
            float smp = voice.nextSample();   // ZATEN vel * volume * expression * env içerir

            float l = smp * volL;
            float r = smp * volR;

            dryLp[i] += l;
            dryRp[i] += r;

#ifdef ENABLE_CHORUS
            float lCho = l * cAmt;
            float rCho = r * cAmt;
            choLp[i] += lCho;
            choRp[i] += rCho;
#endif
#ifdef ENABLE_REVERB
            float lRev = l, rRev = r;
#ifdef ENABLE_CHORUS
            lRev += lCho; rRev += rCho;
#endif
            revLp[i] += lRev * rAmt;
            revRp[i] += rRev * rAmt;
#endif
#ifdef ENABLE_DELAY
            float lDel = l, rDel = r;
#ifdef ENABLE_CHORUS
            lDel += lCho; rDel += rCho;
#endif
            delLp[i] += lDel * dAmt;
            delRp[i] += rDel * dAmt;
#endif
        }
    }

#ifdef ENABLE_CH_FILTER
    // Process filters per channel and accumulate to global dry buffers
    for (int ch = 0; ch < 16; ++ch) {
        ChannelState& chan = channels[ch];
        float* bufL = chan.dryL;
        float* bufR = chan.dryR;

        for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
            float l = bufL[i];
            float r = bufR[i];
            PROCESS_FILTER_LR(chan.filter, l, r);
            dryL[i] += l;
            dryR[i] += r;
        }
    }
#endif

#ifdef ENABLE_CHORUS
    chorus.processBlock(choL, choR);
#endif
#ifdef ENABLE_DELAY
    delayfx.ProcessBlock(delL, delR);
#endif
#ifdef ENABLE_REVERB
    reverb.processBlock(revL, revR);
#endif

    // --- MASTER HEADROOM + SOFT LIMITER ---
    const float MASTER_GAIN = 0.30f;  // 0.25–0.35 arası denenebilir

    for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
        float l = dryL[i];
        float r = dryR[i];
#ifdef ENABLE_CHORUS
        l += choL[i]; r += choR[i];
#endif
#ifdef ENABLE_REVERB
        l += revL[i]; r += revR[i];
#endif
#ifdef ENABLE_DELAY
        l += delL[i]; r += delR[i];
#endif

        l *= MASTER_GAIN;
        r *= MASTER_GAIN;

        l = soft_clip(l);
        r = soft_clip(r);

        // Emniyet clamp
        if (l > 0.999f) l = 0.999f; else if (l < -0.999f) l = -0.999f;
        if (r > 0.999f) r = 0.999f; else if (r < -0.999f) r = -0.999f;

        outL[i] = l;
        outR[i] = r;
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
                v.die(); // Kill voices of the same class
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

    if (count >= MAX_VOICES_PER_NOTE && weakest)// && weakestScore < newScore)
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
    // Audio thread dışı: yalnızca skor güncelle
    for (Voice& v : voices) {
        v.updateScore();
        if (!v.active) continue;
        v.updatePitch();
        v.updatePitchFactors();
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
               // v.sustainHeld = true; // wait until pedal release
            } else {
                v.stop();
            }
        }
    }
}

void Synth::GMReset() {
	
#ifdef ENABLE_GUI
    block_gui();
#endif

    for (uint8_t ch = 0; ch < 16; ++ch) {
        auto& state = channels[ch];
        state.reset();

        // Set default program and bank requests
        state.wantProgram = 0;
        state.wantBankMSB = (ch == 9) ? 1 : 0; // Bank 128
        state.wantBankLSB = 0;

        applyBankProgram(ch);

        allNotesOff(ch);
        soundOff(ch);
    }

    ESP_LOGI(TAG, "General MIDI Reset complete, Channel 10 locked to drum bank.");
}

bool Synth::handleSysEx(const uint8_t* data, size_t len) {
	
#ifdef ENABLE_GUI
    block_gui();
#endif

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
	
	// XG System On (F0 43 1x 4C 00 00 7E 00 F7)
    if (len == 9 &&
        data[0] == 0xF0 &&
        data[1] == 0x43 &&
        // data[2] == 0x10 && 	// is device ID, ignored
        data[3] == 0x4C &&
        data[4] == 0x00 &&
        data[5] == 0x00 &&
        data[6] == 0x7E &&
        data[7] == 0x00 &&
        data[8] == 0xF7) {

        GMReset(); // we actually fully reset the synth
        ESP_LOGI(TAG, "Received XG System On SysEx");
        return true;
    }
    

    if (len == 9 &&
    data[0] == 0xF0 &&
    data[1] == 0x43 &&
    // data[2] == 0x10 //(device ID)
    data[3] == 0x4C &&
    data[4] == 0x08 &&
    data[8] == 0xF7) {
        uint8_t part = data[5]; // MIDI channel 0–15
        uint8_t param = data[6]; 
        uint8_t val = data[7]; // 0 = GM, 1 = Drum
        if (part < 16) {
            auto& state = channels[part];
            if (param == 0x05) {
                bool mono = (val == 0x00);
                setChannelMode(part, mono ? ChannelState::MonoLegato : ChannelState::Poly );
                ESP_LOGI(TAG, "Received XG Mono/Poly SysEx: Part %u → %s", part + 1, mono ? "Mono" : "Poly");
                return true;
            } else if (param == 0x08) {
                state.tuningSemitones = val - 64.0f;
                ESP_LOGI(TAG, "Received XG part note shift SysEx: Part %u → %d", part + 1, (int)(val-64.0f));
                return true;
            } else if (param == 0x07) {
                if (val == 0) {
                    state.wantBankMSB = 0;
                    state.wantBankLSB = 0;
                    ESP_LOGI(TAG, "XG: Ch%u set to General MIDI (Bank 0)", part+1);
                } else {
                    state.wantBankMSB = 1;  // Bank 128
                    state.wantBankLSB = 0;
                    ESP_LOGI(TAG, "XG: Ch%u set to Drum Kit (Bank 128)", part+1);
                }

                applyBankProgram(part);
                return true;
            }
        }
    }
	
    return false;
     
}

fs::FS* Synth::getFileSystem() {
    switch (fsType) {
        case FileSystemType::LITTLEFS: return &LittleFS;
        case FileSystemType::SD:
        return nullptr;   // SdFat yolunu kullanacağız
        default: return nullptr;
    }
}

void Synth::scanSf2Files() {
    sf2Files.clear();

    FsFile dir;
    if (!dir.open(SF2_PATH, O_RDONLY)) {
        ESP_LOGE("Synth", "Can't open directory %s", SF2_PATH);
        currentFileIndex = -1;
        return;
    }

    for (FsFile e; e.openNext(&dir, O_RDONLY); ) {
        if (e.isDir()) { e.close(); continue; }

        char name[128] = {0};
        e.getName(name, sizeof(name));   // sadece dosya adı (klasörsüz)
        String s = String(name);
        String lower = s; lower.toLowerCase();
        if (lower.endsWith(".sf2")) {
            sf2Files.push_back(s);       // sadece adı saklıyoruz
        }
        e.close();
    }

    dir.close();
    currentFileIndex = sf2Files.empty() ? -1 : 0;
}

bool Synth::loadSf2File(const char* filename) {
    String fullPath = String(SF2_PATH);
    if (filename[0] != '/') fullPath += '/';
    fullPath += filename;

    ESP_LOGI("Synth", "\n\nLoading SF2: %s\n\n", fullPath.c_str());

    SF2Parser tempParser(fullPath.c_str());
    if (!tempParser.parse()) {
        ESP_LOGE("Synth", "Failed to parse %s", fullPath.c_str());
        return false;
    }

    parser = std::move(tempParser);
    reset();
    GMReset();
    return true;
}

bool Synth::loadNextSf2() {
    parser.clear();
    if (sf2Files.empty()) {
        scanSf2Files();
        if (sf2Files.empty()) {
            ESP_LOGW("Synth", "No .sf2 files found");
            return false;
        }
    }

    currentFileIndex = (currentFileIndex + 1) % sf2Files.size();
    return loadSf2File(sf2Files[currentFileIndex].c_str());
}

void Synth::setChannelMode(uint8_t ch, ChannelState::MonoMode mode) {
    channels[ch].monoMode = mode;
    channels[ch].clearNoteStack();
}


void Synth::printState() {
    int activeCount = 0;
    for(int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) activeCount++;
        ESP_LOGD(TAG, "%d: id=%d seg=%s val=%.5f target=%.5f", i, voices[i].id, voices[i].ampEnv.getCurrentSegmentStr(), voices[i].ampEnv.getVal(),voices[i].ampEnv.getTarget() );
    }
    ESP_LOGI(TAG, "active %d/%d ", activeCount, MAX_VOICES);

}

void Synth::updateActivity() {
    for(auto& chan : channels) {
        chan.activityUpdate();
    }
}

void Synth::getActivityString(char str[49]) {
    const uint8_t n = 7; 
    const uint8_t escape =  0xE2;
    const uint8_t msb =     0x96;
    const uint8_t lsb =     0x81;
    str[0] = '\0';
    for (int i = 0; i < 16; i++) {
        str[i*3] = escape;
        uint8_t index = channels[i].activity * (float)n;
        str[i*3 + 1] = msb ;
        str[i*3 + 2] = lsb + index;
    }
    str[48] = '\0';  
}

bool Synth::loadSf2ByIndex(int index) {
    if(index >= 0 && index < sf2Files.size()) {
        currentFileIndex = index;
        return loadSf2File(sf2Files[index].c_str());
    }
    return false;
}

bool Synth::saveSynthState(const char* path) {


    fs::FS* fs = &SD_MMC;
    if (!fs) return false;
    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;

    // Store current SF2 filename
    if (!currentSf2Path.isEmpty()) {
        writeTLV(f, PARAM_SF2_FILENAME, currentSf2Path.c_str(), currentSf2Path.length() + 1);
    }

    uint8_t fsTypeByte = static_cast<uint8_t>(getCurrentFsType());
    writeTLV(f, PARAM_SF2_FS_TYPE, &fsTypeByte, 1);


    // Channels
    for (int ch = 0; ch < 16; ++ch) {
        uint8_t data[3] = {
            (uint8_t)channels[ch].wantBankMSB,
            (uint8_t)channels[ch].wantBankLSB,
            (uint8_t)channels[ch].wantProgram
        };
        writeTLV(f, PARAM_CHANNEL(ch), data, 3);
    }

#ifdef ENABLE_REVERB
    float rtime = reverb.getTime();
    float rdamp = reverb.getDamping();
    writeTLV(f, PARAM_REVERB_TIME, &rtime, sizeof(rtime));
    writeTLV(f, PARAM_REVERB_DAMP, &rdamp, sizeof(rdamp));
#endif
#ifdef ENABLE_DELAY
    float dtime = delayfx.getDelayTime();
    writeTLV(f, PARAM_DELAY_TIME, &dtime, sizeof(dtime));
#endif
#ifdef ENABLE_CHORUS
    float cdepth = chorus.getDepth();
    writeTLV(f, PARAM_CHORUS_DEPTH, &cdepth, sizeof(cdepth));
#endif

    f.close();
    return true;
}

bool Synth::loadSynthState(const char* path) {
    fs::FS* fs = &SD_MMC;
    if (!fs) return false;
    File f = fs->open(path, FILE_READ);
    if (!f) return false;

    auto map = readTLV(f);
    f.close();

    for (int ch = 0; ch < 16; ++ch) {
        auto it = map.find(PARAM_CHANNEL(ch));
        if (it != map.end() && it->second.len == 3) {
            auto& b = it->second.data;
            channels[ch].wantBankMSB = b[0];
            channels[ch].wantBankLSB = b[1];
            channels[ch].wantProgram = b[2];
            applyBankProgram(ch);
        }
    }

#ifdef ENABLE_REVERB
    if (auto it = map.find(PARAM_REVERB_TIME); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); reverb.setTime(v);
    }
    if (auto it = map.find(PARAM_REVERB_DAMP); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); reverb.setDamping(v);
    }
#endif
#ifdef ENABLE_DELAY
    if (auto it = map.find(PARAM_DELAY_TIME); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); delayfx.setDelayTime(v);
    }
#endif
#ifdef ENABLE_CHORUS
    if (auto it = map.find(PARAM_CHORUS_DEPTH); it != map.end() && it->second.len == 4) {
        float v; memcpy(&v, it->second.data.data(), 4); chorus.setDepth(v);
    }
#endif

    FileSystemType loadedFsType = FileSystemType::LITTLEFS; // default

    if (auto it = map.find(PARAM_SF2_FS_TYPE); it != map.end() && it->second.len == 1) {
        loadedFsType = static_cast<FileSystemType>(it->second.data[0]);
    }


    if (auto it = map.find(PARAM_SF2_FILENAME); it != map.end() && it->second.len > 0) {
        const char* name = (const char*)it->second.data.data();
        fs::FS* fs = (loadedFsType == FileSystemType::SD)
           ? static_cast<fs::FS*>(&SD_MMC)
           : static_cast<fs::FS*>(&LittleFS);

        if (fs->exists(name)) {
            setFileSystem(loadedFsType);
            loadSf2File(name);  // full path relative to chosen FS
        } else {
            ESP_LOGW(TAG, "Saved SF2 not found: %s (FS=%s)", name,
                    loadedFsType == FileSystemType::SD ? "SD" : "LFS");
        }
    }
    return true;
}
