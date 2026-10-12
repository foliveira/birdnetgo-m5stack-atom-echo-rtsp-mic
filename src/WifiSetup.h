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
// anyway after three minutes without anyone using the portal. Answers Improv like wifiImprovPoll.
[[noreturn]] void wifiRunSetupPortal(const char* apName, const char* hostname, const char* firmwareVersion);

// Improv Wi-Fi over the serial port: an Improv client such as ESP Web Tools asks for the WiFi
// network, sends it over USB, then links to the Web UI. Reads what has arrived on Serial; call it
// from loop(). A network joined this way replaces the stored one and the device carries on with
// it; one that cannot be joined changes nothing.
void wifiImprovPoll(const char* hostname, const char* firmwareVersion);

// True if NVS holds an SSID.
bool wifiHasStoredCredentials();

// Forget the stored network (the next boot opens the setup portal).
void wifiForgetCredentials();
