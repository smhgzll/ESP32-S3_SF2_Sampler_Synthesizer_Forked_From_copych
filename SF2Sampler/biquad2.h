/*
 * ----------------------------------------------------------------------------
 * ESP32-S3 optimized biquad filter
 * splitted into 3 classes
 *
* Author: Evgeny Aslovskiy AKA Copych
 * License: MIT
 * Repository: https://github.com/copych/ESP32-S3_SF2_Sampler_Synthesizer
 * 
 * File: biquad2.h
 * Purpose: contains 3 classes:
 *          Biquad filter coefficient calculator, 
 *          Biquad filter with internal coefficients,
 *          Biquad filter with shared coefficients
 * ----------------------------------------------------------------------------
 */

#pragma once
#include <Arduino.h>
#include "config.h"

// ---------- Tweaks ----------
#ifndef BIQUAD_LUT_IN_PSRAM
#define BIQUAD_LUT_IN_PSRAM 1  // 1: put LUT into PSRAM (EXT_RAM_ATTR) if available
#endif

#ifndef FILTER_MAX_Q
#define FILTER_MAX_Q 10.0f
#endif

// Attribute helpers
#ifndef BIQUAD_FORCE_INLINE
#define BIQUAD_FORCE_INLINE __attribute__((always_inline)) inline
#endif

#ifndef BIQUAD_IRAM
#define BIQUAD_IRAM IRAM_ATTR
#endif

// Likely/unlikely (optional)
#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

class BiquadCalc {
public:
    enum Mode : uint8_t {
        LowPass = 0,
        HighPass,
        BandPass,
        Notch
    };

    struct Coeffs {
        float b0, b1, b2, a1, a2; // a0 is normalized to 1
    };

private:
    // Compile-time constants (stay in flash)
    static constexpr float kPi      = 3.14159265358979323846f;
    static constexpr size_t FreqSteps = 64;
    static constexpr size_t QSteps    = 16;
    static constexpr float Fs         = (float)SAMPLE_RATE;
    static constexpr float FreqMin    = 20.0f;
    static constexpr float FreqMax    = 20000.0f;
    static constexpr float QMin       = 0.5f;
    static constexpr float QMax       = FILTER_MAX_Q;

    // Log-space helpers for freq grid
    static constexpr float logFreqMin      = 2.9957323f;  // logf(20)
    static constexpr float logFreqMax      = 9.9034876f;  // logf(20000)
    static constexpr float invLogFreqRange = 1.0f / (logFreqMax - logFreqMin);

    struct CoeffsLUTEntry {
        float b0, b1, b2, a1, a2;
    };

    // Linear interpolation helpers
    static BIQUAD_FORCE_INLINE float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }
    static BIQUAD_FORCE_INLINE CoeffsLUTEntry lerpCoeffs(const CoeffsLUTEntry& c1,
                                                         const CoeffsLUTEntry& c2,
                                                         float t) {
        CoeffsLUTEntry r;
        r.b0 = lerp(c1.b0, c2.b0, t);
        r.b1 = lerp(c1.b1, c2.b1, t);
        r.b2 = lerp(c1.b2, c2.b2, t);
        r.a1 = lerp(c1.a1, c2.a1, t);
        r.a2 = lerp(c1.a2, c2.a2, t);
        return r;
    }

    static BIQUAD_FORCE_INLINE float qAtIndex(size_t i) {
        const float t = float(i) / float(QSteps - 1);
        return QMin + t * (QMax - QMin);
    }
    static BIQUAD_FORCE_INLINE float freqAtIndex(size_t i) {
        const float t = float(i) / float(FreqSteps - 1);
        return expf(logFreqMin + t * (logFreqMax - logFreqMin));
    }

    // RBJ low-pass base (normalized a0=1)
    static CoeffsLUTEntry calcLP(float f0, float Q) {
        const float w0     = 2.0f * kPi * f0 / Fs;
        const float cw0    = cosf(w0);
        const float sw0    = sinf(w0);
        const float alpha  = sw0 / (2.0f * Q);

        const float a0_inv = 1.0f / (1.0f + alpha);
        const float a1     = -2.0f * cw0;
        const float a2     =  1.0f - alpha;

        const float common = 0.5f * (1.0f - cw0);
        const float b0     = common;
        const float b1     = 2.0f * common;
        const float b2     = common;

        CoeffsLUTEntry c;
        c.b0 = b0 * a0_inv;
        c.b1 = b1 * a0_inv;
        c.b2 = b2 * a0_inv;
        c.a1 = a1 * a0_inv;
        c.a2 = a2 * a0_inv;
        return c;
    }

    // Generate LUT for LowPass only (others use direct RBJ calc)
    static void generateLUT() {
        for (size_t q = 0; q < QSteps; ++q) {
            const float Q = qAtIndex(q);
            for (size_t f = 0; f < FreqSteps; ++f) {
                lut[q * FreqSteps + f] = calcLP(freqAtIndex(f), Q);
            }
        }
        lutInitialized = true;
    }

    static BIQUAD_FORCE_INLINE CoeffsLUTEntry interpolateLUT(float freq, float Q) {
        // Clamp inputs
        if (freq < FreqMin) freq = FreqMin; else if (freq > FreqMax) freq = FreqMax;
        if (Q    < QMin   ) Q    = QMin;    else if (Q    > QMax   ) Q    = QMax;

        const float freqPos = (logf(freq) - logFreqMin) * invLogFreqRange * (FreqSteps - 1);
        const float qPos    = (Q - QMin) / (QMax - QMin) * (QSteps - 1);

        const size_t fi0 = (size_t)freqPos;
        const size_t qi0 = (size_t)qPos;
        const size_t fi1 = (fi0 + 1 < FreqSteps) ? fi0 + 1 : FreqSteps - 1;
        const size_t qi1 = (qi0 + 1 < QSteps   ) ? qi0 + 1 : QSteps - 1;

        const float tf = freqPos - float(fi0);
        const float tq = qPos    - float(qi0);

        const CoeffsLUTEntry& c00 = lut[qi0 * FreqSteps + fi0];
        const CoeffsLUTEntry& c01 = lut[qi0 * FreqSteps + fi1];
        const CoeffsLUTEntry& c10 = lut[qi1 * FreqSteps + fi0];
        const CoeffsLUTEntry& c11 = lut[qi1 * FreqSteps + fi1];

        const CoeffsLUTEntry cf0 = lerpCoeffs(c00, c01, tf);
        const CoeffsLUTEntry cf1 = lerpCoeffs(c10, c11, tf);
        return lerpCoeffs(cf0, cf1, tq);
    }

    // RBJ "cookbook" direct calculators (normalized a0=1)
    static Coeffs calcHP(float f0, float Q) {
        const float w0     = 2.0f * kPi * f0 / Fs;
        const float cw0    = cosf(w0);
        const float sw0    = sinf(w0);
        const float alpha  = sw0 / (2.0f * Q);

        const float a0_inv = 1.0f / (1.0f + alpha);
        const float a1     = -2.0f * cw0;
        const float a2     =  1.0f - alpha;

        const float common = 0.5f * (1.0f + cw0);
        const float b0     =  common;
        const float b1     = -2.0f * common;
        const float b2     =  common;

        Coeffs c;
        c.b0 = b0 * a0_inv;
        c.b1 = b1 * a0_inv;
        c.b2 = b2 * a0_inv;
        c.a1 = a1 * a0_inv;
        c.a2 = a2 * a0_inv;
        return c;
    }

    // Band-pass (constant skirt gain, peak gain = Q)
    static Coeffs calcBP(float f0, float Q) {
        const float w0     = 2.0f * kPi * f0 / Fs;
        const float cw0    = cosf(w0);
        const float sw0    = sinf(w0);
        const float alpha  = sw0 / (2.0f * Q);

        const float a0_inv = 1.0f / (1.0f + alpha);
        const float a1     = -2.0f * cw0;
        const float a2     =  1.0f - alpha;

        const float b0     =  0.5f * sw0;
        const float b1     =  0.0f;
        const float b2     = -0.5f * sw0;

        Coeffs c;
        c.b0 = b0 * a0_inv;
        c.b1 = b1 * a0_inv;
        c.b2 = b2 * a0_inv;
        c.a1 = a1 * a0_inv;
        c.a2 = a2 * a0_inv;
        return c;
    }

    static Coeffs calcNotch(float f0, float Q) {
        const float w0     = 2.0f * kPi * f0 / Fs;
        const float cw0    = cosf(w0);
        const float sw0    = sinf(w0);
        const float alpha  = sw0 / (2.0f * Q);

        const float a0_inv = 1.0f / (1.0f + alpha);
        const float a1     = -2.0f * cw0;
        const float a2     =  1.0f - alpha;

        const float b0     = 1.0f;
        const float b1     = -2.0f * cw0;
        const float b2     = 1.0f;

        Coeffs c;
        c.b0 = b0 * a0_inv;
        c.b1 = b1 * a0_inv;
        c.b2 = b2 * a0_inv;
        c.a1 = a1 * a0_inv;
        c.a2 = a2 * a0_inv;
        return c;
    }

public:
    static void ensureLUT() {
        if (!lutInitialized) generateLUT();
    }

    static Coeffs calcCoeffs(float freq, float Q, Mode mode) {
        // For LP use LUT (+bilinear interpolation). Others: compute directly (rarely called).
        if (mode == LowPass) {
            ensureLUT();
            const CoeffsLUTEntry c = interpolateLUT(freq, Q);
            Coeffs r{c.b0, c.b1, c.b2, c.a1, c.a2};
            return r;
        }
        // Clamp once here for other modes
        if (freq < FreqMin) freq = FreqMin; else if (freq > FreqMax) freq = FreqMax;
        if (Q    < QMin   ) Q    = QMin;    else if (Q    > QMax   ) Q    = QMax;

        switch (mode) {
            case HighPass: return calcHP(freq, Q);
            case BandPass: return calcBP(freq, Q);
            case Notch   : return calcNotch(freq, Q);
            default      : return calcHP(freq, Q); // safe fallback
        }
    }

private:
#if BIQUAD_LUT_IN_PSRAM && defined(EXT_RAM_ATTR)
    static EXT_RAM_ATTR CoeffsLUTEntry lut[FreqSteps * QSteps];
#else
    static CoeffsLUTEntry lut[FreqSteps * QSteps];
#endif
    static bool lutInitialized;
};

// Static storage
#if BIQUAD_LUT_IN_PSRAM && defined(EXT_RAM_ATTR)
EXT_RAM_ATTR
#endif
inline BiquadCalc::CoeffsLUTEntry BiquadCalc::lut[BiquadCalc::FreqSteps * BiquadCalc::QSteps];
inline bool BiquadCalc::lutInitialized = false;

// ================================================================================================

class BiquadFilterSharedCoeffs {
public:
    using Coeffs = BiquadCalc::Coeffs;

    BiquadFilterSharedCoeffs() = default;

    BIQUAD_FORCE_INLINE void setCoeffs(const Coeffs* coeffsPtr) {
        coeffs = coeffsPtr;
    }

    BIQUAD_FORCE_INLINE void resetState() {
        x1 = x2 = y1 = y2 = 0.0f;
        w1 = w2 = z1 = z2 = 0.0f;
    }

    // Single-sample (mono)
    BIQUAD_FORCE_INLINE float BIQUAD_IRAM process(float in) {
        if (UNLIKELY(coeffs == nullptr)) return in; // fail-safe
        const Coeffs& c = *coeffs;

        // Load to registers
        float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;

        // y[n] = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
        float out = (c.b0 * in) + (c.b1 * lx1 + c.b2 * lx2) - (c.a1 * ly1 + c.a2 * ly2);

        // Store back
        x2 = lx1; x1 = in;
        y2 = ly1; y1 = out;
        return out;
    }

    // In-place stereo (L/R pointers must not alias)
    BIQUAD_FORCE_INLINE void BIQUAD_IRAM processLR(float* __restrict inOutL,
                                                   float* __restrict inOutR) {
        if (UNLIKELY(coeffs == nullptr)) return;
        const Coeffs& c = *coeffs;

        // Left
        float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
        float inL = *inOutL;
        float outL = (c.b0 * inL) + (c.b1 * lx1 + c.b2 * lx2) - (c.a1 * ly1 + c.a2 * ly2);
        x2 = lx1; x1 = inL;
        y2 = ly1; y1 = outL;
        *inOutL = outL;

        // Right
        float rx1 = w1, rx2 = w2, ry1 = z1, ry2 = z2;
        float inR = *inOutR;
        float outR = (c.b0 * inR) + (c.b1 * rx1 + c.b2 * rx2) - (c.a1 * ry1 + c.a2 * ry2);
        w2 = rx1; w1 = inR;
        z2 = ry1; z1 = outR;
        *inOutR = outR;
    }

    // Fast block processing (interleaved not assumed; pass L/R separately)
    BIQUAD_FORCE_INLINE void BIQUAD_IRAM processBufferLR(float* __restrict bufL,
                                                         float* __restrict bufR,
                                                         size_t n) {
        if (UNLIKELY(coeffs == nullptr)) return;
        const Coeffs& c = *coeffs;

        float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
        float rx1 = w1, rx2 = w2, ry1 = z1, ry2 = z2;

        for (size_t i = 0; i < n; ++i) {
            const float inL = bufL[i];
            const float inR = bufR[i];

            const float outL = (c.b0 * inL) + (c.b1 * lx1 + c.b2 * lx2) - (c.a1 * ly1 + c.a2 * ly2);
            const float outR = (c.b0 * inR) + (c.b1 * rx1 + c.b2 * rx2) - (c.a1 * ry1 + c.a2 * ry2);

            lx2 = lx1; lx1 = inL;  ly2 = ly1; ly1 = outL;
            rx2 = rx1; rx1 = inR;  ry2 = ry1; ry1 = outR;

            bufL[i] = outL;
            bufR[i] = outR;
        }

        x1 = lx1; x2 = lx2; y1 = ly1; y2 = ly2;
        w1 = rx1; w2 = rx2; z1 = ry1; z2 = ry2;
    }

private:
    const Coeffs* coeffs = nullptr;

    // States in DRAM (hot)
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    float w1 = 0.0f, w2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
};

// ================================================================================================

class BiquadFilterInternalCoeffs {
public:
    using Coeffs = BiquadCalc::Coeffs;

    BiquadFilterInternalCoeffs() {
        resetState();
        setMode(BiquadCalc::LowPass);
        setFreqAndQ(20000.0f, 0.707f);
    }

    BIQUAD_FORCE_INLINE void setMode(BiquadCalc::Mode m) {
        if (m != mode) { mode = m; updateCoeffs(); }
    }
    BIQUAD_FORCE_INLINE void setFreq(float f) {
        if (f != freq) { freq = f; updateCoeffs(); }
    }
    BIQUAD_FORCE_INLINE void setQ(float q) {
        if (q != Q) { Q = q; updateCoeffs(); }
    }
    BIQUAD_FORCE_INLINE void setFreqAndQ(float f, float q) {
        if (f != freq || q != Q) { freq = f; Q = q; updateCoeffs(); }
    }

    BIQUAD_FORCE_INLINE void resetState() {
        x1 = x2 = y1 = y2 = 0.0f;
        w1 = w2 = z1 = z2 = 0.0f;
    }

    // Mono
    BIQUAD_FORCE_INLINE float BIQUAD_IRAM process(float in) {
        const float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
        const float out = (coeffs.b0 * in) + (coeffs.b1 * lx1 + coeffs.b2 * lx2)
                          - (coeffs.a1 * ly1 + coeffs.a2 * ly2);
        x2 = lx1; x1 = in;
        y2 = ly1; y1 = out;
        return out;
    }

    // Stereo LR in-place
    BIQUAD_FORCE_INLINE void BIQUAD_IRAM processLR(float* __restrict inOutL,
                                                   float* __restrict inOutR) {
        // Left
        float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
        const float inL  = *inOutL;
        const float outL = (coeffs.b0 * inL) + (coeffs.b1 * lx1 + coeffs.b2 * lx2)
                           - (coeffs.a1 * ly1 + coeffs.a2 * ly2);
        x2 = lx1; x1 = inL;
        y2 = ly1; y1 = outL;
        *inOutL = outL;

        // Right
        float rx1 = w1, rx2 = w2, ry1 = z1, ry2 = z2;
        const float inR  = *inOutR;
        const float outR = (coeffs.b0 * inR) + (coeffs.b1 * rx1 + coeffs.b2 * rx2)
                           - (coeffs.a1 * ry1 + coeffs.a2 * ry2);
        w2 = rx1; w1 = inR;
        z2 = ry1; z1 = outR;
        *inOutR = outR;
    }

    // Block processing
    BIQUAD_FORCE_INLINE void BIQUAD_IRAM processBufferLR(float* __restrict bufL,
                                                         float* __restrict bufR,
                                                         size_t n) {
        float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
        float rx1 = w1, rx2 = w2, ry1 = z1, ry2 = z2;

        const float b0 = coeffs.b0, b1 = coeffs.b1, b2 = coeffs.b2;
        const float a1 = coeffs.a1, a2 = coeffs.a2;

        for (size_t i = 0; i < n; ++i) {
            const float inL = bufL[i];
            const float inR = bufR[i];

            const float outL = (b0 * inL) + (b1 * lx1 + b2 * lx2) - (a1 * ly1 + a2 * ly2);
            const float outR = (b0 * inR) + (b1 * rx1 + b2 * rx2) - (a1 * ry1 + a2 * ry2);

            lx2 = lx1; lx1 = inL;  ly2 = ly1; ly1 = outL;
            rx2 = rx1; rx1 = inR;  ry2 = ry1; ry1 = outR;

            bufL[i] = outL;
            bufR[i] = outR;
        }

        x1 = lx1; x2 = lx2; y1 = ly1; y2 = ly2;
        w1 = rx1; w2 = rx2; z1 = ry1; z2 = ry2;
    }

private:
    void updateCoeffs() {
        coeffs = BiquadCalc::calcCoeffs(freq, Q, mode);
    }

    BiquadCalc::Mode mode = BiquadCalc::LowPass;
    float freq = 20000.0f;
    float Q    = 0.707f;
    Coeffs coeffs{};

    // States
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    float w1 = 0.0f, w2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
};