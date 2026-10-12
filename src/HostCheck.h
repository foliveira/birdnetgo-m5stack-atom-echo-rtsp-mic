#pragma once
// Guard against DNS rebinding for the Web UI. Plain C++, tested on the host by
// test/host/test_hostcheck.cpp.
#include <ctype.h>
#include <string.h>

// Whether `host`, an HTTP Host header, names this device: an IPv4 address, the device name alone,
// or the device name under .local or under a domain that exists only on local networks; with any
// port. A page from another site can point a name of its own at the device's address and then
// talk to the device as a same-origin page, Origin header included, but its requests always carry
// that name as Host.
inline bool isDeviceHost(const char* host, const char* deviceName) {
    char h[96];
    size_t n = 0;
    while (host[n] && host[n] != ':') {
        if (n == sizeof(h) - 1) return false;
        h[n] = (char)tolower((unsigned char)host[n]);
        n++;
    }
    h[n] = '\0';
    if (n && h[n - 1] == '.') h[--n] = '\0';   // fully qualified: "atomecho.local."
    if (n == 0) return true;                    // no Host: HTTP/1.0 tools; browsers always send one

    // Four decimal numbers 0-255 separated by dots.
    int parts = 0, value = -1;
    bool ipv4 = true;
    for (size_t i = 0; i <= n && ipv4; i++) {
        if (i == n || h[i] == '.') {
            ipv4 = value >= 0 && ++parts <= 4;
            value = -1;
        } else if (h[i] >= '0' && h[i] <= '9') {
            value = (value < 0 ? 0 : value * 10) + (h[i] - '0');
            ipv4 = value <= 255;
        } else {
            ipv4 = false;
        }
    }
    if (ipv4 && parts == 4) return true;

    const size_t d = strlen(deviceName);
    if (d == 0 || strncmp(h, deviceName, d) != 0) return false;
    static const char* const domains[] = {"", ".local", ".lan", ".home", ".home.arpa", ".internal",
                                          ".localdomain", ".fritz.box"};
    for (const char* domain : domains) {
        if (strcmp(h + d, domain) == 0) return true;
    }
    return false;
}
