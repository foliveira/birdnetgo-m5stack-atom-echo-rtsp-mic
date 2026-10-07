#pragma once
// State and functions the main sketch (esp32_rtsp_mic_birdnetgo.ino) shares with WebUI.cpp.
// Both include this header, so a declaration that drifts from its definition fails to compile
// instead of linking against the wrong type.
#include <Arduino.h>

class WiFiClient;

extern const char* FW_VERSION_STR;

// --- Settings (stored in NVS by saveAudioSettings) ---
extern uint32_t currentSampleRate;
extern uint16_t currentBufferSize;
extern float currentGainFactor;
extern bool highpassEnabled;
extern uint16_t highpassCutoffHz;
extern bool agcEnabled;
extern float wifiTxPowerDbm;
extern bool wifiPowerSave;
extern uint8_t cpuFrequencyMhz;
extern uint8_t ledMode;                  // 0 off, 1 status colour, 2 signal level
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool scheduledResetEnabled;
extern uint32_t resetIntervalHours;
extern char deviceName[33];              // DHCP and mDNS name

#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5

// Sample rates the PDM microphone runs at: its clock is 64 x the rate and must stay within
// the SPM1423's 1.0-3.25 MHz.
bool isSupportedSampleRate(uint32_t hz);
bool isValidDeviceName(const char* name);

// --- Live state (read by the Web UI on the loop task) ---
extern uint32_t appliedSampleRate;       // what the I2S driver runs
extern volatile bool isStreaming;
extern bool rtspServerEnabled;
extern unsigned long lastRtspPlayMs;
extern const char* volatile streamEndReason;
extern uint32_t rtspConnectCount;
extern uint32_t rtspPlayCount;
extern volatile uint32_t audioPacketsSent;        // per session
extern volatile uint32_t framesDroppedQueueFull;  // per session
extern volatile uint32_t i2sOverruns;             // per session
extern volatile uint32_t i2sReadErrors;           // per session
extern volatile uint16_t queueHighWater;          // per session
extern volatile uint32_t sendStallMaxMs;          // per session
extern volatile uint32_t sendStallCount;          // per session
extern volatile uint16_t lastPeakAbs16;
extern volatile uint16_t peakHoldAbs16;
extern volatile uint32_t audioClipCount;
extern volatile float agcMultiplier;
extern bool wifiPsSuspended;
extern uint32_t wifiDrops;
extern uint8_t wifiLastDropReason;       // 0 = no loss since boot
extern uint32_t wifiLastDropAt;          // uptime (s) of the latest loss
extern bool micFlat;                     // the microphone delivers constant samples
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern float maxTemperature;
extern bool overheatSensorFault;
extern bool overheatLatched;
extern String overheatLastReason;

// Since-boot totals of the per-session counters above.
struct StreamTotals {
    uint32_t sent, dropped, overruns, readErrors, stalls;
};
StreamTotals streamTotals();

const char* currentRtspClient();
uint16_t streamQueueDepth();
uint16_t streamQueueFrames();
uint32_t uptimeSeconds();

// --- Actions (loop task only) ---
void simplePrintln(String message);
void saveAudioSettings();
void restartI2S();
void updateHighpassCoeffs();
void applyWifiPowerSave(bool log);
void applyWifiTxPower(bool log);
float snapWifiTxDbm(float dbm);
void setDeviceName(const char* name);
bool setRtspServer(bool on);   // false: refused because the thermal latch is set
void clearThermalLatch();
enum class RebootKind { Plain, RestoreDefaults, ForgetWifi };
void scheduleReboot(RebootKind kind, uint32_t delayMs, const char* why);   // why: a string literal
bool requestStreamStop(const char* reason);
void noteFirmwareUpdateInstalled();   // right before restarting into a firmware update
void keepLoopAlive();   // long work inside one loop() pass: the loop watchdog restarts after 60 s
// Live listening (GET /api/listen): 0 once a listener task owns the connection, else an HTTP
// status with the reason in *why.
int startListening(WiFiClient client, const char** why);
const char* currentListener();          // the listener's address, "" when nobody listens
