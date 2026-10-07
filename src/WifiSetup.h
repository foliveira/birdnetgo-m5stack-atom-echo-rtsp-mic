#pragma once
#include <Arduino.h>

// Join the network stored in NVS (credentials saved by the setup portal, or earlier by
// WiFiManager, live in the same place). Returns true once connected and holding an IP address.
// Prefers the strongest access point for the SSID, so mesh nodes and repeaters work as intended.
bool wifiJoinStored(const char* hostname, uint32_t timeoutMs);

// Open access point apName with a captive setup page that lists nearby networks and saves the
// chosen credentials. Never returns: the device restarts after a successful join. If credentials
// are already stored (the join failed, e.g. during a router outage) it retries that network every
// 30 s while nobody is connected to the portal, restarts as soon as it is reachable, and restarts
// anyway after three minutes without anyone using the portal.
[[noreturn]] void wifiRunSetupPortal(const char* apName, const char* hostname);

// True if NVS holds an SSID.
bool wifiHasStoredCredentials();

// Forget the stored network (the next boot opens the setup portal).
void wifiForgetCredentials();
