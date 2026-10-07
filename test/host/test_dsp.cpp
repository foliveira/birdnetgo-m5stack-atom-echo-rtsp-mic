// Host tests for src/AudioDsp.h. Build and run with test/host/run.sh.
#include "AudioDsp.h"
#include "check.h"

#include <vector>

static const float PI_F = 3.14159265358979f;

static std::vector<int16_t> sine(float fs, float f, float amp, int n, float offset = 0.0f) {
    std::vector<int16_t> v(n);
    for (int i = 0; i < n; i++) v[i] = (int16_t)lrintf(offset + amp * sinf(2.0f * PI_F * f * (float)i / fs));
    return v;
}

// Steady-state gain of the HPF at frequency f, in dB (DC blocker included, as in the device).
static float hpfGainDb(float fs, float fc, float f) {
    const int n = (int)fs;  // 1 s; measure the last half
    std::vector<int16_t> in = sine(fs, f, 10000.0f, n);
    std::vector<uint8_t> out(2 * n);
    DcBlocker dc; dc.design(fs, 10.0f);
    Biquad hpf; designHighpass(hpf, fs, fc);
    processBlock(in.data(), n, dc, &hpf, 1.0f, 1.0f, out.data());
    float peak = 0.0f;
    for (int i = n / 2; i < n; i++) {
        int16_t s = (int16_t)((out[2 * i] << 8) | out[2 * i + 1]);
        if (fabsf((float)s) > peak) peak = fabsf((float)s);
    }
    return 20.0f * log10f(peak / 10000.0f);
}

static void testHighpassResponse() {
    const float rates[] = {16000.0f, 32000.0f, 48000.0f};
    for (float fs : rates) {
        CHECK_NEAR(hpfGainDb(fs, 300.0f, 300.0f), -3.01f, 0.3f);    // corner
        CHECK_NEAR(hpfGainDb(fs, 300.0f, 150.0f), -12.3f, 0.6f);    // one octave below: 12 dB/oct
        CHECK_NEAR(hpfGainDb(fs, 300.0f, 2000.0f), 0.0f, 0.2f);     // passband (bird song)
        CHECK(hpfGainDb(fs, 300.0f, 50.0f) < -30.0f);               // mains hum / rumble
    }
}

static void testDcRemoved() {
    // PDM decoders deliver a DC offset; after a second the output must be centred on zero.
    const float fs = 32000.0f;
    const int n = 32000;
    std::vector<int16_t> in = sine(fs, 1000.0f, 1000.0f, n, -1500.0f);
    std::vector<uint8_t> out(2 * n);
    DcBlocker dc; dc.design(fs, 10.0f);
    processBlock(in.data(), n, dc, nullptr, 1.0f, 1.0f, out.data());
    double sum = 0;
    for (int i = n / 2; i < n; i++) sum += (int16_t)((out[2 * i] << 8) | out[2 * i + 1]);
    CHECK_NEAR((float)(sum / (n / 2)), 0.0f, 5.0f);
}

static void testGainRampRoundingAndByteOrder() {
    // Reference: run an identical DC blocker separately and apply the ramp by hand.
    const int n = 256;
    std::vector<int16_t> in = sine(16000.0f, 1000.0f, 3000.0f, n);
    std::vector<uint8_t> out(2 * n);
    DcBlocker dc, ref; dc.design(16000.0f, 10.0f); ref.design(16000.0f, 10.0f);
    processBlock(in.data(), n, dc, nullptr, 1.0f, 3.0f, out.data());
    const float step = 2.0f / n;
    float g = 1.0f;
    int mismatches = 0;
    for (int i = 0; i < n; i++) {
        g += step;
        const float s = ref.process((float)in[i]) * g;
        const long want = lrintf(s);   // round to nearest
        const int16_t got = (int16_t)((out[2 * i] << 8) | out[2 * i + 1]);
        if (labs(want - got) > 1) mismatches++;   // allow ties / float-order differences
    }
    CHECK_EQ(mismatches, 0);
    CHECK_NEAR(g, 3.0f, 1e-4f);  // the last sample got the target gain

    // Big-endian: 0x1234 must come out high byte first.
    int16_t one[1] = {0x1234};
    uint8_t be[2];
    DcBlocker passthrough; passthrough.r = 1.0f; passthrough.x1 = 0.0f; passthrough.y1 = 0.0f;
    // r=1 and x1=0: first output equals the input exactly.
    processBlock(one, 1, passthrough, nullptr, 1.0f, 1.0f, be);
    CHECK_EQ(be[0], 0x12);
    CHECK_EQ(be[1], 0x34);
}

static void testRoundingIsSymmetric() {
    // Truncation toward zero would map +-0.7 to 0 and bias small signals; rounding must not.
    int16_t in[2] = {7, -7};
    uint8_t out[4];
    DcBlocker dc; dc.r = 1.0f;   // x1=y1=0: y0 = x0, y1 = x1 - x0 + y0 = x1
    processBlock(in, 2, dc, nullptr, 0.1f, 0.1f, out);   // 0.7 and -0.7
    CHECK_EQ((int16_t)((out[0] << 8) | out[1]), 1);
    CHECK_EQ((int16_t)((out[2] << 8) | out[3]), -1);
}

static void testClipping() {
    const int n = 1600;
    std::vector<int16_t> in = sine(16000.0f, 500.0f, 20000.0f, n);
    std::vector<uint8_t> out(2 * n);
    DcBlocker dc; dc.design(16000.0f, 10.0f);
    BlockStats st = processBlock(in.data(), n, dc, nullptr, 4.0f, 4.0f, out.data());
    CHECK(st.clipped);
    CHECK(st.peak > 32767.0f);   // metering sees the pre-clip level
    int16_t lo = 0, hi = 0;
    for (int i = 0; i < n; i++) {
        int16_t s = (int16_t)((out[2 * i] << 8) | out[2 * i + 1]);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    CHECK_EQ(hi, 32767);
    CHECK_EQ(lo, -32768);

    // Metering without an output buffer (no client connected) must not write anything.
    DcBlocker dc2; dc2.design(16000.0f, 10.0f);
    BlockStats st2 = processBlock(in.data(), n, dc2, nullptr, 1.0f, 1.0f, nullptr);
    CHECK(!st2.clipped);
    CHECK(st2.peak > 15000.0f && st2.peak < 21000.0f);
}

static void testFlatInput() {
    // A stuck PDM line decodes to a constant; any working microphone varies by at least 1 LSB.
    const int n = 512;
    std::vector<int16_t> in(n, -32768);
    DcBlocker dc; dc.design(32000.0f, 10.0f);
    CHECK(processBlock(in.data(), n, dc, nullptr, 3.0f, 3.0f, nullptr).flat);
    in[n - 1] = -32767;
    CHECK(!processBlock(in.data(), n, dc, nullptr, 3.0f, 3.0f, nullptr).flat);
    std::vector<int16_t> quiet = sine(32000.0f, 1000.0f, 1.0f, n);   // +-1 LSB of signal
    CHECK(!processBlock(quiet.data(), n, dc, nullptr, 3.0f, 3.0f, nullptr).flat);
}

static void testAgcIndependentOfBlockSize() {
    // One second of a loud signal must reduce the AGC multiplier by the same amount whether the
    // capture task runs 16 ms or 128 ms blocks.
    const float rms = 0.6f;   // well above the target
    float small = 1.0f, large = 1.0f;
    for (int i = 0; i < 62; i++) small = agcUpdate(small, rms, 0.016f);
    for (int i = 0; i < 8; i++) large = agcUpdate(large, rms, 0.128f);
    CHECK(small < 0.75f);
    CHECK_NEAR(small, large, 0.05f);
    // Silence holds the multiplier; quiet input raises it slowly and never past the cap.
    CHECK_EQ(agcUpdate(2.0f, 0.0f, 0.064f), 2.0f);
    float m = 1.0f;
    for (int i = 0; i < 100000; i++) m = agcUpdate(m, 0.01f, 0.064f);
    CHECK_EQ(m, 10.0f);
}

int main() {
    testHighpassResponse();
    testDcRemoved();
    testGainRampRoundingAndByteOrder();
    testRoundingIsSymmetric();
    testClipping();
    testFlatInput();
    testAgcIndependentOfBlockSize();
    return checkSummary("test_dsp");
}
