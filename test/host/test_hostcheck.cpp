// Host tests for src/HostCheck.h. Build and run with test/host/run.sh.
#include "HostCheck.h"
#include "check.h"

int main() {
    const char* name = "atomecho";
    // Ways a browser reaches the device.
    CHECK(isDeviceHost("192.168.1.50", name));
    CHECK(isDeviceHost("192.168.1.50:80", name));
    CHECK(isDeviceHost("10.0.0.255", name));
    CHECK(isDeviceHost("atomecho", name));
    CHECK(isDeviceHost("atomecho.local", name));
    CHECK(isDeviceHost("AtomEcho.Local:80", name));
    CHECK(isDeviceHost("atomecho.local.", name));
    CHECK(isDeviceHost("atomecho.lan", name));
    CHECK(isDeviceHost("atomecho.home.arpa", name));
    CHECK(isDeviceHost("atomecho.fritz.box", name));
    CHECK(isDeviceHost("", name));   // HTTP/1.0 without Host

    // Names a rebinding page would use.
    CHECK(!isDeviceHost("attacker.example", name));
    CHECK(!isDeviceHost("attacker.example:80", name));
    CHECK(!isDeviceHost("atomecho.attacker.example", name));
    CHECK(!isDeviceHost("atomecho.local.attacker.example", name));
    CHECK(!isDeviceHost("atomechox.local", name));
    CHECK(!isDeviceHost("192.168.1.50.nip.io", name));
    CHECK(!isDeviceHost("x192.168.1.50", name));

    // Not IPv4 addresses.
    CHECK(!isDeviceHost("192.168.1", name));
    CHECK(!isDeviceHost("192.168.1.50.1", name));
    CHECK(!isDeviceHost("192.168.1.256", name));
    CHECK(!isDeviceHost("192.168..50", name));
    CHECK(!isDeviceHost("[fe80::1]", name));

    // Longer than the buffer.
    CHECK(!isDeviceHost("atomecho.local.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", name));

    // Another device name.
    CHECK(isDeviceHost("garden-mic.local", "garden-mic"));
    CHECK(!isDeviceHost("atomecho.local", "garden-mic"));
    return checkSummary("test_hostcheck");
}
