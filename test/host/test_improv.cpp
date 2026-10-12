// Host tests for src/ImprovSerial.h. Build and run with test/host/run.sh.
#include "ImprovSerial.h"
#include "check.h"

#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

struct Fake : improv::Hooks {
    Bytes out;
    bool on = false;
    bool joinWorks = true;
    std::string ssid, password;
    int joinedCalls = 0;
    const char* strings[4] = {"Atom Echo RTSP Mic for BirdNET-Go", "3.2.0", "ESP32", "atomecho"};

    void write(const uint8_t* p, size_t n) override { out.insert(out.end(), p, p + n); }
    bool provisioned() override { return on; }
    void url(char* o, size_t cap) override { snprintf(o, cap, "http://192.168.1.50/"); }
    const char* const* info() override { return strings; }
    int networks(improv::Network* o, int max) override {
        if (max < 2) return 0;
        snprintf(o[0].ssid, sizeof(o[0].ssid), "home");
        o[0].rssi = -48;
        o[0].secured = true;
        snprintf(o[1].ssid, sizeof(o[1].ssid), "cafe");
        o[1].rssi = -81;
        o[1].secured = false;
        return 2;
    }
    bool join(const char* s, const char* p) override {
        ssid = s;
        password = p;
        on = joinWorks;
        return joinWorks;
    }
    void joined() override { joinedCalls++; }
};

// An RPC as the browser client writes it.
static Bytes rpc(uint8_t command, const Bytes& args, uint8_t version = 1) {
    Bytes p = {'I', 'M', 'P', 'R', 'O', 'V', version, improv::TYPE_RPC, (uint8_t)(args.size() + 2), command,
               (uint8_t)args.size()};
    p.insert(p.end(), args.begin(), args.end());
    uint8_t sum = 0;
    for (uint8_t c : p) sum += c;
    p.push_back(sum);
    p.push_back('\n');
    return p;
}

static Bytes wifiArgs(const std::string& ssid, const std::string& password) {
    Bytes a = {(uint8_t)ssid.size()};
    a.insert(a.end(), ssid.begin(), ssid.end());
    a.push_back((uint8_t)password.size());
    a.insert(a.end(), password.begin(), password.end());
    return a;
}

struct Packet {
    uint8_t type;
    Bytes data;
};

// The device's output as the browser client reads it: a packet counts only at the start of a
// line, with version 1 and a correct checksum.
static std::vector<Packet> packets(const Bytes& b) {
    std::vector<Packet> r;
    size_t i = 0;
    while (i < b.size()) {
        if (b[i] == '\n') { i++; continue; }
        if (i + 9 <= b.size() && memcmp(&b[i], "IMPROV", 6) == 0 && i + 10 + b[i + 8] <= b.size()) {
            const size_t len = b[i + 8];
            uint8_t sum = 0;
            for (size_t k = i; k < i + 9 + len; k++) sum += b[k];
            CHECK_EQ(b[i + 6], 1);
            CHECK_EQ(sum, b[i + 9 + len]);
            r.push_back({b[i + 7], Bytes(b.begin() + i + 9, b.begin() + i + 9 + len)});
            i += 10 + len;
            continue;
        }
        while (i < b.size() && b[i] != '\n') i++;
    }
    return r;
}

// The strings of an RPC result, after checking its command and length bytes.
static std::vector<std::string> strings(const Packet& p, uint8_t command) {
    std::vector<std::string> r;
    CHECK_EQ(p.type, improv::TYPE_RESULT);
    if (p.data.size() < 2) return r;
    CHECK_EQ(p.data[0], command);
    CHECK_EQ(p.data[1], p.data.size() - 2);
    size_t i = 2;
    while (i < p.data.size()) {
        const size_t len = p.data[i];
        r.push_back(std::string(p.data.begin() + i + 1, p.data.begin() + i + 1 + len));
        i += 1 + len;
    }
    return r;
}

static bool isState(const Packet& p, uint8_t s) { return p.type == improv::TYPE_STATE && p.data == Bytes{s}; }
static bool isError(const Packet& p, uint8_t e) { return p.type == improv::TYPE_ERROR && p.data == Bytes{e}; }

static std::vector<Packet> send(Fake& f, improv::Device& d, const Bytes& in) {
    f.out.clear();
    for (uint8_t b : in) d.feed(b);
    return packets(f.out);
}

static void testPacketBytes() {
    uint8_t p[16];
    const uint8_t ready = improv::STATE_READY;
    const size_t n = improv::packet(p, sizeof(p), improv::TYPE_STATE, &ready, 1);
    // Checksum: 'I'+'M'+'P'+'R'+'O'+'V' = 477, + version, type, length (1 each) + 2 = 482 = 0xE2.
    const Bytes expected = {'\n', 'I', 'M', 'P', 'R', 'O', 'V', 1, 1, 1, 2, 0xE2, '\n'};
    CHECK(Bytes(p, p + n) == expected);
    CHECK_EQ(improv::packet(p, 12, improv::TYPE_STATE, &ready, 1), 0);   // does not fit
}

static void testState() {
    Fake f;
    improv::Device d(f);
    auto r = send(f, d, rpc(improv::CMD_GET_STATE, {}));
    CHECK_EQ(r.size(), 2);
    if (r.size() == 2) {
        CHECK(isError(r[0], improv::ERROR_NONE));
        CHECK(isState(r[1], improv::STATE_READY));
    }

    f.on = true;
    r = send(f, d, rpc(improv::CMD_GET_STATE, {}));
    CHECK_EQ(r.size(), 3);
    if (r.size() == 3) {
        CHECK(isState(r[1], improv::STATE_PROVISIONED));
        const auto s = strings(r[2], improv::CMD_GET_STATE);
        CHECK(s == std::vector<std::string>{"http://192.168.1.50/"});
    }
}

static void testInfoAndNetworks() {
    Fake f;
    improv::Device d(f);
    auto r = send(f, d, rpc(improv::CMD_GET_INFO, {}));
    CHECK_EQ(r.size(), 2);
    if (r.size() == 2) {
        const auto s = strings(r[1], improv::CMD_GET_INFO);
        CHECK(s == (std::vector<std::string>{"Atom Echo RTSP Mic for BirdNET-Go", "3.2.0", "ESP32", "atomecho"}));
    }

    r = send(f, d, rpc(improv::CMD_GET_NETWORKS, {}));
    CHECK_EQ(r.size(), 4);
    if (r.size() == 4) {
        CHECK(strings(r[1], improv::CMD_GET_NETWORKS) == (std::vector<std::string>{"home", "-48", "YES"}));
        CHECK(strings(r[2], improv::CMD_GET_NETWORKS) == (std::vector<std::string>{"cafe", "-81", "NO"}));
        CHECK(strings(r[3], improv::CMD_GET_NETWORKS).empty());   // ends the list
    }
}

static void testWifiSettings() {
    Fake f;
    improv::Device d(f);
    auto r = send(f, d, rpc(improv::CMD_WIFI_SETTINGS, wifiArgs("home", "secret123")));
    CHECK(f.ssid == "home");
    CHECK(f.password == "secret123");
    CHECK_EQ(f.joinedCalls, 1);
    CHECK_EQ(r.size(), 4);
    if (r.size() == 4) {
        CHECK(isError(r[0], improv::ERROR_NONE));
        CHECK(isState(r[1], improv::STATE_PROVISIONING));
        CHECK(isState(r[2], improv::STATE_PROVISIONED));
        CHECK(strings(r[3], improv::CMD_WIFI_SETTINGS) == std::vector<std::string>{"http://192.168.1.50/"});
    }

    Fake g;
    g.joinWorks = false;
    improv::Device e(g);
    r = send(g, e, rpc(improv::CMD_WIFI_SETTINGS, wifiArgs("home", "")));   // open network
    CHECK(g.password.empty());
    CHECK_EQ(g.joinedCalls, 0);
    CHECK_EQ(r.size(), 4);
    if (r.size() == 4) {
        CHECK(isState(r[1], improv::STATE_PROVISIONING));
        CHECK(isError(r[2], improv::ERROR_UNABLE_TO_CONNECT));
        CHECK(isState(r[3], improv::STATE_READY));
    }

    // Malformed: the SSID length runs past the data.
    Fake h;
    improv::Device k(h);
    Bytes bad = wifiArgs("home", "secret123");
    bad[0] = 40;
    r = send(h, k, rpc(improv::CMD_WIFI_SETTINGS, bad));
    CHECK(h.ssid.empty());
    CHECK_EQ(r.size(), 2);
    if (r.size() == 2) CHECK(isError(r[1], improv::ERROR_INVALID_RPC));
}

static void testFraming() {
    Fake f;
    improv::Device d(f);

    // Log noise, a false start and a repeated 'I' before the packet.
    std::string noise = "boot log line\nIMPRO IIMPROV";
    Bytes in(noise.begin(), noise.end() - 6);
    const Bytes p = rpc(improv::CMD_GET_STATE, {});
    in.insert(in.end(), p.begin(), p.end());
    auto r = send(f, d, in);
    CHECK_EQ(r.size(), 2);

    // Two packets back to back.
    Bytes two = rpc(improv::CMD_GET_STATE, {});
    const Bytes info = rpc(improv::CMD_GET_INFO, {});
    two.insert(two.end(), info.begin(), info.end());
    r = send(f, d, two);
    CHECK_EQ(r.size(), 4);

    // Bad checksum.
    Bytes corrupt = rpc(improv::CMD_GET_STATE, {});
    corrupt[corrupt.size() - 2] ^= 0x55;
    r = send(f, d, corrupt);
    CHECK_EQ(r.size(), 1);
    if (r.size() == 1) CHECK(isError(r[0], improv::ERROR_INVALID_RPC));

    // Unsupported version.
    r = send(f, d, rpc(improv::CMD_GET_STATE, {}, 2));
    CHECK_EQ(r.size(), 1);
    if (r.size() == 1) CHECK(isError(r[0], improv::ERROR_INVALID_RPC));

    // Unknown command.
    r = send(f, d, rpc(0x7F, {}));
    CHECK_EQ(r.size(), 2);
    if (r.size() == 2) CHECK(isError(r[1], improv::ERROR_UNKNOWN_RPC));

    // A packet of another type is not answered, and the next one still is.
    Bytes state = rpc(improv::CMD_GET_STATE, {});
    state[7] = improv::TYPE_STATE;
    uint8_t sum = 0;
    for (size_t i = 0; i < state.size() - 2; i++) sum += state[i];
    state[state.size() - 2] = sum;
    CHECK_EQ(send(f, d, state).size(), 0);
    CHECK_EQ(send(f, d, rpc(improv::CMD_GET_STATE, {})).size(), 2);

    // A maximal packet (255 bytes of data) fits the receive buffer.
    Bytes big = rpc(improv::CMD_GET_INFO, Bytes(253, 'x'));
    r = send(f, d, big);
    CHECK_EQ(r.size(), 2);
}

static void testLimits() {
    char ssid[33], pass[65];
    std::string s32(32, 's'), p64(64, 'p');
    Bytes a = wifiArgs(s32, p64);
    CHECK(improv::parseWifiSettings(a.data(), a.size(), ssid, sizeof(ssid), pass, sizeof(pass)));
    CHECK(std::string(ssid) == s32);
    CHECK(std::string(pass) == p64);
    a = wifiArgs(std::string(33, 's'), "");
    CHECK(!improv::parseWifiSettings(a.data(), a.size(), ssid, sizeof(ssid), pass, sizeof(pass)));
    a = wifiArgs("home", std::string(65, 'p'));
    CHECK(!improv::parseWifiSettings(a.data(), a.size(), ssid, sizeof(ssid), pass, sizeof(pass)));
    a = wifiArgs("", "secret123");
    CHECK(!improv::parseWifiSettings(a.data(), a.size(), ssid, sizeof(ssid), pass, sizeof(pass)));
    a = {4, 'h', 'o', 'm', 'e'};   // no password length byte
    CHECK(!improv::parseWifiSettings(a.data(), a.size(), ssid, sizeof(ssid), pass, sizeof(pass)));

    uint8_t d[300];
    const std::string long1(200, 'a'), long2(60, 'b');
    const char* fits[] = {long1.c_str()};
    CHECK_EQ(improv::resultData(d, sizeof(d), 3, fits, 1), 203);
    const char* tooLong[] = {long1.c_str(), long2.c_str()};
    CHECK_EQ(improv::resultData(d, sizeof(d), 3, tooLong, 2), 0);   // over one packet
}

int main() {
    testPacketBytes();
    testState();
    testInfoAndNetworks();
    testWifiSettings();
    testFraming();
    testLimits();
    return checkSummary("test_improv");
}
