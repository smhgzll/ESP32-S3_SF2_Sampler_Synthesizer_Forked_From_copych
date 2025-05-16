#pragma once
#include "voice.h"
#include "SF2Parser.h"

struct RPNState {
    uint8_t msb = 127;
    uint8_t lsb = 127;
};

enum class FileSystemType {
    LITTLEFS,
    SD
};

class Synth {
public:
    Synth(SF2Parser& parserRef);
    bool begin();
    void noteOn(uint8_t ch, uint8_t note, uint8_t vel);
    void noteOff(uint8_t ch, uint8_t note);
    void controlChange(uint8_t ch, uint8_t control, uint8_t value);
    void allNotesOff(uint8_t ch);
    bool handleSysEx(const uint8_t* data, size_t len); 
    void soundOff(uint8_t ch);
    void reset();
    void programChange(uint8_t channel, uint8_t program);
    void pitchBend(uint8_t ch, int value);
    void updateScores();
    void printState();
    void renderLR(float* sampleL, float* sampleR);
    void GMReset(); 
    const ChannelState& getChannelState(uint8_t channel) const { return channels[channel]; }
    void setFileSystem(FileSystemType type) { fsType = type; }
    bool loadSf2File(const char* path);
    bool loadNextSf2();
    void scanSf2Files();
    void renderLRBlock(float*, float*);
    
private:
    
    int currentFileIndex = -1;
    float pitchBendRatio(int value);

    Voice* allocateVoice(uint8_t ch, uint8_t note, float newScore);
    Voice* findWeakestVoiceOnNote(uint8_t ch, uint8_t note, float newScore, uint32_t exclusiveClass);
    Voice* findWorstVoice();

    Voice voices[MAX_VOICES];

    SF2Parser& parser;
    
    ChannelState channels[16];
    RPNState rpnState[16]; // One per MIDI channel

    fs::FS* getFileSystem() ;

    FileSystemType fsType = FileSystemType::LITTLEFS;  // default
    std::vector<String> sf2Files;
};
