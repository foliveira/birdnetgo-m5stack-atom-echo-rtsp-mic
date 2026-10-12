#pragma once
// Improv Wi-Fi over the serial port (https://www.improv-wifi.com/serial/): how ESP Web Tools, the
// browser flasher, hands WiFi credentials to a device right after flashing it. This is the
// protocol only; WifiSetup.cpp connects it to the serial port and the WiFi driver. Plain C++,
// tested on the host by test/host/test_improv.cpp.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace improv {

enum : uint8_t { TYPE_STATE = 0x01, TYPE_ERROR = 0x02, TYPE_RPC = 0x03, TYPE_RESULT = 0x04 };
enum : uint8_t { STATE_READY = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum : uint8_t { ERROR_NONE = 0x00, ERROR_INVALID_RPC = 0x01, ERROR_UNKNOWN_RPC = 0x02, ERROR_UNABLE_TO_CONNECT = 0x03 };
enum : uint8_t { CMD_WIFI_SETTINGS = 0x01, CMD_GET_STATE = 0x02, CMD_GET_INFO = 0x03, CMD_GET_NETWORKS = 0x04 };

struct Network {
    char ssid[33];
    int rssi;
    bool secured;
};

// What the protocol needs from the device.
class Hooks {
public:
    virtual ~Hooks() {}
    virtual void write(const uint8_t* p, size_t n) = 0;   // to the serial port, in one piece
    virtual bool provisioned() = 0;                        // on a network, with an address
    virtual void url(char* out, size_t cap) = 0;           // the Web UI, e.g. "http://192.168.1.50/"
    virtual const char* const* info() = 0;                 // firmware, version, chip, device name
    virtual int networks(Network* out, int max) = 0;       // nearby networks, strongest first
    virtual bool join(const char* ssid, const char* password) = 0;   // blocks until joined or failed
    virtual void joined() = 0;                             // after the client has been told
};

// One packet: a newline first, because the client only looks for packets at the start of a line,
// then "IMPROV", version 1, type, length, data, checksum (sum of all bytes before it) and a final
// newline. Returns the bytes written, 0 if it does not fit.
inline size_t packet(uint8_t* out, size_t cap, uint8_t type, const uint8_t* data, size_t len) {
    if (len > 255 || cap < len + 12) return 0;
    size_t n = 0;
    out[n++] = '\n';
    memcpy(out + n, "IMPROV", 6);
    n += 6;
    out[n++] = 1;
    out[n++] = type;
    out[n++] = (uint8_t)len;
    if (len) memcpy(out + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 1; i < n; i++) sum += out[i];
    out[n++] = sum;
    out[n++] = '\n';
    return n;
}

// Data of an RPC result: command, length of the rest, then each string with a length byte.
// Returns its length, 0 if it does not fit in `cap` or in one packet (255 bytes).
inline size_t resultData(uint8_t* out, size_t cap, uint8_t command, const char* const* strings, int count) {
    size_t n = 2;
    if (cap < n) return 0;
    for (int i = 0; i < count; i++) {
        const size_t len = strlen(strings[i]);
        if (n + 1 + len > cap || n + 1 + len > 255) return 0;
        out[n++] = (uint8_t)len;
        memcpy(out + n, strings[i], len);
        n += len;
    }
    out[0] = command;
    out[1] = (uint8_t)(n - 2);
    return n;
}

// The SSID and password of a "send WiFi settings" RPC.
inline bool parseWifiSettings(const uint8_t* args, size_t len, char* ssid, size_t ssidCap, char* pass, size_t passCap) {
    if (len < 1) return false;
    const size_t s = args[0];
    if (1 + s + 1 > len || s == 0 || s >= ssidCap) return false;
    const size_t p = args[1 + s];
    if (2 + s + p > len || p >= passCap) return false;
    memcpy(ssid, args + 1, s);
    ssid[s] = '\0';
    memcpy(pass, args + 2 + s, p);
    pass[p] = '\0';
    return true;
}

// Collects bytes from the serial port, recognises RPC packets and answers them through Hooks.
// Anything else on the line (stray bytes, a terminal typing) is ignored.
class Device {
public:
    explicit Device(Hooks& h) : h_(h) {}

    void feed(uint8_t b) {
        if (n_ < 6) {                          // matching the header
            if (b == (uint8_t)"IMPROV"[n_]) buf_[n_++] = b;
            else n_ = (b == 'I') ? (buf_[0] = b, 1) : 0;
            return;
        }
        buf_[n_++] = b;
        if (n_ < 9) return;                    // version, type, length
        if (n_ < 9 + (size_t)buf_[8] + 1) return;
        const size_t len = buf_[8];
        uint8_t sum = 0;
        for (size_t i = 0; i < 9 + len; i++) sum += buf_[i];
        const bool ok = buf_[6] == 1 && sum == buf_[9 + len];
        const uint8_t type = buf_[7];
        n_ = 0;
        if (!ok) { sendError(ERROR_INVALID_RPC); return; }
        if (type != TYPE_RPC) return;
        const uint8_t* d = buf_ + 9;
        if (len < 2 || (size_t)d[1] + 2 > len) { sendError(ERROR_INVALID_RPC); return; }
        rpc(d[0], d + 2, d[1]);
    }

private:
    void send(uint8_t type, const uint8_t* data, size_t len) {
        uint8_t p[270];
        const size_t n = packet(p, sizeof(p), type, data, len);
        if (n) h_.write(p, n);
    }
    void sendState(uint8_t s) { send(TYPE_STATE, &s, 1); }
    void sendError(uint8_t e) { send(TYPE_ERROR, &e, 1); }
    void sendResult(uint8_t command, const char* const* strings, int count) {
        uint8_t d[258];
        const size_t n = resultData(d, sizeof(d), command, strings, count);
        if (n) send(TYPE_RESULT, d, n);
    }
    void sendUrl(uint8_t command) {
        char u[96];
        h_.url(u, sizeof(u));
        const char* s[] = {u};
        sendResult(command, s, 1);
    }

    void rpc(uint8_t command, const uint8_t* args, size_t len) {
        sendError(ERROR_NONE);                 // a new command clears the last error
        if (command == CMD_GET_STATE) {
            const bool on = h_.provisioned();
            sendState(on ? STATE_PROVISIONED : STATE_READY);
            if (on) sendUrl(CMD_GET_STATE);
        } else if (command == CMD_GET_INFO) {
            sendResult(CMD_GET_INFO, h_.info(), 4);
        } else if (command == CMD_GET_NETWORKS) {
            Network nets[20];
            const int count = h_.networks(nets, 20);
            for (int i = 0; i < count; i++) {
                char rssi[8];
                snprintf(rssi, sizeof(rssi), "%d", nets[i].rssi);
                const char* s[] = {nets[i].ssid, rssi, nets[i].secured ? "YES" : "NO"};
                sendResult(CMD_GET_NETWORKS, s, 3);
            }
            sendResult(CMD_GET_NETWORKS, nullptr, 0);   // an empty result ends the list
        } else if (command == CMD_WIFI_SETTINGS) {
            char ssid[33], pass[65];
            if (!parseWifiSettings(args, len, ssid, sizeof(ssid), pass, sizeof(pass))) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            sendState(STATE_PROVISIONING);
            if (h_.join(ssid, pass)) {
                sendState(STATE_PROVISIONED);
                sendUrl(CMD_WIFI_SETTINGS);
                h_.joined();
            } else {
                sendError(ERROR_UNABLE_TO_CONNECT);
                sendState(h_.provisioned() ? STATE_PROVISIONED : STATE_READY);
            }
        } else {
            sendError(ERROR_UNKNOWN_RPC);
        }
    }

    Hooks& h_;
    uint8_t buf_[9 + 255 + 1];
    size_t n_ = 0;
};

}  // namespace improv
