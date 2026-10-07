#pragma once
// Audio block processing for the capture task. Plain C++ with no Arduino or ESP-IDF
// dependency, so test/host/test_dsp.cpp can exercise it on a PC.
#include <math.h>
#include <stdint.h>

// 2nd-order IIR section (direct form I).
struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    inline void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};

// Butterworth high-pass (RBJ cookbook, Q = 1/sqrt(2)). Sets coefficients only; state is kept.
inline void designHighpass(Biquad& f, float fs, float fc) {
    const float w0 = 2.0f * 3.14159265358979f * (fc / fs);
    const float cosw0 = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + alpha;
    f.b0 = (1.0f + cosw0) * 0.5f / a0;
    f.b1 = -(1.0f + cosw0) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0f * cosw0 / a0;
    f.a2 = (1.0f - alpha) / a0;
}

// One-pole DC blocker, y[n] = x[n] - x[n-1] + r * y[n-1]. The PDM decoder output carries a DC
// offset; without this it is multiplied by the gain and eats headroom when the HPF is off.
struct DcBlocker {
    float r{0.998f}, x1{0.0f}, y1{0.0f};
    inline void design(float fs, float fc) { r = 1.0f - 2.0f * 3.14159265358979f * fc / fs; }
    inline float process(float x) {
        float y = x - x1 + r * y1;
        x1 = x; y1 = y;
        return y;
    }
    inline void reset() { x1 = y1 = 0.0f; }
};

struct BlockStats {
    float peak;        // largest |sample| after gain, before clipping (0..)
    float sumSquares;  // for RMS (AGC)
    bool clipped;
    bool flat;         // every input sample identical: a stuck PDM data line, not a quiet scene
};

// DC block -> optional high-pass -> gain ramped linearly from g0 to g1 across the block (no
// zipper noise when gain or AGC changes) -> round to nearest -> clip to 16 bit. Samples are
// written big-endian (RTP L16) to outBE when it is non-null; metering runs either way.
inline BlockStats processBlock(const int16_t* in, uint32_t n, DcBlocker& dc, Biquad* hpf,
                               float g0, float g1, uint8_t* outBE) {
    BlockStats st{0.0f, 0.0f, false, true};
    const float step = (g1 - g0) / (float)n;
    float g = g0;
    for (uint32_t i = 0; i < n; i++) {
        if (in[i] != in[0]) st.flat = false;
        float s = dc.process((float)in[i]);
        if (hpf) s = hpf->process(s);
        g += step;
        s *= g;
        const float a = fabsf(s);
        if (a > st.peak) st.peak = a;
        st.sumSquares += s * s;
        if (s > 32767.0f)       { s = 32767.0f;  st.clipped = true; }
        else if (s < -32768.0f) { s = -32768.0f; st.clipped = true; }
        if (outBE) {
            const int16_t v = (int16_t)(int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
            outBE[2 * i]     = (uint8_t)((uint16_t)v >> 8);
            outBE[2 * i + 1] = (uint8_t)((uint16_t)v & 0xFF);
        }
    }
    return st;
}

// Automatic gain control step, applied once per block. Rates are per second so the behaviour
// does not depend on sample rate or block size. Returns the new multiplier.
inline float agcUpdate(float mult, float rms, float blockSeconds) {
    const float TARGET_RMS = 0.15f;    // fraction of full scale (~ -16 dBFS)
    const float ATTACK_PER_S = 0.78f;  // gain reduction (fast)
    const float RELEASE_PER_S = 0.0156f;  // gain increase (slow)
    if (rms <= 0.001f) return mult;    // silence: hold
    const float ratio = TARGET_RMS / rms;
    const float rate = (ratio < 1.0f ? ATTACK_PER_S : RELEASE_PER_S) * blockSeconds;
    mult += (ratio - 1.0f) * rate * mult;
    if (mult < 0.1f) mult = 0.1f;
    if (mult > 10.0f) mult = 10.0f;
    return mult;
}
