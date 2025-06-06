#pragma once

/**
* Simple class for handling rotary encoders
* 
* 1. Create an instance:
*     MuxEncoder encoder;
*
* 2. Submit encoder id, pointers to the GPIO readings, your callback function and your encoder type e.g.:
*     uint8_t A, B; 
*     myEncHandler(int id, int dir) {
*       // id is what you submit via bind()
*       // dir is -1 for counter-clock-wise or 1 for clock-wise rotation
*       ... 
*     }
*     encoder.bind(0, &A, &B, myEncHandler, MuxEncoder::MODE_HALF_STEP);
* 
* 3. Poll process() in a loop:
*     while(i_want_enc_processing) {
*       A = digitalRead(PIN_A);
*       B = digitalRead(PIN_B);
*       encoder.process();
*     }
*
* File: encoder.h
* April 2025
* Author: Evgeny Aslovskiy AKA Copych
* License: MIT
*/
#pragma once

#include <functional>

#ifndef ACTIVE_STATE
#define ACTIVE_STATE LOW   // LOW = switch connects to GND, HIGH = switch connects to 3V3
#endif

class MuxEncoder {
public:
    MuxEncoder() {;}
    ~MuxEncoder() {;}

    enum encMode { 
        MODE_HALF_STEP, 
        MODE_FULL_STEP, 
        MODE_DOUBLE_STEP, 
        MODE_QUAD_STEP, 
        MODE_COUNT 
    };

    static constexpr int8_t stepIncrement[2][16] = {
        // MODE_HALF_STEP
        {0, 1, -1, 0,  -1, 0, 0, 1,  1, 0, 0, -1,  0, -1, 1, 0},
        // MODE_FULL_STEP
        {0, 0,  0, 0,  -1, 0, 0, 1,  1, 0, 0, -1,  0,  0, 0, 0}
    };

    inline void bind(uint8_t id, uint8_t* a, uint8_t* b, std::function<void(int, int)> cb, encMode mode = MODE_FULL_STEP) {
        _a = a;
        _b = b;
        _mode = mode;
        _id = id;
        _callbackFunc = cb;
        _accumulator = 0;
    }

    inline void process() {
        if (!_a || !_b || !_callbackFunc) return;

        unsigned int clk = (*_a == ACTIVE_STATE);
        unsigned int dt  = (*_b == ACTIVE_STATE);

        newState = (clk | (dt << 1));
        if (newState != oldState) {
            int stateMux = newState | (oldState << 2);
            oldState = newState;

            int delta = 0;

            if (_mode == MODE_HALF_STEP || _mode == MODE_FULL_STEP) {
                delta = stepIncrement[_mode][stateMux];
                if (delta != 0) _callbackFunc(_id, delta);
            } else {
                // Always decode with HALF_STEP table
                delta = stepIncrement[MODE_HALF_STEP][stateMux];
                _accumulator += delta;

                // fire only when at physical detent position (both LOW = 00)
                if (newState == 0) {
                    int steps_needed = (_mode == MODE_DOUBLE_STEP) ? 2 : 4;
                    if (std::abs(_accumulator) >= steps_needed) {
                        int dir = (_accumulator > 0) ? 1 : -1;
                        _accumulator = 0;
                        _callbackFunc(_id, dir);
                    } else {
                        // Snap to 0 if noise causes early return to 00
                        _accumulator = 0;
                    }
                }
            }
        }
    }

protected:
    int newState = 0;
    int oldState = 0;

private:
    uint8_t _id;
    uint8_t* _a = nullptr;
    uint8_t* _b = nullptr;
    encMode _mode = MODE_FULL_STEP;
    std::function<void(int, int)> _callbackFunc;

    int _accumulator = 0;
};
