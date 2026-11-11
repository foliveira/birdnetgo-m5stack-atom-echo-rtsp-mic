// Host tests for src/SettingsParse.h. Build and run with test/host/run.sh.
#include "SettingsParse.h"
#include "check.h"

#include <initializer_list>
#include <string.h>

static Settings defaults() {
    Settings s;
    memset(&s, 0, sizeof(s));
    s.rate = 32000;
    s.buffer = 1024;
    s.gain = 3;
    s.hpf = true;
    s.hpfHz = 300;
    s.agc = false;
    s.tx = 19.5f;
    s.ps = false;
    s.cpu = 80;
    s.led = 1;
    s.oh = true;
    s.ohLimit = 80;
    s.sched = false;
    s.schedH = 24;
    strcpy(s.name, "atomecho");
    return s;
}

static Settings s = defaults();

static bool accepts(const char* key, const char* v) {
    const char* e = parseSetting(key, v, s);
    if (e) printf("  %s=\"%s\" rejected: %s\n", key, v, e);
    return e == nullptr;
}

// Rejected with exactly this message, and s left as it was.
static bool rejects(const char* key, const char* v, const char* message) {
    Settings before;
    memcpy(&before, &s, sizeof(s));
    const char* e = parseSetting(key, v, s);
    const bool unchanged = memcmp(&before, &s, sizeof(s)) == 0;
    if (!e || strcmp(e, message) != 0 || !unchanged)
        printf("  %s=\"%s\": got \"%s\"%s\n", key, v, e ? e : "(accepted)", unchanged ? "" : ", settings changed");
    return e && strcmp(e, message) == 0 && unchanged;
}

static void testRate() {
    const char* msg = "must be 16000, 22050, 24000, 32000, 44100 or 48000";
    CHECK(accepts("rate", "16000")); CHECK_EQ(s.rate, 16000);
    CHECK(accepts("rate", "48000")); CHECK_EQ(s.rate, 48000);
    for (const char* r : {"22050", "24000", "32000", "44100"}) CHECK(accepts("rate", r));
    CHECK_EQ(s.rate, 44100);
    CHECK(rejects("rate", "15999", msg));
    CHECK(rejects("rate", "48001", msg));
    CHECK(rejects("rate", "8000", msg));
    CHECK(rejects("rate", "20000", msg));   // in range, not a rate the microphone runs at
    CHECK(rejects("rate", "44100.5", msg));
}

static void testBuffer() {
    const char* msg = "must be 256 to 4096 samples";
    CHECK(accepts("buffer", "256")); CHECK_EQ(s.buffer, 256);
    CHECK(accepts("buffer", "4096")); CHECK_EQ(s.buffer, 4096);
    CHECK(rejects("buffer", "255", msg));
    CHECK(rejects("buffer", "4097", msg));
    CHECK(rejects("buffer", "8192", msg));
    CHECK(rejects("buffer", "3.5", msg));
    CHECK(accepts("buffer", "1024")); CHECK_EQ(s.buffer, 1024);
}

static void testGain() {
    const char* msg = "must be 0.1 to 100";
    CHECK(accepts("gain", "0.1")); CHECK_EQ(s.gain, 0.1f);
    CHECK(accepts("gain", "100")); CHECK_EQ(s.gain, 100.0f);
    CHECK(accepts("gain", "3.25")); CHECK_EQ(s.gain, 3.25f);
    CHECK(rejects("gain", "0.09", msg));
    CHECK(rejects("gain", "100.01", msg));
    CHECK(rejects("gain", "-3", msg));
}

static void testHighpass() {
    const char* msg = "must be 10 to 10000 Hz";
    CHECK(accepts("hpf_hz", "10")); CHECK_EQ(s.hpfHz, 10);
    CHECK(accepts("hpf_hz", "10000")); CHECK_EQ(s.hpfHz, 10000);
    CHECK(rejects("hpf_hz", "9", msg));
    CHECK(rejects("hpf_hz", "10001", msg));
    CHECK(rejects("hpf_hz", "300.5", msg));
}

static void testTxPower() {
    const char* msg = "must be -1 to 19.5 dBm";
    CHECK(accepts("tx", "-1")); CHECK_EQ(s.tx, -1.0f);
    CHECK(accepts("tx", "19.5")); CHECK_EQ(s.tx, 19.5f);
    CHECK(accepts("tx", "10")); CHECK_EQ(s.tx, 11.0f);
    CHECK(accepts("tx", "6")); CHECK_EQ(s.tx, 5.0f);   // halfway between 5 and 7
    CHECK(accepts("tx", "8.5")); CHECK_EQ(s.tx, 8.5f);
    CHECK(rejects("tx", "-1.5", msg));
    CHECK(rejects("tx", "19.6", msg));
    CHECK(rejects("tx", "20", msg));
}

static void testCpu() {
    const char* msg = "must be 80, 160 or 240";
    CHECK(accepts("cpu", "80")); CHECK_EQ(s.cpu, 80);
    CHECK(accepts("cpu", "240")); CHECK_EQ(s.cpu, 240);
    CHECK(accepts("cpu", "160")); CHECK_EQ(s.cpu, 160);
    CHECK(rejects("cpu", "79", msg));
    CHECK(rejects("cpu", "241", msg));
    CHECK(rejects("cpu", "120", msg));   // not a valid PLL setting
    CHECK(rejects("cpu", "40", msg));
}

static void testLed() {
    const char* msg = "must be 0, 1 or 2";
    CHECK(accepts("led", "0")); CHECK_EQ(s.led, 0);
    CHECK(accepts("led", "2")); CHECK_EQ(s.led, 2);
    CHECK(rejects("led", "-1", msg));
    CHECK(rejects("led", "3", msg));
    CHECK(rejects("led", "1.5", msg));
}

static void testOverheatLimit() {
    const char* msg = "must be 30 to 95";
    CHECK(accepts("oh_limit", "30")); CHECK_EQ(s.ohLimit, 30);
    CHECK(accepts("oh_limit", "95")); CHECK_EQ(s.ohLimit, 95);
    CHECK(accepts("oh_limit", "84")); CHECK_EQ(s.ohLimit, 80);   // down to a multiple of 5
    CHECK(accepts("oh_limit", "31")); CHECK_EQ(s.ohLimit, 30);
    CHECK(accepts("oh_limit", "94")); CHECK_EQ(s.ohLimit, 90);
    CHECK(rejects("oh_limit", "29", msg));
    CHECK(rejects("oh_limit", "96", msg));
}

static void testScheduledRestart() {
    const char* msg = "must be 1 to 168 hours";
    CHECK(accepts("sched_h", "1")); CHECK_EQ(s.schedH, 1);
    CHECK(accepts("sched_h", "168")); CHECK_EQ(s.schedH, 168);
    CHECK(rejects("sched_h", "0", msg));
    CHECK(rejects("sched_h", "169", msg));
}

// Only decimal numbers: what strtod alone would also take is refused.
static void testStrictNumbers() {
    const char* msg = "must be 256 to 4096 samples";
    for (const char* v : {"0x400", " 1024", "1024 ", "\t1024", "1024\n", "inf", "-inf", "INF", "nan", "NAN",
                          "", "+", "-", ".", "e", "1e", "1e+", "12abc", "1e999", "1,024", "1024.0.0", "--1024"})
        CHECK(rejects("buffer", v, msg));
    CHECK(rejects("hpf_hz", "3.5", "must be 10 to 10000 Hz"));
    CHECK(rejects("gain", "1e999", "must be 0.1 to 100"));
    CHECK(accepts("hpf_hz", "1e2")); CHECK_EQ(s.hpfHz, 100);
    CHECK(accepts("buffer", "+512")); CHECK_EQ(s.buffer, 512);
    CHECK(accepts("buffer", "2048.0")); CHECK_EQ(s.buffer, 2048);
    CHECK(accepts("gain", ".5")); CHECK_EQ(s.gain, 0.5f);
    CHECK(accepts("gain", "2.5E1")); CHECK_EQ(s.gain, 25.0f);

    double d = 7;
    CHECK(parseNum("10", 10, 20, true, d)); CHECK_EQ(d, 10);
    CHECK(parseNum("20", 10, 20, true, d)); CHECK_EQ(d, 20);
    CHECK(!parseNum("20.5", 10, 30, true, d)); CHECK_EQ(d, 20);   // out is written only on success
    CHECK(parseNum("20.5", 10, 30, false, d)); CHECK_EQ(d, 20.5);
    CHECK(!parseNum("0x10", 0, 100, true, d));
    CHECK(!parseNum("nan", -1e9, 1e9, false, d));
}

static void testFlags() {
    struct { const char* key; bool Settings::*field; } flags[] = {
        {"hpf", &Settings::hpf}, {"agc", &Settings::agc}, {"ps", &Settings::ps},
        {"oh", &Settings::oh}, {"sched", &Settings::sched}};
    for (const auto& f : flags) {
        for (const char* word : {"1", "on", "true"}) {
            s.*f.field = false;
            CHECK(accepts(f.key, word));
            CHECK(s.*f.field);
        }
        for (const char* word : {"0", "off", "false"}) {
            s.*f.field = true;
            CHECK(accepts(f.key, word));
            CHECK(!(s.*f.field));
        }
        for (const char* word : {"yes", "", "TRUE", "On", "2", " 1"}) CHECK(rejects(f.key, word, "must be 1 or 0"));
    }
}

static void testName() {
    const char* msg = "must be 1 to 31 letters, digits or hyphens, with no hyphen at either end";
    CHECK(accepts("name", "Garden-Mic-2")); CHECK(strcmp(s.name, "garden-mic-2") == 0);
    CHECK(accepts("name", "a")); CHECK(strcmp(s.name, "a") == 0);

    char name[2001];
    memset(name, 'A', sizeof(name) - 1);
    name[31] = '\0';
    CHECK(accepts("name", name)); CHECK_EQ(strlen(s.name), 31); CHECK(s.name[30] == 'a');
    name[31] = 'A'; name[32] = '\0';
    CHECK(rejects("name", name, msg));
    name[32] = 'A'; name[33] = '\0';
    CHECK(rejects("name", name, msg));
    name[sizeof(name) - 1] = '\0';
    for (size_t i = 33; i < sizeof(name) - 1; i++) name[i] = 'A';
    CHECK(rejects("name", name, msg));

    for (const char* v : {"", "-mic", "mic-", "-", "garden_mic", "garden mic", "garden.mic", "caf\xc3\xa9", "\xc3\x89"})
        CHECK(rejects("name", v, msg));
    CHECK(accepts("name", "atomecho")); CHECK(strcmp(s.name, "atomecho") == 0);
}

static void testUnknownKey() {
    for (const char* key : {"volume", "", "RATE", "rate ", "hpfhz"}) CHECK(rejects(key, "1", "unknown setting"));
}

static void testSampleRates() {
    for (uint32_t hz : {16000u, 22050u, 24000u, 32000u, 44100u, 48000u}) CHECK(isSupportedSampleRate(hz));
    for (uint32_t hz : {0u, 8000u, 15999u, 20000u, 48001u, 96000u}) CHECK(!isSupportedSampleRate(hz));
}

static void testDeviceNames() {
    for (const char* n : {"atomecho", "a", "0", "a-b", "garden-mic-2", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
        CHECK(isValidDeviceName(n));
    for (const char* n : {"", "-a", "a-", "-", "AtomEcho", "a_b", "a.b", "a b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
        CHECK(!isValidDeviceName(n));
}

static void testWifiTxSteps() {
    for (float step : {-1.0f, 2.0f, 5.0f, 7.0f, 8.5f, 11.0f, 13.0f, 15.0f, 17.0f, 18.5f, 19.0f, 19.5f})
        CHECK_EQ(snapWifiTxDbm(step), step);
    CHECK_EQ(snapWifiTxDbm(10), 11);
    CHECK_EQ(snapWifiTxDbm(6), 5);         // ties go to the lower step
    CHECK_EQ(snapWifiTxDbm(0.5f), -1);
    CHECK_EQ(snapWifiTxDbm(19.25f), 19);
    CHECK_EQ(snapWifiTxDbm(12.1f), 13);
    CHECK_EQ(snapWifiTxDbm(-10), -1);
    CHECK_EQ(snapWifiTxDbm(30), 19.5);
}

int main() {
    testRate();
    testBuffer();
    testGain();
    testHighpass();
    testTxPower();
    testCpu();
    testLed();
    testOverheatLimit();
    testScheduledRestart();
    testStrictNumbers();
    testFlags();
    testName();
    testUnknownKey();
    testSampleRates();
    testDeviceNames();
    testWifiTxSteps();
    return checkSummary("test_settings");
}
