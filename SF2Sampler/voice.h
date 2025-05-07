#pragma once
#include <Arduino.h>
#include "config.h"
#include "misc.h"
#include "SF2Parser.h"
#include "adsr.h"


struct ChannelState {
    // Bank and Program
    float portaTime = 0.0f;  // CC#5 value, mapped from 0.0 to 1.0
    float volume = 1.0f;        // CC#7, 0.0–1.0
    float expression = 1.0f;    // CC#11, 0.0–1.0
    float pan = 0.5f;           // CC#10, 0.0 = left, 1.0 = right
    float modWheel = 0.0f;       // CC#1, 0.0–1.0
    // Effects
    float reverbSend = 0.0f;    // CC#91, 0.0–1.0
    float chorusSend = 0.0f;    // CC#93, 0.0–1.0

    // Pitch bend
    float pitchBend = 0.0f;     // -1.0 to +1.0 (centered)
    float pitchBendRange = 2.0f; // default ±2 semitones
    float pitchBendFactor = 1.0f;  // derived from pitchBend and pitchBendRange

    // Pedals
    bool sustainPedal = false;  // CC#64
    bool portamento = false ; // CC#65

    uint8_t bankMSB = 0;     // CC#0
    uint8_t bankLSB = 0;     // CC#32
    uint8_t program = 0;     // Program Change
    
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
		pitchBend = 8192;
        pitchBendFactor = 1.0f;
		pitchBendRange = 2.0f;
		sustainPedal = false;
		portaTime = 0.0f;
	}
};

enum LoopType {
    NO_LOOP = 0,
    FORWARD_LOOP = 1,
    SUSTAIN_LOOP = 2,
    PING_PONG_LOOP = 3
};

struct Voice {
    float WORD_ALIGNED_ATTR phase = 0.0f; 
    float velocityVolume = 1.0f;
    float panL = 1.0f, panR = 1.0f;
    float score = 0.0f;
    float reverbAmount = 0.0f;
    float chorusAmount = 0.0f;
    float expression = 0.0f;
    float volume = 1.0f;
    uint32_t exclusiveClass = 0;

    float* modVolume = nullptr;
    float* modExpression = nullptr;
    float* modPitchBendFactor = nullptr;
    float* modPan = nullptr;
    float* modPortaTime = nullptr;
    bool* modPortamento = nullptr;
    bool* modSustain = nullptr;

    float basePhaseIncrement = 1.0f;     // from note and tuning
    float targetPhaseIncrement = 0.0f;     // updated by pitch bend
    float currentPhaseIncrement = 0.0f;  // final value used in phase stepping

    // Portamento
    float portamentoFactor = 1.0f;         // current factor applied
    float targetPortamentoFactor = 1.0f;   // target to glide to
    float portamentoRate = 0.0005f;        // tweak as needed for glide speed (smaller = slower) 
    bool sustainHeld = false;
    bool portamentoActive = false;

    uint32_t WORD_ALIGNED_ATTR length = 0;
    uint32_t WORD_ALIGNED_ATTR loopStart = 0;
    uint32_t WORD_ALIGNED_ATTR loopEnd = 0;
    uint32_t WORD_ALIGNED_ATTR loopLength = 0;
    LoopType WORD_ALIGNED_ATTR loopType = NO_LOOP;
    uint32_t WORD_ALIGNED_ATTR active = false; 
    uint32_t WORD_ALIGNED_ATTR forward = true; // for ping-pong
    SampleHeader* sample = nullptr;
    Zone* zone = nullptr;
    int16_t* data;

    Adsr ampEnv;

    uint8_t note = 0;
    uint8_t velocity = 0;
    uint8_t channel = 0;

    void start(uint8_t channel, uint8_t note_, uint8_t velocity_,  SampleHeader* s_,  Zone* z_,  ChannelState* ch);
    void stop();
    void kill();
    void die();
    bool isRunning() const;
    float nextSample();
    void init();
    static int usage; // = 0
    int id = 0;
    inline void setFXSend(float reverb, float chorus) {
        reverbAmount = reverb;
        chorusAmount = chorus;
    }
    void updateScore();
    void updatePortamento();

    inline void updatePan() {
        float zonePan = 0.0f;
        if (zone) zonePan = constrain(zone->pan, -1.0f, 1.0f); // from Generator
        float chanPan = (modPan ? (*modPan * 2.0f - 1.0f) : 0.0f); // CC10 → [-1,1]
        float combinedPan = constrain(zonePan + chanPan, -1.0f, 1.0f);
        float pan = 0.5f * (combinedPan + 1.0f); // back to 0.0–1.0

        panL = 1.0f - pan * 0.5f;
        panR = 1.0f + pan * 0.5f;
    }

    inline void updatePitch() {
        // Recompute currentPhaseIncrement with updated pitch bend or portamento
        currentPhaseIncrement = basePhaseIncrement * (*modPitchBendFactor) * portamentoFactor;
    }
    
    void setPortamentoTarget(float targetNoteRatio) ;
    void printState();
    bool isLegato = false;
};

