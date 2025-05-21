#pragma once
#include <Arduino.h>
#include "config.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"
#include "biquad.h"

struct ChannelState {
    // Bank and Program
    
    alignas(16) float dryL[DMA_BUFFER_LEN] = {0};
    alignas(16) float dryR[DMA_BUFFER_LEN] = {0};

    alignas(16) float portaTime = 0.0f;  // CC#5 value, mapped from 0.0 to 1.0
    alignas(16) float volume = 1.0f;        // CC#7, 0.0–1.0
    alignas(16) float expression = 1.0f;    // CC#11, 0.0–1.0
    alignas(16) float pan = 0.5f;           // CC#10, 0.0 = left, 1.0 = right
    alignas(16) float modWheel = 0.0f;       // CC#1, 0.0–1.0
    // Effects
    alignas(16) float reverbSend = 0.0f;    // CC#91, 0.0–1.0
    alignas(16) float chorusSend = 0.0f;    // CC#93, 0.0–1.0
    alignas(16) float delaySend = 0.0f;     // CC#95, 0.0-1.0

    // Pitch bend
    alignas(16) float pitchBend = 0.0f;     // -1.0 to +1.0 (centered)
    alignas(16) float pitchBendRange = 2.0f; // default ±2 semitones
    alignas(16) float pitchBendFactor = 1.0f;  // derived from pitchBend and pitchBendRange

    // Pedals
    alignas(16) uint32_t sustainPedal = false;  // CC#64
    alignas(16) uint32_t portamento = false ; // CC#65

    alignas(16) uint32_t  bankMSB = 0;     // CC#0
    alignas(16) uint32_t  bankLSB = 0;     // CC#32
    alignas(16) uint32_t  program = 0;     // Program Change
    
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
	
	inline void reset() {
		volume = 1.0f;
		expression = 1.0f;
		pan = 0.0f;
		pitchBend = 0.0f;
        pitchBendFactor = 1.0f;
		pitchBendRange = 2.0f;
		sustainPedal = false;
		portaTime = 0.0f;
	}
 
 #ifdef ENABLE_CH_FILTER
	BiquadFilter filter;
    
    // Filter parameters
    float filterCutoff = 20000.0f;  // default no filtering
    float filterResonance = 0.707f;
	
	void updateFilter(float cutoff, float resonance) {
		filterCutoff = cutoff;
		filterResonance = resonance;
		filter.setFreqAndQ(cutoff, resonance);
	}
	
    void recalcFilter() {
        filter.setFreqAndQ(filterCutoff, filterResonance);
    }
#endif
};

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    SUSTAIN_LOOP = 2,
    PING_PONG_LOOP = 3
};

struct Voice {
    alignas(16) float WORD_ALIGNED_ATTR phase = 0.0f; 
    alignas(16) float velocityVolume = 1.0f;
    alignas(16) float panL = 1.0f, panR = 1.0f;
    alignas(16) float score = 0.0f;
    alignas(16) float reverbAmount = 0.0f;
    alignas(16) float chorusAmount = 0.0f;
    alignas(16) float expression = 0.0f;
    alignas(16) float volume = 1.0f;
    alignas(16) uint32_t exclusiveClass = 0;

#ifdef ENABLE_IN_VOICE_FILTERS
    BiquadFilter filter;
    alignas(16) float filterCutoff = 20000.0f;
    alignas(16) float filterResonance = 0.0f;
#endif

    float* modVolume = nullptr;
    float* modExpression = nullptr;
    float* modPitchBendFactor = nullptr;
    float* modPan = nullptr;
    float* modPortaTime = nullptr;
    uint32_t* modPortamento = nullptr;
    uint32_t* modSustain = nullptr;
    alignas(16) uint32_t noteHeld = false;

    alignas(16) float basePhaseIncrement = 1.0f;     // from note and tuning
    alignas(16) float targetPhaseIncrement = 0.0f;     // updated by pitch bend
    alignas(16) float currentPhaseIncrement = 0.0f;  // final value used in phase stepping

    // Portamento
    alignas(16) float portamentoFactor = 1.0f;         // current factor applied
    alignas(16) float targetPortamentoFactor = 1.0f;   // target to glide to
    alignas(16) float portamentoRate = 0.0005f;        // tweak as needed for glide speed (smaller = slower)  
    alignas(16) uint32_t portamentoActive = false;

    alignas(16) uint32_t  length = 0;
    alignas(16) uint32_t  loopStart = 0;
    alignas(16) uint32_t  loopEnd = 0;
    alignas(16) uint32_t  loopLength = 0;
    alignas(16) LoopType  loopType = NO_LOOP;
    alignas(16) uint32_t  active = false; 
    alignas(16) uint32_t  forward = true; // for ping-pong
    SampleHeader* sample = nullptr;
    Zone* zone = nullptr;
    int16_t* data;

    Adsr ampEnv;

    alignas(16) uint32_t note = 0;
    alignas(16) uint32_t velocity = 0;
    alignas(16) uint32_t channel = 0;

    void start(uint8_t channel, uint8_t note_, uint8_t velocity_,  SampleHeader* s_,  Zone* z_,  ChannelState* ch);
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

    inline void updatePan() {
        float pZone = zone ? zone->pan : 0.0f;
        float pMod  = modPan ? (*modPan * 2.0f - 1.0f) : 0.0f;

        float p = fclamp(pZone + pMod, -1.0f, 1.0f);  // clamp to [-1, 1]
        p = 0.5f * (p + 1.0f);                        // remap to [0, 1]

        // Equal-power pan could be used instead here if needed
        panL = 1.0f - p * 0.5f;
        panR = 0.5f + p * 0.5f;
    }


    inline void updatePitch() {
        // Recompute currentPhaseIncrement with updated pitch bend or portamento
        currentPhaseIncrement = basePhaseIncrement * (*modPitchBendFactor) * portamentoFactor;
    }
    
    void setPortamentoTarget(float targetNoteRatio) ;
    void printState();
    bool isLegato = false;
};

