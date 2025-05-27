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


static const char* TAG = "Synth";

float pitchBendRatioFromValue(int bendValue, float semitoneRange) {
    float norm = (bendValue - PITCH_BEND_CENTER) * DIV_8192; // Normalize to [-1, 1]
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

    volume_scaler = 1.0f / sqrtf(MAX_VOICES);
}

bool Synth::begin() {
    if (!parser.parse()) {
        ESP_LOGW(TAG, "No SF2 parsed. Auto-loading next SF2...");
        return loadNextSf2();
    }
    return true;
}


void Synth::noteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (ch >= 16 || vel == 0) return;

    ChannelState* chan = &channels[ch];
    auto zones = parser.getZonesForNote(note, vel, chan->getBank(), chan->program);
    if (zones.empty()) return;

    for (auto& zone : zones) {
        if (!zone.sample) continue;

        uint32_t exclusiveClass = zone.exclusiveClass;
        float score = vel * DIV_127;

        Voice* v = findWeakestVoiceOnNote(ch, note, score, exclusiveClass);
        if (!v) v = findWorstVoice();

        if (v) {
            v->start(ch, note, vel, zone.sample, zone, chan);
        }
    }
}



void Synth::noteOff(uint8_t ch, uint8_t note) {
    if (ch >= 16) return;
    for (Voice& v : voices) {
        if (v.active && v.channel == ch && v.note == note) {
            {
                v.noteHeld = false;
                v.stop();
            }
        }
    }
}


void Synth::pitchBend(uint8_t ch, int value) {
    if (ch >= 16) return;

    float norm = (value - PITCH_BEND_CENTER) * DIV_8192;
    float semis = norm * channels[ch].pitchBendRange;

    channels[ch].pitchBend = norm;
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

        case 91: // Reverb Send
            state.reverbSend = fval;
            break;
        case 93: // Chorus Send
            state.chorusSend = fval;
            break; 
        case 95: // Delay Send
            state.delaySend = fval;
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

    if ((state.bankMSB == 120 || state.bankMSB == 127) && !parser.hasPreset(bank, program) ) {
        state.setBank(128);
        if (parser.hasPreset(bank, program)) {
            ESP_LOGI(TAG, "Ch%u: Fallback to Program=%u, Bank=%u", ch, program, bank);
            return;
        } else if (parser.hasPreset(128, 0)) {
            state.program = 0;
            ESP_LOGI(TAG, "Ch%u: Fallback to Program=%u, Bank=%u", ch, program, bank);
            return;
        }
    }
    // Fallback to Bank 0, same program
    ESP_LOGW(TAG, "Ch%u: Program %u not found in Bank %u, falling back to Bank 0", ch, program, bank);
    uint16_t fallbackBank = (bank & 0x7F00);  
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



// Inline filter processing to reduce function call overhead
#define PROCESS_FILTER_LR(flt, inL, inR) { \
    float tmpL = inL, tmpR = inR;         \
    flt.processLR(&tmpL, &tmpR);         \
    inL = tmpL; inR = tmpR;              \
}


void Synth::renderLRBlock(float* outL, float* outR) {
    alignas(16) float dryL[DMA_BUFFER_LEN] = {0};
    alignas(16) float dryR[DMA_BUFFER_LEN] = {0};
#ifdef ENABLE_CHORUS
    alignas(16) float choL[DMA_BUFFER_LEN] = {0}, choR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_REVERB
    alignas(16) float revL[DMA_BUFFER_LEN] = {0}, revR[DMA_BUFFER_LEN] = {0};
#endif
#ifdef ENABLE_DELAY
    alignas(16) float delL[DMA_BUFFER_LEN] = {0}, delR[DMA_BUFFER_LEN] = {0};
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

        float volL = volume_scaler * (*voice.modVolume) * (*voice.modExpression) * voice.velocityVolume * voice.panL;
        float volR = volume_scaler * (*voice.modVolume) * (*voice.modExpression) * voice.velocityVolume * voice.panR;

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
            float smp = voice.nextSample();

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

    for (int i = 0; i < DMA_BUFFER_LEN; ++i) {
        outL[i] = dryL[i];
        outR[i] = dryR[i];
#ifdef ENABLE_CHORUS
        outL[i] += choL[i]; outR[i] += choR[i];
#endif
#ifdef ENABLE_REVERB
        outL[i] += revL[i]; outR[i] += revR[i];
#endif
#ifdef ENABLE_DELAY
        outL[i] += delL[i]; outR[i] += delR[i];
#endif
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
    for (uint8_t ch = 0; ch < 16; ch++) {
        auto& state = channels[ch];

        // Reset controllers
        state.reset();

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
    if (len==9 && 
        data[0] == 0xF0 && 
        data[1] == 0x43 && 
        data[2] == 0x10 &&
        data[3] == 0x4C && 
        data[4] == 0x08 && 
        data[5] == 0x00 &&
        data[8] == 0xF7) {

        uint8_t part = data[6];     // MIDI channel (0–15)
        uint8_t mode = data[7];     // 0 = normal, 1 = drum

        if (part < 16) {
            if (mode == 0) {
                channels[part].setBank(0);
                ESP_LOGI(TAG, "Set MIDI channel %d to General MIDI", part);
            } else {
                channels[part].setBank(128);
                ESP_LOGI(TAG, "Set MIDI channel %d to Drum Kit", part);
            }
        }
    }

    return false;
}

fs::FS* Synth::getFileSystem() {
    switch (fsType) {
        case FileSystemType::LITTLEFS: return &LittleFS;
        case FileSystemType::SD: return &SD_MMC;
        default: return nullptr;
    }
}

void Synth::scanSf2Files() {
    sf2Files.clear();
    fs::FS* fs = getFileSystem();
    if (!fs) return;

    File dir = fs->open(SF2_PATH);
    if (!dir || !dir.isDirectory()) {
        ESP_LOGE("Synth", "Can't open directory %s", SF2_PATH);
        return;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        String lower = name;
        lower.toLowerCase();
        if (!entry.isDirectory() && lower.endsWith(".sf2")) {
            sf2Files.push_back(name);
        }
    }

    dir.close();
    currentFileIndex = -1;
}

bool Synth::loadSf2File(const char* filename) {
    
    parser.clear();
    ESP_LOGI(TAG, "\n\nFree heap: %u, PSRAM: %u\n\n", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    fs::FS* fs = getFileSystem();
    if (!fs) {
        ESP_LOGE("Synth", "Filesystem not initialized");
        return false;
    }

    reset();

    String fullPath = String(SF2_PATH) + filename;
    ESP_LOGI("Synth", "\n\nLoading SF2: %s\n\n", fullPath.c_str());

    SF2Parser tempParser(fullPath.c_str(), fs);
    if (!tempParser.parse()) {
        ESP_LOGE("Synth", "Failed to parse %s", fullPath.c_str());
        return false;
    }

    parser = std::move(tempParser);

    reset();      // Stop voices and reset channels
    GMReset();    // Apply GM defaults
    return true;
}

bool Synth::loadNextSf2() {
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


void Synth::printState() {
    int activeCount = 0;
    for(int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) activeCount++;
        ESP_LOGD(TAG, "%d: id=%d seg=%s val=%.5f target=%.5f", i, voices[i].id, voices[i].ampEnv.getCurrentSegmentStr(), voices[i].ampEnv.getVal(),voices[i].ampEnv.getTarget() );
    }
    ESP_LOGI(TAG, "active %d/%d ", activeCount, MAX_VOICES);

}