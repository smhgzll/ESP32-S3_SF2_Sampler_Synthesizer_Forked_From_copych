#pragma once

#include "config.h"
#ifdef ENABLE_GUI

#include <U8g2lib.h>
#include "button.h"
#include "encoder.h"
#include "synth.h"

class TextGUI {
public:
    uint8_t encA;
    uint8_t encB;
    uint8_t btnState;

    enum MenuPage {
        PAGE_MAIN,
        PAGE_PROGRAM,
        PAGE_CHANNEL,
        PAGE_SF2,
        PAGE_COUNT
    };

    TextGUI(Synth& synthRef);
    void begin();
    void process();
    void draw();

private:
    Synth& synth;
    MuxEncoder encoder;
    MuxButton button;
    U8_OBJECT display;
    uint32_t decimator = 0;

    MenuPage currentPage = PAGE_MAIN;
    uint8_t currentChannel = 0;
    int16_t menuPosition = 0;
    bool needsDisplayUpdate = true;

    void onEncoderTurn(int direction);
    void onButtonEvent(MuxButton::btnEvents evt);
    void changeProgram(int direction);
    void updateDisplay();
    void renderDisplay();
    int updateUiBlock();

};

#endif