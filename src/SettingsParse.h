#pragma once
// Limits of the settings and validation of POST /api/settings. Plain C++, tested on the host by
// test/host/test_settings.cpp.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5

// Samples per RTP frame (the "buffer" setting). The page offers 256 to 4096; a frame pool of
// 8192-sample frames does not fit in the Atom Echo's memory.
#define MIN_BUFFER_SIZE 256
#define MAX_BUFFER_SIZE 4096

// Sample rates the PDM microphone runs at: its clock is 64 x the rate and must stay within
// the SPM1423's 1.0-3.25 MHz.
inline bool isSupportedSampleRate(uint32_t hz) {
    return hz == 16000 || hz == 22050 || hz == 24000 || hz == 32000 || hz == 44100 || hz == 48000;
}

// A DNS label: 1-31 lowercase letters, digits and hyphens, no hyphen at either end. 31 is what
// the core's hostname buffer (32 bytes) keeps.
inline bool isValidDeviceName(const char* name) {
    const size_t n = strlen(name);
    if (n < 1 || n > 31 || name[0] == '-' || name[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

// Nearest transmit power step the radio supports, in dBm. A value halfway between two steps gets
// the lower one.
inline float snapWifiTxDbm(float dbm) {
    static constexpr float steps[] = {-1, 2, 5, 7, 8.5f, 11, 13, 15, 17, 18.5f, 19, 19.5f};
    float best = steps[0];
    for (const float s : steps) {
        if (fabsf(s - dbm) < fabsf(best - dbm)) best = s;
    }
    return best;
}

struct Settings {
    uint32_t rate;
    uint16_t buffer;
    float gain;
    bool hpf;
    uint16_t hpfHz;
    bool agc;
    float tx;
    bool ps;
    uint8_t cpu;
    uint8_t led;
    bool oh;
    uint8_t ohLimit;
    bool sched;
    uint16_t schedH;
    char name[33];
};

inline bool parseBool(const char* v, bool& out) {
    if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true")) { out = true; return true; }
    if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false")) { out = false; return true; }
    return false;
}

// A decimal number within [lo, hi]; integral if asked. Rejects empty text, trailing text ("12abc"),
// blanks, hex, "inf" and "nan": only digits, signs, a point and an exponent get to strtod.
inline bool parseNum(const char* v, double lo, double hi, bool integral, double& out) {
    if (*v == '\0' || v[strspn(v, "0123456789+-.eE")] != '\0') return false;
    char* end = nullptr;
    const double d = strtod(v, &end);
    if (end == v || *end != '\0' || !isfinite(d) || d < lo || d > hi) return false;
    if (integral && d != floor(d)) return false;
    out = d;
    return true;
}

// Parses one key into s. Returns an error message, or nullptr. On an error s is unchanged.
inline const char* parseSetting(const char* key, const char* v, Settings& s) {
    const auto is = [key](const char* name) { return strcmp(key, name) == 0; };
    double d = 0;
    if (is("rate")) {
        if (!parseNum(v, 16000, 48000, true, d) || !isSupportedSampleRate((uint32_t)d))
            return "must be 16000, 22050, 24000, 32000, 44100 or 48000";
        s.rate = (uint32_t)d;
    } else if (is("buffer")) {
        if (!parseNum(v, MIN_BUFFER_SIZE, MAX_BUFFER_SIZE, true, d)) return "must be 256 to 4096 samples";
        s.buffer = (uint16_t)d;
    } else if (is("gain")) {
        if (!parseNum(v, 0.1, 100, false, d)) return "must be 0.1 to 100";
        s.gain = (float)d;
    } else if (is("hpf_hz")) {
        if (!parseNum(v, 10, 10000, true, d)) return "must be 10 to 10000 Hz";
        s.hpfHz = (uint16_t)d;
    } else if (is("tx")) {
        if (!parseNum(v, -1, 19.5, false, d)) return "must be -1 to 19.5 dBm";
        s.tx = snapWifiTxDbm((float)d);
    } else if (is("cpu")) {
        if (!parseNum(v, 80, 240, true, d) || (d != 80 && d != 160 && d != 240)) return "must be 80, 160 or 240";
        s.cpu = (uint8_t)d;
    } else if (is("led")) {
        if (!parseNum(v, 0, 2, true, d)) return "must be 0, 1 or 2";
        s.led = (uint8_t)d;
    } else if (is("oh_limit")) {
        if (!parseNum(v, OVERHEAT_MIN_LIMIT_C, OVERHEAT_MAX_LIMIT_C, true, d)) return "must be 30 to 95";
        s.ohLimit = (uint8_t)(OVERHEAT_MIN_LIMIT_C +
                              ((uint32_t)d - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C * OVERHEAT_LIMIT_STEP_C);
    } else if (is("sched_h")) {
        if (!parseNum(v, 1, 168, true, d)) return "must be 1 to 168 hours";
        s.schedH = (uint16_t)d;
    } else if (is("name")) {
        // Lower-cased copy. Text that does not fit is refused, not cut.
        char n[sizeof(s.name)];
        size_t len = 0;
        while (v[len] && len < sizeof(n) - 1) {
            const char c = v[len];
            n[len++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        n[len] = '\0';
        if (v[len] != '\0' || !isValidDeviceName(n))
            return "must be 1 to 31 letters, digits or hyphens, with no hyphen at either end";
        memcpy(s.name, n, len + 1);
    } else {
        bool* flag = is("hpf") ? &s.hpf : is("agc") ? &s.agc : is("ps") ? &s.ps :
                     is("oh") ? &s.oh : is("sched") ? &s.sched : nullptr;
        if (!flag) return "unknown setting";
        if (!parseBool(v, *flag)) return "must be 1 or 0";
    }
    return nullptr;
}
