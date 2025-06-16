#pragma once

#include "config.h"
#include "channel.h"

#ifdef ENABLE_REVERB
  #include "fx_reverb.h"
#endif
#ifdef ENABLE_DELAY
  #include "fx_delay.h"
#endif
#ifdef ENABLE_CHORUS
  #include "fx_chorus.h"
#endif

struct SynthState {
    ChannelState (&channels)[NUM_MIDI_CHANNELS];

#ifdef ENABLE_REVERB
    FxReverb& reverb;
#endif
#ifdef ENABLE_DELAY
    FxDelay& delayfx;
#endif
#ifdef ENABLE_CHORUS
    FxChorus& chorus;
#endif

    SynthState(
        ChannelState (&ch)[NUM_MIDI_CHANNELS]
#ifdef ENABLE_REVERB
        , FxReverb& r
#endif
#ifdef ENABLE_DELAY
        , FxDelay& d
#endif
#ifdef ENABLE_CHORUS
        , FxChorus& c
#endif
    )
    : channels(ch)
#ifdef ENABLE_REVERB
    , reverb(r)
#endif
#ifdef ENABLE_DELAY
    , delayfx(d)
#endif
#ifdef ENABLE_CHORUS
    , chorus(c)
#endif
    {}

    inline ChannelState& ch(uint8_t i) { return channels[i & 0x0F]; }
};
