#pragma once
/*
   FAST Pseudo-random generator
*/

//#define XORSHIFT
//#define LFSR
#define CONGRUENT

const uint32_t MYRAND_MAX = 0xFFFFFFFF;
const uint32_t MYRAND_MAGIC = 0xcf300000;
// const uint32_t MYRAND_MAGIC = 0xBF000000;

class MyRand {

public:
  MyRand(){setSeed();};
  ~MyRand(){};
  
  inline uint32_t getRaw() {
    _myRandomState = next(_myRandomState);
    return _myRandomState;
  }

  inline uint32_t getUnsignedInt(uint32_t max) {
    if (max==0) return 0;
    return getRaw() % max;
  }

  inline float getFloat() {
    return (getRaw()) * TO_FLOAT;
  }

  inline float getFloat(const float& upper_limit) {
    return ( upper_limit * TO_FLOAT * (float)(getRaw() ) ) ;
  }
  
  inline float getFloatInRange(const float& lower_limit, const float& upper_limit) {
    return (float)lower_limit + ( (float)(upper_limit - lower_limit) * (float)TO_FLOAT * (float)(getRaw() ) ) ;
  }

  inline float getFloatSpread(const float& center_val, const float& variation) {
    return center_val - variation + ( variation * TO_FLOAT_2 * (float)(getRaw() ) ) ;
  }
  
  inline void randomize(uint32_t data) {
    _myRandomState = next((_myRandomState << 1) ^ data);
  }

  inline void setSeed() {
    randomize((uint32_t)(micros() & MYRAND_MAX));
  }
  
  inline void setSeed(uint32_t seed) {
    _myRandomState = seed;
  }
  
  inline bool tryChance(float chance_normalized) { // e.g. tryChance(0.3f) should return true ~30% times tried
    bool ret (getFloat() < chance_normalized);
    return ret;
  }

  inline void init() {
    randomize(random(3, MYRAND_MAX));
  }
  

private:  
  const float TO_FLOAT = 1.0f / MYRAND_MAX;
  const float TO_FLOAT_2 = 2.0f / MYRAND_MAX;
  uint32_t _myRandomState = 1664525UL ;
  const uint32_t _a = 1664525UL ;
  const uint32_t _c = 1013904223UL;

#ifdef LFSR
// lfsr32
  inline uint32_t next(uint32_t x) {
    uint32_t y ;
    y = x >> 1;
    if (x & 1) y ^= MYRAND_MAGIC;
    return y;
  }
#elif defined XORSHIFT
// xorshift32
  inline uint32_t next(uint32_t x)
  {
      uint32_t y = x;
      y ^= y << 13;
      y ^= y >> 17;
      y ^= y << 5;
   //   x = y;
      return y;
  }
#elif defined CONGRUENT
// Linear Congruential Generator
  inline uint32_t next(uint32_t x)
  {
      uint32_t y;
      y = (uint32_t)_a * (uint32_t)x + (uint32_t)_c;      
      return y;
  }
#endif

};
