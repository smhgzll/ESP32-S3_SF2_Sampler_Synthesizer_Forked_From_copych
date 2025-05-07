#pragma once

/**
* LFO class by Copych https://github.com/copych (c) 2024 
* It's based on LUTs and micros(), mostly intended for using on MCUs
* It can be called asynchronously as it has internal phase to time sync, so the sampling rate is not in use
* Note that the phase used is normalized to [0..1)
*/

#include "misc.h"
#include "myrand.h"

#define   MAX_LFO_FREQ      (200.0f)
#define   MIN_LFO_FREQ      (0.0f)

class LFO {

  public:
    enum eWave_t   {WAVE_SIN, WAVE_SAW, WAVE_REV_SAW, WAVE_TRIANGLE, WAVE_SQUARE, WAVE_RANDOM, WAVE_COUNT} ;
    enum eRange_t  {RANGE_FULL /*(-1..1)*/, RANGE_POSITIVE /*(0..1)*/, RANGE_NEGATIVE /*(-1..0)*/,RANGE_COUNT};
    
    LFO() { Rnd.init(); syncPhase(); }
    ~LFO() {;}
    
    inline void         setRange(eRange_t range) {
                          _range = range;
                          switch (range) {
                            case RANGE_POSITIVE:  _low_val = 0.0f;  _high_val = 1.0f; _k = 0.5f;  _a = 0.5f;  break;
                            case RANGE_NEGATIVE:  _low_val = -1.0f; _high_val = 0.0f; _k = -0.5f; _a = -0.5f; break;
                            case RANGE_FULL:      
                            default:              _low_val = -1.0f; _high_val = 1.0f; _k = 1.0f;  _a = 0.0f;  break;        
                          }
                        }
    inline eRange_t     getRange()                { return _range; }
    
    inline void         setWave(eWave_t wave, bool soft = false) {
                          float new_phase ;
                          if (soft) new_phase = findBestPhase(_wave, wave); else new_phase = _phase;
                          switch (wave) {
                            case WAVE_SAW:        getCurVal = &LFO::getSaw; break;
                            case WAVE_REV_SAW:    getCurVal = &LFO::getRSaw; break;
                            case WAVE_TRIANGLE:   getCurVal = &LFO::getTri; break;
                            case WAVE_SQUARE:     getCurVal = &LFO::getSqr; break;
                            case WAVE_RANDOM:     getCurVal = &LFO::getRnd; break;
                            default:
                            case WAVE_SIN:        getCurVal = &LFO::getSin; break;
                          }
                          _wave = wave;
                          _phase = new_phase;
                        }
    inline eWave_t      getWave()                 { return _wave; }
    
    inline void         setFreq(float freq)       { _freq = constrain(freq, MIN_LFO_FREQ, MAX_LFO_FREQ); _micro_freq = (float)freq * 1.0e-06f; }
    inline float        getFreq()                 { return _freq; }
    
    inline void         setPhase(float norm_phase){ int p = norm_phase; _phase = norm_phase - p; } // note that there might be some issues if you try to use this with big numbers as both int and float are quite limited types
    inline float        getPhase()                { return _phase; }
    inline void         syncPhase() {
                          syncPhase(micros());
                        }
    inline void         syncPhase(size_t t) {
                          static size_t last_sync = 0;
                          size_t        interval  = t - last_sync;
                          float         new_phase = (float)_phase + (float)interval * (float)_micro_freq;
                          last_sync     = t;
                          setPhase(new_phase);
                        }
                        
    inline float        getSample()               { return getSample(micros()); }
    inline float        getSample(size_t t) {
                          syncPhase(t);
                          _val = (this->*getCurVal)();
                          return _val; 
                        }
    
  private:
    MyRand              Rnd;
    float               _freq           = 0.5f;
    float               _micro_freq     = 0.5e-06f;
    float               _phase          = 0.0f; // MUST be within [0..1)
    eWave_t             _wave           = WAVE_SIN;
    eRange_t            _range          = RANGE_FULL;
    float               _k              = 1.0f; // v(x) = _k * x + _a
    float               _a              = 0.0f;
    float               _low_val        = -1.0f;
    float               _high_val       = 1.0f;
    float               _val            = 0.0f;
    
    inline float        findBestPhase(eWave_t wave_old, eWave_t wave_new) {
                          bool raising = false;
                          bool positive = false;
                          bool no_matter = false;
                          float new_phase = _phase;
                          switch (wave_old) {
                            case WAVE_SAW:        raising = true; positive = (_phase >= 0.5f); break;   
                            case WAVE_REV_SAW:    raising = false; positive = (_phase < 0.5f); break;
                            case WAVE_TRIANGLE:   raising = (_phase < 0.5f); positive = !(_phase >= 0.25f && _phase < 0.75f); break;
                            case WAVE_SQUARE:     raising = false; positive = (_phase >= 0.5f); break;
                            case WAVE_RANDOM:     no_matter = true; break;
                            default:
                            case WAVE_SIN:        raising = !(_phase >= 0.25f && _phase < 0.75f); positive = (_phase < 0.5f ); break;
                          }
                          if (no_matter) return _phase;
                          switch (wave_new) {
                            case WAVE_SAW:        if (positive) new_phase = 0.75f; else new_phase = 0.25f; break;   
                            case WAVE_REV_SAW:    if (positive) new_phase = 0.25f; else new_phase = 0.75f; break;
                            case WAVE_SQUARE:     if (positive) new_phase = 0.51f; else new_phase = 0.01f; break;
                            case WAVE_RANDOM:     no_matter = true; break;
                            default:
                            case WAVE_SIN:
                            case WAVE_TRIANGLE:   
                              if (positive) {
                                if (raising) new_phase = 0.375f; else new_phase = 0.625f;
                              } else {
                                if (raising) new_phase = 0.125f; else new_phase = 0.875f;
                              }
                              break;
                          }
                          return new_phase;
                        }
    
    inline float        getSaw() {
                          float ret = (2.0f * (float)_phase - 1.0f) * _k + _a;
                          return ret;
                        }
    inline float        getRSaw() {
                          float ret = (1.0f - 2.0f * (float)_phase) * _k + _a;
                          return ret;
                        }
    inline float        getTri() {
                          float ret = (_phase < 0.5f) ? (4.0f * (float)_phase - 1.0f) : (3.0f - 4.0f * (float)_phase) ;
                          return (float)_k * (float)ret + (float)_a;
                        }
    inline float        getSqr() {
                          float ret = (_phase < 0.5f) ? _low_val : _high_val;
                          return ret;
                        }
    inline float        getRnd() {
                          return Rnd.getFloatInRange(_low_val, _high_val);
                        }
    inline float        getSin() {
                          float ret = sin_lut(_phase);
                          return (float)_k * (float)ret + (float)_a;
                        }
    float               (LFO::*getCurVal)() = &LFO::getSin ;
};
