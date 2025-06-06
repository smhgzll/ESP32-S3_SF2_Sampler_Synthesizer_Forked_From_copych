#include "config.h"
#ifdef ENABLE_GUI
#include "TextGUI.h"

TextGUI::TextGUI(Synth& synthRef) : 
    synth(synthRef),
    display(U8_ROTATE, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ DISPLAY_SCL, /* data=*/ DISPLAY_SDA),
    encA(0),
    encB(0),
    btnState(0)
{
  ESP_LOGI("GUI", "Initialized display");
}

void TextGUI::begin() {
    // Initialize display with proper power sequence
    display.begin();
    display.setPowerSave(0);  // Wake up display
    display.setContrast(255); // Max contrast
    //display.setFont(u8g2_font_6x10_tf);
    display.setFont(u8g2_font_6x12_m_symbols);
    display.enableUTF8Print();
    display.setFontRefHeightExtendedText();
    display.setDrawColor(2);
    display.setFontPosTop();
    display.setFontDirection(0);
    
    // Clear and refresh display
    display.clearBuffer();
    display.sendBuffer();

    encoder.bind(0, &encA, &encB, [this](int id, int dir) {
        this->onEncoderTurn(dir);
    }, MuxEncoder::MODE_QUAD_STEP);

    button.bind(0, &btnState, [this](int id, MuxButton::btnEvents evt) {
        this->onButtonEvent(evt);
    });

    button.setRiseTimeMs(20);
    button.setFallTimeMs(10);
    button.setLongPressDelayMs(800);
    button.enableLateClick(true);

    currentPage = PAGE_MAIN;
    updateDisplay();
}

void TextGUI::process() {
    encoder.process();
    button.process();
    // render is moved to top .ino
}


void TextGUI::onEncoderTurn(int direction) {
    switch(currentPage) {
        case PAGE_MAIN:
            menuPosition = (menuPosition + direction) % 4;
            if (menuPosition < 0) menuPosition = 3;
            break;
        case PAGE_PROGRAM:
            changeProgram(direction);
            break;
        case PAGE_CHANNEL:
            currentChannel = (currentChannel + direction) & 0x0F;
            break;
        case PAGE_SF2:
            if (direction > 0) synth.loadNextSf2();
            break;
    }
    updateDisplay();
}

void TextGUI::onButtonEvent(MuxButton::btnEvents evt) {
    if (evt == MuxButton::EVENT_CLICK) {
        switch(currentPage) {
            case PAGE_MAIN:
                if (menuPosition == 0) currentPage = PAGE_PROGRAM;
                else if (menuPosition == 1) currentPage = PAGE_CHANNEL;
                else if (menuPosition == 2) currentPage = PAGE_SF2;
                else if (menuPosition == 3) synth.GMReset();
                break;
            default:
                currentPage = PAGE_MAIN;
                break;
        }
        updateDisplay();
    }
    else if (evt == MuxButton::EVENT_LONGPRESS && currentPage != PAGE_MAIN) {
        currentPage = PAGE_MAIN;
        updateDisplay();
    }
}

void TextGUI::changeProgram(int direction) {
    auto& channel = synth.getChannelState(currentChannel);
    int newProgram = channel.program + direction;
    if (newProgram < 0) newProgram = 127;
    else if (newProgram > 127) newProgram = 0;
    synth.programChange(currentChannel, newProgram);
}

void TextGUI::updateDisplay() {
    needsDisplayUpdate = true;
}

void TextGUI::renderDisplay() {
    display.clearBuffer();
    const uint8_t lineHeight = 10;
    uint8_t y = 0;
    
    switch(currentPage) {
        case PAGE_MAIN:
            display.drawStr(0, y, "SF2 Synth - Main Menu");
            y += lineHeight;
         //   display.drawHLine(0, y, display.getDisplayWidth());
         //   y += lineHeight;
            
            display.drawStr(2, y, menuPosition == 0 ? "> Program" : "  Program");
            y += lineHeight;
            display.drawStr(2, y, menuPosition == 1 ? "> Channel" : "  Channel");
            y += lineHeight;
            display.drawStr(2, y, menuPosition == 2 ? "> SoundFont" : "  SoundFont");
            y += lineHeight;
            display.drawStr(2, y, menuPosition == 3 ? "> GM Reset" : "  GM Reset");
            break;
            
        case PAGE_PROGRAM: {
            auto& channel = synth.getChannelState(currentChannel);
            display.drawStr(0, y, "Program Change");
            y += lineHeight;
        //    display.drawHLine(0, y, display.getDisplayWidth());
        //    y += lineHeight;
            
            char buf[32];
            snprintf(buf, sizeof(buf), "Channel: %d", currentChannel + 1);
            display.drawStr(2, y, buf);
            y += lineHeight;
            
            snprintf(buf, sizeof(buf), "Program: %d", channel.program);
            display.drawStr(2, y, buf);
            y += lineHeight;
            
            snprintf(buf, sizeof(buf), "Bank: %d", channel.getBank());
            display.drawStr(2, y, buf);
            break;
        }
            
        case PAGE_CHANNEL: {
            char buf[32];
            display.drawStr(0, y, "Channel Select");
            y += lineHeight;
        //    display.drawHLine(0, y, display.getDisplayWidth());
        //    y += lineHeight;
            
            snprintf(buf, sizeof(buf), "Current Channel: %d", currentChannel + 1);
            display.drawStr(2, y, buf);
            break;
        }
            
        case PAGE_SF2:
            display.drawStr(0, y, "SoundFont");
            y += lineHeight;
        //    display.drawHLine(0, y, display.getDisplayWidth());
        //    y += lineHeight;
            display.drawStr(2, y, "Turn encoder to");
            y += lineHeight;
            display.drawStr(2, y, "load next SF2");
            break;
    }
    char buf[49];
    synth.getActivityString(buf);
    display.drawUTF8(14, 63 - 9, buf);
    //display.sendBuffer();
}

int TextGUI::updateUiBlock() {
    static const int send_tiles = 4;
    static const int block_h = display.getBufferTileHeight();
    static const int block_w = display.getBufferTileWidth();
    static int cur_xt = 0;
    static int cur_yt = 0;
    display.updateDisplayArea(cur_xt, cur_yt, send_tiles, 1);
    cur_xt+=send_tiles;
    if(cur_xt >= block_w) {
      cur_xt = 0;
      cur_yt++;
    }
    cur_yt %= block_h;
    return cur_xt + cur_yt;
  }

void TextGUI::draw() {
    if ( updateUiBlock() == 0 ) {
        renderDisplay();
    }
}
#endif