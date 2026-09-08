#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "driver/i2s.h"
#include <Preferences.h>
#include <math.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include <M5Atom.h>
#include "WebUI.h"

// ================== PIPELINE ARCHITECTURE ==================
// Capture task (core 1, high priority):  I2S DMA -> HPF/gain/AGC -> complete RTP frame -> readyQ
// Sender task  (core 0, one per session): readyQ -> non-blocking send() on the raw socket -> freeQ
// Arduino loop (core 1, priority 1):      RTSP negotiation, Web UI, diagnostics; owns the WiFiClient
//
// Frames are allocated once (TARGET_QUEUE_MS of audio) and travel between cores as pointers in two
// FreeRTOS queues. The sender is the only task that touches the socket during a session, and it only
// ever sends whole frames, so the interleaved RTSP framing cannot desynchronise. The loop closes the
// WiFiClient only after the sender has confirmed exit through senderExitSem.
// PDM microphone outputs 16-bit samples directly (no bit shift).

struct TxFrame {
    uint16_t len;      // valid bytes in data[]
    uint16_t seq;      // RTP sequence (diagnostics)
    uint8_t  data[];   // '$' interleave header (4) + RTP header (12) + big-endian L16 PCM
};
#define FRAME_HDR_BYTES   16
#define I2S_DMA_BUF_COUNT 8
#define I2S_DMA_BUF_LEN   512    // frames per DMA buffer; one buffer is lost per RX_Q_OVF event
#define TARGET_QUEUE_MS   1500   // software buffer in front of the socket
#define MIN_QUEUE_FRAMES  4
#define MAX_QUEUE_FRAMES  64
#define MAX_POOL_BYTES    (64 * 1024)  // cap on the frame pool regardless of rate/buffer choice
#define HEAP_HEADROOM_BYTES (80 * 1024) // leave this much of the largest free block to WiFi/lwIP/HTTP
#define SEND_STALL_LIMIT_MS 10000 // no byte accepted by the socket for this long -> drop the client
#define SENDER_JOIN_LIMIT_MS 30000 // unconfirmed sender exit for this long -> reboot (never close under it)
#define WIFI_PS_DROP_LIMIT 10     // dropped frames in a session that suspend WiFi power save until reboot

static uint8_t*  framePoolMem = NULL;
static uint16_t  framePoolCount = 0;
static uint32_t  frameSlotBytes = 0;
volatile uint16_t pipelineSamples = 0;   // samples per packet the pool is sized for; changes only while capture is parked
static uint32_t appliedSampleRate = 0;   // what the I2S driver actually runs (for reverting a failed reconfiguration)
static uint16_t appliedBufferSize = 0;
static QueueHandle_t freeQ = NULL;
static QueueHandle_t readyQ = NULL;
static QueueHandle_t i2sEventQueue = NULL;

TaskHandle_t captureTaskHandle = NULL;
TaskHandle_t senderTaskHandle = NULL;
SemaphoreHandle_t senderExitSem = NULL;      // given by the sender right before it deletes itself

volatile bool isStreaming = false;            // true from PLAY until the sender has left the socket
volatile bool stopStreamRequested = false;    // loop asks the sender to end the session
unsigned long stopRequestedAt = 0;            // when the pending stop request was first issued (0 = none)
volatile int  streamFd = -1;                  // raw lwIP socket used by the sender
volatile bool capturePauseRequested = false;  // loop asks the capture task to park (I2S reconfig)
volatile bool capturePaused = false;
const char* volatile streamEndReason = "";

// Stream diagnostics (written by the tasks, read by loop / Web UI)
volatile uint32_t audioPacketsSent = 0;
volatile uint32_t framesDroppedQueueFull = 0;
volatile uint32_t i2sOverruns = 0;
volatile uint32_t i2sReadErrors = 0;
volatile uint16_t queueHighWater = 0;
volatile uint32_t sendStallMaxMs = 0;
volatile uint32_t sendStallCount = 0;

portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;  // guards the fixed-size log ring (WebUI.cpp)
portMUX_TYPE hpfMux = portMUX_INITIALIZER_UNLOCKED;  // guards HPF coefficient hand-off

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "2.3.0"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 16000  // M5Stack Atom Echo: 16kHz for PDM microphone
#define DEFAULT_GAIN_FACTOR 3.0f
#define DEFAULT_BUFFER_SIZE 1024   // 64ms @ 16kHz - good balance for BirdNET-Go
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm
#define DEFAULT_CPU_MHZ 80         // Plenty for 16 kHz DSP + ~260 kbit/s TCP; runs noticeably cooler than 160
#define DEFAULT_WIFI_POWER_SAVE false
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 300

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5
#define OVERHEAT_CONSECUTIVE_READINGS 3  // readings (1/min) at/above the limit before tripping

// -- Pins (M5Stack Atom Echo with SPM1423 PDM Microphone)
#define I2S_BCLK_PIN    19  // Bit Clock
#define I2S_LRCLK_PIN   33  // Word Select / Left-Right Clock
#define I2S_DATA_IN_PIN 23  // Microphone Data In
#define I2S_DATA_OUT_PIN 22 // Speaker Data Out (not used for mic-only)

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

// -- RTSP Streaming
String rtspSessionId = "";
char streamSessionId[24] = "";       // C copy for the sender task's in-stream replies
String rtspClientIp = "";            // cached at accept so status handlers never touch the socket
volatile uint8_t rtpChannel = 0;     // interleaved channel requested in SETUP
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
uint8_t rtspParseBuffer[1024];
int rtspParseBufferPos = 0;

// -- Global state
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
// PDM microphones (like SPM1423 on Atom Echo) output decimated 16-bit samples; no shift is applied.
uint8_t i2sShiftBits = 0;  // Always 0 for the PDM microphone on M5Stack Atom Echo (read-only in UI)

// -- Audio metering / clipping diagnostics
volatile uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
volatile uint32_t audioClipCount = 0;      // total blocks where clipping occurred
volatile bool audioClippedLastBlock = false; // clipping occurred in last processed block
volatile uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldUntilMs = 0; // when to clear hold

// -- LED mode: 0=off, 1=static, 2=level
uint8_t ledMode = 1;  // Default: static green during streaming

// -- Automatic Gain Control (AGC)
bool agcEnabled = false;
volatile float agcMultiplier = 1.0f;   // Current AGC multiplier (read by WebUI)
const float AGC_TARGET_RMS = 0.15f;    // Target RMS as fraction of full scale (~-16 dBFS)
const float AGC_MIN_MULT = 0.1f;
const float AGC_MAX_MULT = 10.0f;      // With 3.0x base gain, max effective = 30x
const float AGC_ATTACK_RATE = 0.05f;   // Fast attack (gain reduction) per buffer
const float AGC_RELEASE_RATE = 0.001f; // Slow release (gain increase) per buffer

// -- High-pass filter (biquad) to cut low-frequency rumble
struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    inline void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
Biquad hpf;                              // coefficients published by the loop task (under hpfMux)
volatile uint32_t hpfVersion = 0;        // bumped on every coefficient update
uint32_t hpfConfigSampleRate = 0;
uint16_t hpfConfigCutoff = 0;

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics, auto-recovery and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastPerformanceCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastTempCheck = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = false;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
float overheatTripTemp = 0.0f;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason = "";
String overheatLastTimestamp = "";
bool overheatSensorFault = false;
bool overheatLatched = false;

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum acceptable packet rate (restart below this)
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = DEFAULT_CPU_MHZ;

// -- WiFi TX power / power save (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;
bool wifiPowerSave = DEFAULT_WIFI_POWER_SAVE;  // WIFI_PS_MIN_MODEM when true
bool wifiPsSuspended = false;                  // power save turned off for this boot because the stream dropped frames

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;

// ===============================================

// Helper: convert WiFi power enum to dBm (for logs)
float wifiPowerLevelToDbm(wifi_power_t lvl) {
    switch (lvl) {
        case WIFI_POWER_19_5dBm:    return 19.5f;
        case WIFI_POWER_19dBm:      return 19.0f;
        case WIFI_POWER_18_5dBm:    return 18.5f;
        case WIFI_POWER_17dBm:      return 17.0f;
        case WIFI_POWER_15dBm:      return 15.0f;
        case WIFI_POWER_13dBm:      return 13.0f;
        case WIFI_POWER_11dBm:      return 11.0f;
        case WIFI_POWER_8_5dBm:     return 8.5f;
        case WIFI_POWER_7dBm:       return 7.0f;
        case WIFI_POWER_5dBm:       return 5.0f;
        case WIFI_POWER_2dBm:       return 2.0f;
        case WIFI_POWER_MINUS_1dBm: return -1.0f;
        default:                    return 19.5f;
    }
}

// Helper: pick the highest power level not exceeding requested dBm
static wifi_power_t pickWifiPowerLevel(float dbm) {
    if (dbm <= -1.0f) return WIFI_POWER_MINUS_1dBm;
    if (dbm <= 2.0f)  return WIFI_POWER_2dBm;
    if (dbm <= 5.0f)  return WIFI_POWER_5dBm;
    if (dbm <= 7.0f)  return WIFI_POWER_7dBm;
    if (dbm <= 8.5f)  return WIFI_POWER_8_5dBm;
    if (dbm <= 11.0f) return WIFI_POWER_11dBm;
    if (dbm <= 13.0f) return WIFI_POWER_13dBm;
    if (dbm <= 15.0f) return WIFI_POWER_15dBm;
    if (dbm <= 17.0f) return WIFI_POWER_17dBm;
    if (dbm <= 18.5f) return WIFI_POWER_18_5dBm;
    if (dbm <= 19.0f) return WIFI_POWER_19dBm;
    return WIFI_POWER_19_5dBm;
}

// Apply WiFi TX power
// Logs only when changed; can be muted with log=false
void applyWifiTxPower(bool log = true) {
    wifi_power_t desired = pickWifiPowerLevel(wifiTxPowerDbm);
    if (desired != currentWifiPowerLevel) {
        WiFi.setTxPower(desired);
        currentWifiPowerLevel = desired;
        if (log) {
            simplePrintln("WiFi TX power set to " + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + " dBm");
        }
    }
}

// Apply WiFi modem power save. MIN_MODEM lets the radio sleep between DTIM beacons, which is the
// single largest power (heat) lever on this board, at the cost of delayed ACKs from the AP.
void applyWifiPowerSave(bool log = true) {
    wifiPsSuspended = false;
    WiFi.setSleep(wifiPowerSave);
    if (log) {
        simplePrintln(String("WiFi power save ") + (wifiPowerSave ? "ON (modem sleep)" : "OFF"));
    }
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass) and publish them to the capture task.
// The capture task keeps its own filter state, so changing the cutoff does not reset the filter.
void updateHighpassCoeffs() {
    if (!highpassEnabled) {
        hpfConfigSampleRate = currentSampleRate;
        hpfConfigCutoff = highpassCutoffHz;
        return;
    }
    float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;
    if (fc < 10.0f) fc = 10.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f; // keep reasonable

    const float pi = 3.14159265358979323846f;
    float w0 = 2.0f * pi * (fc / fs);
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q = 0.70710678f; // Butterworth-like
    float alpha = sinw0 / (2.0f * Q);

    float b0 =  (1.0f + cosw0) * 0.5f;
    float b1 = -(1.0f + cosw0);
    float b2 =  (1.0f + cosw0) * 0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    portENTER_CRITICAL(&hpfMux);
    hpf.b0 = b0 / a0;
    hpf.b1 = b1 / a0;
    hpf.b2 = b2 / a0;
    hpf.a1 = a1 / a0;
    hpf.a2 = a2 / a0;
    hpfVersion++;
    portEXIT_CRITICAL(&hpfMux);

    hpfConfigSampleRate = currentSampleRate;
    hpfConfigCutoff = (uint16_t)fc;
}

// Uptime -> "Xd Yh Zm Ts"
String formatUptime(unsigned long seconds) {
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;

    String result = "";
    if (days > 0) result += String(days) + "d ";
    if (hours > 0 || days > 0) result += String(hours) + "h ";
    if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
    result += String(seconds) + "s";
    return result;
}

// Format "X ago" for events based on millis()
String formatSince(unsigned long eventMs) {
    if (eventMs == 0) return String("never");
    unsigned long seconds = (millis() - eventMs) / 1000;
    return formatUptime(seconds) + " ago";
}

static bool isTemperatureValid(float temp) {
    if (isnan(temp) || isinf(temp)) return false;
    if (temp < -20.0f || temp > 130.0f) return false;
    return true;
}

static void persistOverheatNote() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();
}

void recordOverheatTrip(float temp) {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    overheatTripTemp = temp;
    overheatTriggeredAt = millis();
    overheatLastTimestamp = formatUptime(uptimeSeconds);
    overheatLastReason = String("Thermal shutdown: ") + String(temp, 1) + " C reached (limit " +
                         String(overheatShutdownC, 1) + " C). Stream disabled; acknowledge in UI.";
    overheatLatched = true;
    simplePrintln("THERMAL PROTECTION: " + overheatLastReason);
    simplePrintln("TIP: The ESP32 internal sensor reads high. Try WiFi power save, 80 MHz CPU, or a higher limit.");
    persistOverheatNote();
}

// Temperature monitoring + thermal protection.
// The ESP32 (classic) die sensor is uncalibrated and typically reads 10-20 C above the real die
// temperature, so a single reading is not trusted: the limit must be met on
// OVERHEAT_CONSECUTIVE_READINGS consecutive checks (one per minute) before the stream is stopped.
void checkTemperature() {
    static uint8_t overLimitCount = 0;
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    bool tempValid = isTemperatureValid(temp);
    if (!tempValid) {
        lastTemperatureValid = false;
        overLimitCount = 0;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
            overheatLastTimestamp = "";
            overheatTripTemp = 0.0f;
            overheatTriggeredAt = 0;
            persistOverheatNote();
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal protection paused.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;

    if (overheatSensorFault) {
        overheatSensorFault = false;
        overheatLastReason = "Thermal protection restored: temperature sensor reading valid.";
        overheatLastTimestamp = formatUptime((millis() - bootTime) / 1000);
        persistOverheatNote();
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }

    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    bool protectionActive = overheatProtectionEnabled && !overheatSensorFault;
    if (protectionActive) {
        if (!overheatLockoutActive && temp >= overheatShutdownC) {
            overLimitCount++;
            if (overLimitCount >= OVERHEAT_CONSECUTIVE_READINGS) {
                overLimitCount = 0;
                overheatLockoutActive = true;
                recordOverheatTrip(temp);
                requestStreamStop("overheat");
                rtspServerEnabled = false;
                rtspServer.stop();
            } else {
                simplePrintln("Temperature " + String(temp, 1) + " C at/above limit (" +
                              String(overLimitCount) + "/" + String(OVERHEAT_CONSECUTIVE_READINGS) + ")");
            }
        } else {
            overLimitCount = 0;
            if (overheatLockoutActive && temp <= (overheatShutdownC - OVERHEAT_LIMIT_STEP_C)) {
                // Allow re-arming after we cool down by at least one step
                overheatLockoutActive = false;
            }
        }
    } else {
        overLimitCount = 0;
        overheatLockoutActive = false;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    float warnThreshold = max(overheatShutdownC - 5.0f, (float)OVERHEAT_MIN_LIMIT_C);
    if (temp > warnThreshold && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected (" + String(temp, 1) + " C). Approaching shutdown limit.");
        lastTempWarn = millis();
    }
}

// Performance diagnostics
void checkPerformance() {
    uint32_t currentHeap = ESP.getFreeHeap();
    if (currentHeap < minFreeHeap) {
        minFreeHeap = currentHeap;
    }

    if (isStreaming && (millis() - lastStatsReset) > 30000) {
        // Cooldown: skip performance check for 2 minutes after last I2S reset
        if (lastI2SReset > 0 && (millis() - lastI2SReset) < 120000) {
            return;
        }

        uint32_t runtime = millis() - lastStatsReset;
        uint32_t currentRate = (audioPacketsSent * 1000) / runtime;

        if (currentRate > maxPacketRate) maxPacketRate = currentRate;
        if (currentRate < minPacketRate) minPacketRate = currentRate;

        static uint8_t consecutiveLowCount = 0;
        if (currentRate < minAcceptableRate) {
            consecutiveLowCount++;
            simplePrintln("Low packet rate: " + String(currentRate) + " < " + String(minAcceptableRate) + " pkt/s (" + String(consecutiveLowCount) + "/3)");

            if (consecutiveLowCount >= 3 && autoRecoveryEnabled) {
                simplePrintln("AUTO-RECOVERY: 3 consecutive failures, restarting I2S...");
                consecutiveLowCount = 0;
                restartI2S();
                lastStatsReset = millis();
                lastI2SReset = millis();
            }
        } else {
            consecutiveLowCount = 0;
        }
    }
}

// WiFi health check
void checkWiFiHealth() {
    if (WiFi.status() != WL_CONNECTED) {
        // Stop streaming before reconnecting — no point sending RTP into a dead radio
        requestStreamStop("WiFi disconnect");
        simplePrintln("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
    }

    // Re-apply TX power WITHOUT logging (prevent periodic log spam)
    applyWifiTxPower(false);

    int32_t rssi = WiFi.RSSI();
    if (rssi < -85) {
        simplePrintln("WARNING: Weak WiFi signal: " + String(rssi) + " dBm");
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled) return;

    unsigned long uptimeHours = (millis() - bootTime) / 3600000;
    if (uptimeHours >= resetIntervalHours) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        delay(1000);
        ESP.restart();
    }
}

// Load settings from flash
void loadAudioSettings() {
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // i2sShiftBits is ALWAYS 0 for PDM microphones - not configurable
    i2sShiftBits = 0;
    autoRecoveryEnabled = audioPrefs.getBool("autoRecovery", false);
    scheduledResetEnabled = audioPrefs.getBool("schedReset", false);
    resetIntervalHours = audioPrefs.getUInt("resetHours", 24);
    minAcceptableRate = audioPrefs.getUInt("minRate", 50);
    performanceCheckInterval = audioPrefs.getUInt("checkInterval", 15);
    autoThresholdEnabled = audioPrefs.getBool("thrAuto", true);
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", DEFAULT_CPU_MHZ);
    if (cpuFrequencyMhz != 80 && cpuFrequencyMhz != 160 && cpuFrequencyMhz != 240) {
        cpuFrequencyMhz = DEFAULT_CPU_MHZ;   // 2.3.x could store 120 (rejected by the PLL) or 40 (breaks WiFi)
    }
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    wifiPowerSave = audioPrefs.getBool("wifiPs", DEFAULT_WIFI_POWER_SAVE);
    highpassEnabled = audioPrefs.getBool("hpEnable", DEFAULT_HPF_ENABLED);
    highpassCutoffHz = (uint16_t)audioPrefs.getUInt("hpCutoff", DEFAULT_HPF_CUTOFF_HZ);
    agcEnabled = audioPrefs.getBool("agcEnable", false);
    ledMode = audioPrefs.getUChar("ledMode", 1);
    if (ledMode > 2) ledMode = 1;
    overheatProtectionEnabled = audioPrefs.getBool("ohEnable", DEFAULT_OVERHEAT_PROTECTION);
    uint32_t ohLimit = audioPrefs.getUInt("ohThresh", DEFAULT_OVERHEAT_LIMIT_C);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    ohLimit = OVERHEAT_MIN_LIMIT_C + ((ohLimit - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C) * OVERHEAT_LIMIT_STEP_C;
    overheatShutdownC = (float)ohLimit;
    overheatLastReason = audioPrefs.getString("ohReason", "");
    overheatLastTimestamp = audioPrefs.getString("ohStamp", "");
    overheatTripTemp = audioPrefs.getFloat("ohTripC", 0.0f);
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    audioPrefs.end();

    // Sanity clamps (corrupted flash must not produce an unusable pipeline)
    if (currentSampleRate < 8000 || currentSampleRate > 48000) currentSampleRate = DEFAULT_SAMPLE_RATE;
    if (currentBufferSize < 256 || currentBufferSize > 8192) currentBufferSize = DEFAULT_BUFFER_SIZE;
    if (!(currentGainFactor >= 0.1f && currentGainFactor <= 100.0f)) currentGainFactor = DEFAULT_GAIN_FACTOR;

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (overheatLatched) {
        rtspServerEnabled = false;
    }
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
    simplePrintln("Loaded settings: Rate=" + String(currentSampleRate) +
                  ", Gain=" + String(currentGainFactor, 1) +
                  ", Buffer=" + String(currentBufferSize) +
                  ", WiFiTX=" + String(txShown, 1) + "dBm" +
                  ", WiFiPS=" + String(wifiPowerSave ? "on" : "off") +
                  ", CPU=" + String(cpuFrequencyMhz) + "MHz" +
                  ", HPF=" + String(highpassEnabled?"on":"off") +
                  ", HPFcut=" + String(highpassCutoffHz) + "Hz");
}

// Save settings to flash
void saveAudioSettings() {
    audioPrefs.begin("audio", false);
    audioPrefs.putUInt("sampleRate", currentSampleRate);
    audioPrefs.putFloat("gainFactor", currentGainFactor);
    audioPrefs.putUShort("bufferSize", currentBufferSize);
    audioPrefs.putUChar("shiftBits", i2sShiftBits);
    audioPrefs.putBool("autoRecovery", autoRecoveryEnabled);
    audioPrefs.putBool("schedReset", scheduledResetEnabled);
    audioPrefs.putUInt("resetHours", resetIntervalHours);
    audioPrefs.putUInt("minRate", minAcceptableRate);
    audioPrefs.putUInt("checkInterval", performanceCheckInterval);
    audioPrefs.putBool("thrAuto", autoThresholdEnabled);
    audioPrefs.putUChar("cpuFreq", cpuFrequencyMhz);
    audioPrefs.putFloat("wifiTxDbm", wifiTxPowerDbm);
    audioPrefs.putBool("wifiPs", wifiPowerSave);
    audioPrefs.putBool("hpEnable", highpassEnabled);
    audioPrefs.putUInt("hpCutoff", (uint32_t)highpassCutoffHz);
    audioPrefs.putBool("agcEnable", agcEnabled);
    audioPrefs.putUChar("ledMode", ledMode);
    audioPrefs.putBool("ohEnable", overheatProtectionEnabled);
    uint32_t ohLimit = (uint32_t)(overheatShutdownC + 0.5f);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    audioPrefs.putUInt("ohThresh", ohLimit);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Compute recommended minimum packet-rate threshold based on current sample rate and buffer size
uint32_t computeRecommendedMinRate() {
    uint32_t buf = max((uint16_t)1, currentBufferSize);
    float expectedPktPerSec = (float)currentSampleRate / (float)buf;
    uint32_t rec = (uint32_t)(expectedPktPerSec * 0.5f + 0.5f); // 50% safety margin
    if (rec < 5) rec = 5;
    return rec;
}

// Restore application settings to safe defaults and persist
void resetToDefaultSettings() {
    simplePrintln("FACTORY RESET: Restoring default settings...");

    // Clear persisted settings in our namespace
    audioPrefs.begin("audio", false);
    audioPrefs.clear();
    audioPrefs.end();

    // Reset runtime variables to defaults
    currentSampleRate = DEFAULT_SAMPLE_RATE;
    currentGainFactor = DEFAULT_GAIN_FACTOR;
    currentBufferSize = DEFAULT_BUFFER_SIZE;
    i2sShiftBits = 0;  // Default for PDM microphone on M5Stack Atom Echo

    autoRecoveryEnabled = false;
    autoThresholdEnabled = true;
    scheduledResetEnabled = false;
    resetIntervalHours = 24;
    minAcceptableRate = computeRecommendedMinRate();
    performanceCheckInterval = 15;
    cpuFrequencyMhz = DEFAULT_CPU_MHZ;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
    wifiPowerSave = DEFAULT_WIFI_POWER_SAVE;
    highpassEnabled = DEFAULT_HPF_ENABLED;
    highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    agcEnabled = false;
    agcMultiplier = 1.0f;
    ledMode = 1;
    overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLockoutActive = false;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Minimal print helpers: Serial + buffered for Web UI
// Timestamp prefix for log messages (NTP time if available, uptime fallback)
static String logTimestamp() {
    time_t now;
    time(&now);
    if (now > 100000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[24];
        strftime(buf, sizeof(buf), "[%H:%M:%S] ", &ti);
        return String(buf);
    }
    // Fallback: uptime
    unsigned long s = (millis() - bootTime) / 1000;
    unsigned long h = s / 3600; s %= 3600;
    unsigned long m = s / 60; s %= 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "[%02lu:%02lu:%02lu] ", h, m, s);
    return String(buf);
}

void simplePrint(String message) {
    Serial.print(logTimestamp() + message);
}

void simplePrintln(String message) {
    String stamped = logTimestamp() + message;
    Serial.println(stamped);
    webui_pushLog(stamped);
}

// ================== AUDIO PIPELINE: FRAME POOL ==================

static void freeFramePool() {
    if (freeQ)  { vQueueDelete(freeQ);  freeQ = NULL; }
    if (readyQ) { vQueueDelete(readyQ); readyQ = NULL; }
    if (framePoolMem) { free(framePoolMem); framePoolMem = NULL; }
    framePoolCount = 0;
    frameSlotBytes = 0;
}

// Allocate frames for TARGET_QUEUE_MS of audio at the current rate/buffer size.
// Only call while the capture task is parked (or not yet started) and no sender is running.
static bool allocFramePool() {
    freeFramePool();
    pipelineSamples = currentBufferSize;   // the capture task reads this, never currentBufferSize directly
                                           // (set even if the pool fails so metering keeps running)
    uint32_t frameMs = (uint32_t)currentBufferSize * 1000UL / currentSampleRate;
    if (frameMs == 0) frameMs = 1;
    uint32_t want = (TARGET_QUEUE_MS + frameMs - 1) / frameMs;
    if (want < MIN_QUEUE_FRAMES) want = MIN_QUEUE_FRAMES;
    if (want > MAX_QUEUE_FRAMES) want = MAX_QUEUE_FRAMES;
    frameSlotBytes = (sizeof(TxFrame) + FRAME_HDR_BYTES + (uint32_t)currentBufferSize * 2 + 3) & ~3UL;

    // Byte budget: never more than MAX_POOL_BYTES, and always leave HEAP_HEADROOM_BYTES of the
    // largest free block for WiFi/lwIP/HTTP. (At 48 kHz the time-based count alone would ask
    // for ~130 KB.)
    uint32_t byTarget = want;
    if (want > MAX_POOL_BYTES / frameSlotBytes) want = MAX_POOL_BYTES / frameSlotBytes;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    uint32_t byHeap = (largest > HEAP_HEADROOM_BYTES) ? (uint32_t)((largest - HEAP_HEADROOM_BYTES) / frameSlotBytes) : 0;
    if (want > byHeap) want = byHeap;
    if (want < MIN_QUEUE_FRAMES) want = MIN_QUEUE_FRAMES;

    while (want >= MIN_QUEUE_FRAMES) {
        framePoolMem = (uint8_t*)malloc(frameSlotBytes * want);
        if (framePoolMem) break;
        want /= 2;
    }
    if (!framePoolMem) {
        simplePrintln("FATAL: cannot allocate audio frame pool");
        return false;
    }
    freeQ = xQueueCreate(want, sizeof(TxFrame*));
    readyQ = xQueueCreate(want, sizeof(TxFrame*));
    if (!freeQ || !readyQ) {
        freeFramePool();
        simplePrintln("FATAL: cannot allocate frame queues");
        return false;
    }
    for (uint32_t i = 0; i < want; i++) {
        TxFrame* f = (TxFrame*)(framePoolMem + i * frameSlotBytes);
        f->len = 0;
        f->seq = 0;
        xQueueSend(freeQ, &f, 0);
    }
    framePoolCount = (uint16_t)want;
    appliedBufferSize = currentBufferSize;
    simplePrintln("Frame pool: " + String(want) + " x " + String(frameSlotBytes) + " B (" +
                  String(want * frameMs) + " ms of audio), largest free block was " + String(largest / 1024) + " KB");
    if (want < byTarget) {
        simplePrintln("NOTE: pool shorter than " + String(TARGET_QUEUE_MS) +
                      " ms target (memory); shorter WiFi stalls will drop frames. Lower the sample rate or buffer size.");
    }
    return true;
}

// Move every ready frame back to the free queue. Only when no producer/consumer is active.
static void flushReadyQueue() {
    TxFrame* f;
    while (readyQ && xQueueReceive(readyQ, &f, 0) == pdTRUE) {
        xQueueSend(freeQ, &f, 0);
    }
}

uint16_t streamQueueDepth() {
    return readyQ ? (uint16_t)uxQueueMessagesWaiting(readyQ) : 0;
}

uint16_t streamQueueFrames() {
    return framePoolCount;
}

// ================== I2S (PDM microphone) ==================
void setup_i2s_driver() {
    static bool installed = false;
    if (installed) i2s_driver_uninstall(I2S_NUM_0);
    i2sEventQueue = NULL;

    // Hardware ring: 8 buffers x 512 frames. The driver keeps one buffer in flight, so 7 x 512 =
    // 3584 samples (224 ms at 16 kHz) can wait for the capture task before samples are lost; the
    // old 6 x 60 ring gave 22.5 ms. (Buffer bytes stay well under the driver's 4092-byte limit.)
    i2s_config_t i2s_config = {
        // PDM mode required for SPM1423 microphone on Atom Echo
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 1, 0)
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
#else
        .communication_format = I2S_COMM_FORMAT_I2S,
#endif
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DMA_BUF_COUNT,
        .dma_buf_len = I2S_DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
#if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 3, 0))
        .mck_io_num = I2S_PIN_NO_CHANGE,
#endif
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCLK_PIN,
        .data_out_num = I2S_DATA_OUT_PIN,
        .data_in_num = I2S_DATA_IN_PIN
    };

    // Event queue lets the capture task count DMA overruns (I2S_EVENT_RX_Q_OVF). The driver also
    // posts one RX_DONE per finished DMA buffer with no fullness check, so the queue must hold far
    // more than dma_buf_count events or the OVF event itself gets lost: 40 slots covers a 1.2 s
    // stall at 16 kHz and the queue is drained before and after every read.
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 40, &i2sEventQueue);
    installed = (err == ESP_OK);
    if (installed) appliedSampleRate = currentSampleRate;
    if (err != ESP_OK) {
        simplePrintln("ERROR: i2s_driver_install failed: " + String((int)err));
        i2sEventQueue = NULL;
    }
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, currentSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    simplePrintln("I2S ready (PDM mode): " + String(currentSampleRate) + "Hz, buffer " +
                  String(currentBufferSize) + " samples, DMA slack 3584 samples");
}

// ================== CAPTURE TASK (core 1) ==================
// Runs from boot for the life of the device. Always meters the signal (so the Web UI level works
// without a client) and, while a session is active, produces complete RTP frames into readyQ.
// It is also the only LED writer after setup(), which removes the LED ownership race.

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

// Returns the number of DMA buffers the driver discarded since the last call.
static inline uint32_t drainI2sEvents() {
    if (!i2sEventQueue) return 0;
    i2s_event_t evt;
    uint32_t lost = 0;
    while (xQueueReceive(i2sEventQueue, &evt, 0) == pdTRUE) {
        if (evt.type == I2S_EVENT_RX_Q_OVF) { i2sOverruns++; lost++; }
    }
    return lost;
}

static CRGB ledColorFor(bool streaming, bool clipped, float pct) {
    if (!streaming) {
        if (overheatLatched) return CRGB(128, 0, 0);                 // red: thermal latch
        return (ledMode > 0) ? CRGB(0, 0, 128) : CRGB(0, 0, 0);      // blue: ready
    }
    if (ledMode == 0) return CRGB(0, 0, 0);
    if (ledMode == 1) return CRGB(0, 128, 0);                         // static green: streaming
    if (clipped)      return CRGB(255, 0, 0);                         // red: clipping
    if (pct > 0.7f)   return CRGB(255, 165, 0);                       // orange: hot signal
    if (pct > 0.3f)   return CRGB(0, 255, 0);                         // bright green: good level
    if (pct > 0.05f)  return CRGB(0, 64, 0);                          // dim green: low signal
    return CRGB(32, 0, 32);                                           // dim purple: very quiet
}

static void captureTask(void* arg) {
    Serial.printf("[Capture] task started on core %d\n", xPortGetCoreID());

    int16_t* captureBuf = NULL;
    uint16_t capSamples = 0;

    Biquad localHpf;                       // coefficients + filter state live here
    uint32_t localHpfVersion = 0xFFFFFFFF;
    bool hpfWasEnabled = false;
    float localAgcMult = 1.0f;
    float appliedGain = -1.0f;             // gain at the end of the previous block (ramp start)

    CRGB lastLed = CRGB(1, 2, 3);          // impossible value forces the first draw
    unsigned long lastLedUpdate = 0;
    unsigned long lastStatsLog = millis();

    for (;;) {
        // --- pause protocol: the loop task reinstalls I2S / reallocates the pool while we are parked ---
        if (capturePauseRequested) {
            if (captureBuf) { free(captureBuf); captureBuf = NULL; capSamples = 0; }
            capturePaused = true;
            __asm__ __volatile__("memw" ::: "memory");
            while (capturePauseRequested) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            capturePaused = false;
            localHpfVersion = 0xFFFFFFFF;  // sample rate may have changed: refetch coefficients
            localHpf.reset();
            continue;
        }

        if (pipelineSamples == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (!captureBuf || capSamples != pipelineSamples) {
            if (captureBuf) free(captureBuf);
            capSamples = pipelineSamples;
            captureBuf = (int16_t*)malloc((size_t)capSamples * sizeof(int16_t));
            if (!captureBuf) {
                Serial.println("[Capture] FATAL: no memory for capture buffer");
                capSamples = 0;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        // --- one packet worth of samples (blocks in the driver until the DMA has them) ---
        uint32_t lostBufs = drainI2sEvents();
        size_t bytesRead = 0;
        const size_t want = (size_t)capSamples * sizeof(int16_t);
        esp_err_t r = i2s_read(I2S_NUM_0, captureBuf, want, &bytesRead, pdMS_TO_TICKS(1000));
        lostBufs += drainI2sEvents();   // RX_DONE events from this read must not sit in the queue during a stall
        if (isStreaming && lostBufs) {
            // Audio the DMA discarded still happened: keep the RTP clock honest.
            rtpTimestamp += lostBufs * I2S_DMA_BUF_LEN;
        }
        if (r != ESP_OK || bytesRead != want) {
            i2sReadErrors++;
            if (isStreaming) {                  // account for the packet we did not get
                rtpTimestamp += (uint32_t)((want - bytesRead) / sizeof(int16_t));
                rtpSequence++;
            }
            if (r != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // --- HPF coefficient hand-off (published by the loop under hpfMux) ---
        const bool hpfOn = highpassEnabled;
        if (hpfOn) {
            if (localHpfVersion != hpfVersion) {
                portENTER_CRITICAL(&hpfMux);
                localHpf.b0 = hpf.b0; localHpf.b1 = hpf.b1; localHpf.b2 = hpf.b2;
                localHpf.a1 = hpf.a1; localHpf.a2 = hpf.a2;
                localHpfVersion = hpfVersion;
                portEXIT_CRITICAL(&hpfMux);
            }
            if (!hpfWasEnabled) localHpf.reset();
        }
        hpfWasEnabled = hpfOn;

        const bool agcOn = agcEnabled;
        float effectiveGain = currentGainFactor;
        if (agcOn) effectiveGain *= localAgcMult;
        if (appliedGain < 0.0f) appliedGain = effectiveGain;
        // Ramp linearly from last block's gain to this block's target: AGC steps and manual gain
        // changes glide over 64 ms instead of producing a click at the block boundary.
        float g = appliedGain;
        const float gStep = (effectiveGain - appliedGain) / (float)capSamples;

        // --- output frame: only while streaming; when the pool is exhausted, recycle the OLDEST
        //     queued frame so the receiver gets the freshest audio and latency stays bounded ---
        TxFrame* frame = NULL;
        const bool streaming = isStreaming;
        if (streaming && freeQ) {
            if (xQueueReceive(freeQ, &frame, 0) != pdTRUE) {
                framesDroppedQueueFull++;   // either the recycled oldest frame or (never, with >= 2 frames) nothing
                xQueueReceive(readyQ, &frame, 0);
            }
        }
        int16_t* out = frame ? (int16_t*)(frame->data + FRAME_HDR_BYTES) : NULL;

        // --- DSP: HPF -> gain -> clip -> metering, written big-endian straight into the frame ---
        float peakAbs = 0.0f, sumSquares = 0.0f;
        bool clipped = false;
        for (uint16_t i = 0; i < capSamples; i++) {
            float s = (float)captureBuf[i];
            if (hpfOn) s = localHpf.process(s);
            g += gStep;
            s *= g;
            float a = fabsf(s);
            if (a > peakAbs) peakAbs = a;
            sumSquares += s * s;
            if (s > 32767.0f)       { s = 32767.0f;  clipped = true; }
            else if (s < -32768.0f) { s = -32768.0f; clipped = true; }
            if (out) out[i] = (int16_t)bswap16((uint16_t)(int16_t)s);
        }
        appliedGain = effectiveGain;

        // --- AGC ---
        if (agcOn) {
            float rms = sqrtf(sumSquares / (float)capSamples) / 32767.0f;
            if (rms > 0.001f) {  // Don't adjust on silence
                float ratio = AGC_TARGET_RMS / rms;
                if (ratio < 1.0f) {
                    localAgcMult += (ratio - 1.0f) * AGC_ATTACK_RATE * localAgcMult;   // fast attack
                } else {
                    localAgcMult += (ratio - 1.0f) * AGC_RELEASE_RATE * localAgcMult;  // slow release
                }
                if (localAgcMult < AGC_MIN_MULT) localAgcMult = AGC_MIN_MULT;
                if (localAgcMult > AGC_MAX_MULT) localAgcMult = AGC_MAX_MULT;
            }
            agcMultiplier = localAgcMult;
        } else {
            localAgcMult = 1.0f;
        }

        // --- RTP framing + enqueue ---
        if (streaming) {
            if (frame) {
                uint8_t* d = frame->data;
                const uint16_t payload = (uint16_t)(capSamples * 2);
                const uint16_t pkt = (uint16_t)(12 + payload);
                d[0] = 0x24; d[1] = rtpChannel;                             // '$', interleaved channel
                d[2] = (uint8_t)(pkt >> 8); d[3] = (uint8_t)(pkt & 0xFF);
                d[4] = 0x80; d[5] = 96;                                     // V=2, PT=96 (L16)
                d[6] = (uint8_t)(rtpSequence >> 8); d[7] = (uint8_t)(rtpSequence & 0xFF);
                d[8]  = (uint8_t)(rtpTimestamp >> 24); d[9]  = (uint8_t)(rtpTimestamp >> 16);
                d[10] = (uint8_t)(rtpTimestamp >> 8);  d[11] = (uint8_t)(rtpTimestamp);
                d[12] = (uint8_t)(rtpSSRC >> 24); d[13] = (uint8_t)(rtpSSRC >> 16);
                d[14] = (uint8_t)(rtpSSRC >> 8);  d[15] = (uint8_t)(rtpSSRC);
                frame->len = FRAME_HDR_BYTES + payload;
                frame->seq = rtpSequence;
                if (xQueueSend(readyQ, &frame, 0) != pdTRUE) {
                    xQueueSend(freeQ, &frame, 0);
                    framesDroppedQueueFull++;
                }
                UBaseType_t depth = uxQueueMessagesWaiting(readyQ);
                if (depth > queueHighWater) queueHighWater = (uint16_t)depth;
            }
            // Sequence and timestamp advance for every captured packet, sent or not, so a dropped
            // frame reaches the receiver as an honest gap instead of a time-compressed splice.
            rtpSequence++;
            rtpTimestamp += capSamples;
        }

        // --- metering for the Web UI ---
        if (peakAbs > 32767.0f) peakAbs = 32767.0f;
        lastPeakAbs16 = (uint16_t)peakAbs;
        audioClippedLastBlock = clipped;
        if (clipped) audioClipCount++;
        const unsigned long now = millis();
        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = now + 3000UL;
        } else if (peakHoldAbs16 > 0 && now > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        // --- LED (~10 Hz, only redrawn when the colour changes) ---
        if (now - lastLedUpdate > 100) {
            lastLedUpdate = now;
            CRGB c = ledColorFor(streaming, clipped, peakAbs / 32767.0f);
            if (c != lastLed) {
                M5.dis.drawpix(0, c);
                lastLed = c;
            }
        }

        // --- periodic serial stats (no heap use) ---
        if (streaming && now - lastStatsLog > 30000) {
            lastStatsLog = now;
            Serial.printf("[Capture] sent=%u dropped=%u i2s_ovf=%u q=%u/%u hw=%u stall_max=%ums clip=%u agc=%.2f\n",
                          audioPacketsSent, framesDroppedQueueFull, i2sOverruns,
                          (unsigned)uxQueueMessagesWaiting(readyQ), framePoolCount, queueHighWater,
                          sendStallMaxMs, audioClipCount, localAgcMult);
        }
    }
}

// Park the capture task (blocks up to ~2.5 s). Returns false if it did not park.
static bool pauseCapture() {
    if (!captureTaskHandle) return true;
    capturePauseRequested = true;
    __asm__ __volatile__("memw" ::: "memory");
    unsigned long deadline = millis() + 2500;
    while (!capturePaused && millis() < deadline) vTaskDelay(pdMS_TO_TICKS(5));
    if (!capturePaused) {
        Serial.println("[Loop] WARNING: capture task did not pause");
        capturePauseRequested = false;
        __asm__ __volatile__("memw" ::: "memory");
        xTaskNotifyGive(captureTaskHandle);  // in case it parks right after the deadline
        return false;
    }
    return true;
}

static void resumeCapture() {
    capturePauseRequested = false;
    __asm__ __volatile__("memw" ::: "memory");
    if (captureTaskHandle) xTaskNotifyGive(captureTaskHandle);
}

static void startCaptureTask() {
    if (captureTaskHandle) return;
    BaseType_t ok = xTaskCreatePinnedToCore(captureTask, "AudioCapture", 8192, NULL, 20,
                                            &captureTaskHandle, 1);
    if (ok != pdPASS) {
        captureTaskHandle = NULL;
        simplePrintln("FATAL: failed to create capture task");
    }
}

// ================== SENDER TASK (core 0, one per session) ==================
// Owns the socket from PLAY until exit. It only ever sends whole frames: once the first byte of a
// frame is out, the rest follows (or the connection is dropped), so the '$'-interleaved framing
// stays intact under any WiFi stall. RTSP requests from the client are serviced at frame boundaries.

// Write a short RTSP response at a frame boundary. Same no-progress budget as RTP frames
// (SEND_STALL_LIMIT_MS) so a congested link does not end the session sooner than a frame would;
// gives up early if the loop asks the sender to stop.
static bool senderRespond(int fd, const char* resp, int len) {
    int off = 0;
    unsigned long lastProgress = millis();
    while (off < len) {
        if (stopStreamRequested) return false;
        int w = send(fd, resp + off, len - off, MSG_DONTWAIT);
        if (w > 0) { off += w; lastProgress = millis(); continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) return false;
        if (millis() - lastProgress > SEND_STALL_LIMIT_MS) return false;
        fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
        struct timeval tv = { 0, 50000 };
        select(fd + 1, NULL, &ws, NULL, &tv);
    }
    return true;
}

// Drain and answer whatever the client sent. Returns false when the session must end.
static bool senderServiceRtsp(int fd, char* rx, int& rxlen, const int rxcap, int& skipBytes, const char** reason) {
    for (;;) {
        int room = rxcap - 1 - rxlen;
        if (room <= 0) { rxlen = 0; room = rxcap - 1; }   // unparseable junk: reset
        int n = recv(fd, rx + rxlen, room, MSG_DONTWAIT);
        if (n == 0) { *reason = "client closed connection"; return false; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            *reason = "socket read error";
            return false;
        }
        rxlen += n;

        while (rxlen > 0) {
            if (skipBytes > 0) {                       // tail of an oversized interleaved packet
                int k = (skipBytes < rxlen) ? skipBytes : rxlen;
                memmove(rx, rx + k, rxlen - k); rxlen -= k; skipBytes -= k;
                continue;
            }
            if (rx[0] == '$') {                        // interleaved binary from the client (RTCP)
                if (rxlen < 4) break;
                int total = 4 + (((uint8_t)rx[2] << 8) | (uint8_t)rx[3]);
                if (total > rxlen) {
                    if (total > rxcap - 1) { skipBytes = total - rxlen; rxlen = 0; }
                    break;
                }
                memmove(rx, rx + total, rxlen - total); rxlen -= total;
                continue;
            }
            rx[rxlen] = '\0';
            char* end = strstr(rx, "\r\n\r\n");
            if (!end) break;                           // wait for the rest of the header
            int consumed = (int)(end - rx) + 4;
            *end = '\0';
            int bodyLen = 0;
            const char* cl = strstr(rx, "Content-Length:");
            if (cl) bodyLen = atoi(cl + 15);
            if (bodyLen < 0 || bodyLen > 4096) { *reason = "malformed RTSP request"; return false; }
            if (rxlen < consumed + bodyLen) {          // body not complete yet
                if (consumed + bodyLen > rxcap - 1) { skipBytes = consumed + bodyLen - rxlen; rxlen = 0; break; }
                *end = '\r';
                break;
            }
            consumed += bodyLen;

            int cseq = 1;
            const char* cs = strstr(rx, "CSeq:");
            if (cs) cseq = atoi(cs + 5);
            char resp[224];
            int rl;
            bool endSession = false;
            if (strncmp(rx, "TEARDOWN", 8) == 0) {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", cseq, streamSessionId);
                endSession = true;
            } else if (strncmp(rx, "OPTIONS", 7) == 0) {
                rl = snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\nCSeq: %d\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n", cseq);
            } else if (strncmp(rx, "GET_PARAMETER", 13) == 0 || strncmp(rx, "SET_PARAMETER", 13) == 0) {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", cseq, streamSessionId);  // keepalive
            } else {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %d\r\n\r\n", cseq);
            }
            if (!senderRespond(fd, resp, rl)) { *reason = stopStreamRequested ? "stop requested" : "client stalled"; return false; }
            lastRTSPActivity = millis();
            if (endSession) { *reason = "TEARDOWN"; return false; }

            memmove(rx, rx + consumed, rxlen - consumed);
            rxlen -= consumed;
        }
    }
}

static void senderTask(void* arg) {
    const int fd = streamFd;
    TxFrame* cur = NULL;
    size_t off = 0;
    char rx[512];
    int rxlen = 0, skipBytes = 0;
    unsigned long lastProgress = millis();
    unsigned long lastRtspPoll = 0;
    unsigned long stallStart = 0;
    const char* reason = "stop requested";
    uint32_t sent = 0;

    Serial.printf("[Sender] session started on core %d, fd %d\n", xPortGetCoreID(), fd);

    while (!stopStreamRequested) {
        const unsigned long now = millis();

        if (!cur) {
            // Frame boundary: the only safe point to interleave RTSP responses.
            if (now - lastRtspPoll >= 200) {
                lastRtspPoll = now;
                if (!senderServiceRtsp(fd, rx, rxlen, sizeof(rx), skipBytes, &reason)) break;
            }
            if (xQueueReceive(readyQ, &cur, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            off = 0;
        }

        int w = send(fd, cur->data + off, cur->len - off, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            lastProgress = now;
            if (stallStart) {
                uint32_t d = (uint32_t)(now - stallStart);
                if (d > sendStallMaxMs) sendStallMaxMs = d;
                stallStart = 0;
            }
            if (off >= cur->len) {
                xQueueSend(freeQ, &cur, 0);
                cur = NULL;
                sent++;
                audioPacketsSent = sent;
            }
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
            reason = "socket write error";
            break;
        }

        // Send buffer full (WiFi retransmits, slow client, sleeping AP...). Wait, bounded.
        if (!stallStart) { stallStart = now; sendStallCount++; }
        if (now - lastProgress > SEND_STALL_LIMIT_MS) { reason = "client stalled"; break; }
        fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
        struct timeval tv = { 0, 50000 };
        select(fd + 1, NULL, &ws, NULL, &tv);
    }

    // --- cleanup: leave the socket alone (the loop closes it), recycle frames, confirm exit ---
    streamEndReason = reason;
    isStreaming = false;
    __asm__ __volatile__("memw" ::: "memory");
    if (cur) xQueueSend(freeQ, &cur, 0);
    flushReadyQueue();
    Serial.printf("[Sender] session ended: %s (sent %u)\n", reason, sent);
    xSemaphoreGive(senderExitSem);
    vTaskDelete(NULL);
}

// Loop task: tear down bookkeeping after the sender has confirmed exit, close the client, log.
static void finishStreamSession(const char* reason) {
    senderTaskHandle = NULL;
    isStreaming = false;
    stopStreamRequested = false;
    stopRequestedAt = 0;
    streamFd = -1;
    rtspClient.stop();               // the loop task is the only WiFiClient user
    rtspParseBufferPos = 0;
    unsigned long sessionSec = (millis() - lastRtspPlayMs) / 1000;
    simplePrintln("STREAMING STOPPED (" + String(reason) + "): " + String(sessionSec) + "s, sent " +
                  String(audioPacketsSent) + ", dropped " + String(framesDroppedQueueFull) +
                  ", I2S overruns " + String(i2sOverruns) + ", queue peak " + String(queueHighWater) +
                  "/" + String(framePoolCount) + ", longest stall " + String(sendStallMaxMs) +
                  " ms, RSSI " + String(WiFi.RSSI()) + " dBm");
}

// Loop task: hand the negotiated client to a new sender task. Called right after the PLAY reply.
static bool startStreamSession(WiFiClient& client) {
    if (senderTaskHandle || !readyQ) return false;
    flushReadyQueue();
    strlcpy(streamSessionId, rtspSessionId.c_str(), sizeof(streamSessionId));
    audioPacketsSent = 0;
    framesDroppedQueueFull = 0;
    i2sOverruns = 0;
    i2sReadErrors = 0;
    queueHighWater = 0;
    sendStallMaxMs = 0;
    sendStallCount = 0;
    lastStatsReset = millis();
    lastRtspPlayMs = millis();
    rtspPlayCount++;
    streamEndReason = "";
    stopStreamRequested = false;
    stopRequestedAt = 0;

    // Streaming socket: let Nagle coalesce the 2064-byte frames into full-MSS segments
    // (fewer radio frames and ACKs). Latency is irrelevant for BirdNET-Go.
    client.setNoDelay(false);
    streamFd = client.fd();
    if (streamFd < 0) { streamFd = -1; return false; }   // negotiation write failed and closed the socket
    xSemaphoreTake(senderExitSem, 0);   // clear any stale signal
    __asm__ __volatile__("memw" ::: "memory");
    isStreaming = true;

    BaseType_t ok = xTaskCreatePinnedToCore(senderTask, "RtpSender", 6144, NULL, 8, &senderTaskHandle, 0);
    if (ok != pdPASS) {
        senderTaskHandle = NULL;
        isStreaming = false;
        streamFd = -1;
        simplePrintln("ERROR: failed to create sender task");
        return false;
    }
    return true;
}

// Loop task: end the current session (if any) and wait for the sender to confirm.
// Returns true if the sender exited cleanly (or nothing was running).
// On timeout NOTHING is torn down: the socket stays open and the task handle stays set, so no
// second sender can be created and no fd or queue is closed under a live task. The stop request
// stays pending; the loop keeps trying to join and reboots after SENDER_JOIN_LIMIT_MS.
bool requestStreamStop(const char* reason) {
    if (!senderTaskHandle) return true;
    Serial.printf("[Loop] requestStreamStop: %s\n", reason);
    stopStreamRequested = true;
    if (stopRequestedAt == 0) stopRequestedAt = millis();
    __asm__ __volatile__("memw" ::: "memory");
    bool clean = (xSemaphoreTake(senderExitSem, pdMS_TO_TICKS(3000)) == pdTRUE);
    if (!clean) {
        Serial.printf("[Loop] WARNING: sender did not exit within 3 s (%s); leaving session in place\n", reason);
        return false;
    }
    finishStreamSession(reason);
    return true;
}

// Reconfigure the audio front end (sample rate / buffer size). Ends any active session; the
// client reconnects and gets the new parameters through DESCRIBE.
// Put the last working rate/buffer back into the settings (and flash) after a failed change.
static void revertAudioConfig(uint32_t rate, uint16_t buf, const char* why) {
    if (rate != 0 && buf != 0 && (currentSampleRate != rate || currentBufferSize != buf)) {
        currentSampleRate = rate;
        currentBufferSize = buf;
        if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();
        saveAudioSettings();
    }
    simplePrintln(String("Audio reconfiguration aborted (") + why + "); keeping " +
                  String(currentSampleRate) + " Hz / " + String(currentBufferSize) + " samples");
}

void restartI2S() {
    const uint32_t prevRate = appliedSampleRate;   // last configuration that actually ran
    const uint16_t prevBuf = appliedBufferSize;
    simplePrintln("Reconfiguring audio: " + String(currentSampleRate) + " Hz, " +
                  String(currentBufferSize) + " samples/packet");
    if (!requestStreamStop("audio reconfiguration")) {
        revertAudioConfig(prevRate, prevBuf, "sender task busy");   // pool/queues must not change under a live sender
        return;
    }
    if (!pauseCapture()) {
        revertAudioConfig(prevRate, prevBuf, "capture task busy");
        return;
    }
    setup_i2s_driver();
    if (!allocFramePool()) {
        // New parameters do not fit in memory: go back to the ones that worked so streaming stays possible.
        revertAudioConfig(prevRate, prevBuf, "frame pool allocation failed");
        setup_i2s_driver();
        if (!allocFramePool()) simplePrintln("Frame pool still unavailable: PLAY will be refused until reboot");
    }
    updateHighpassCoeffs();
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    resumeCapture();
    simplePrintln("Audio pipeline restarted");
}

// ================== RTSP NEGOTIATION (loop task, only while not streaming) ==================
void handleRTSPCommand(WiFiClient &client, String request) {
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }

    lastRTSPActivity = millis();

    if (request.startsWith("OPTIONS")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n");

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Content-Type: application/sdp\r\n");
        client.print("Content-Base: rtsp://" + ip + ":8554/audio/\r\n");
        client.print("Content-Length: " + String(sdp.length()) + "\r\n\r\n");
        client.print(sdp);

    } else if (request.startsWith("SETUP")) {
        // Only TCP interleaved is offered. Answering 461 to a UDP request makes clients (VLC, some
        // ffmpeg configs) fall back to TCP immediately instead of failing their first attempt.
        int tPos = request.indexOf("Transport:");
        String transport = (tPos >= 0) ? request.substring(tPos, request.indexOf("\r", tPos) < 0 ? request.length() : request.indexOf("\r", tPos)) : String("");
        if (tPos >= 0 && transport.indexOf("RTP/AVP/TCP") < 0) {
            client.print("RTSP/1.0 461 Unsupported Transport\r\n");
            client.print("CSeq: " + cseq + "\r\n\r\n");
            return;
        }
        uint8_t ch = 0;
        int iPos = transport.indexOf("interleaved=");
        if (iPos >= 0) {
            long v = transport.substring(iPos + 12).toInt();
            if (v >= 0 && v <= 254) ch = (uint8_t)v;
        }
        rtpChannel = ch;
        rtspSessionId = String(random(100000000, 999999999));
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + ";timeout=60\r\n");
        client.print("Transport: RTP/AVP/TCP;unicast;interleaved=" + String(ch) + "-" + String(ch + 1) + "\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        if (senderTaskHandle != NULL || readyQ == NULL) {
            // Previous sender not joined yet, or no frame pool (allocation failed): say so instead of
            // answering 200 OK and then sending nothing until the idle timeout.
            client.print("RTSP/1.0 503 Service Unavailable\r\nCSeq: " + cseq + "\r\nSession: " + rtspSessionId + "\r\n\r\n");
            simplePrintln(String("PLAY refused: ") + (readyQ == NULL ? "no frame pool (memory)" : "previous session still closing"));
            return;
        }
        String ip = WiFi.localIP().toString();
        // Random non-zero start values (RFC 3550); ffmpeg treats an RTP timestamp of 0 as "unset".
        rtpSequence = (uint16_t)random(1, 65535);
        rtpTimestamp = (uint32_t)random(1, 0x7FFFFFFF);
        // Reply first (the sender is not running yet, so the loop still owns the socket).
        // One print: the socket still has TCP_NODELAY set during negotiation.
        String resp = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\nSession: " + rtspSessionId +
                      "\r\nRange: npt=0.000-\r\nRTP-Info: url=rtsp://" + ip + ":8554/audio/track1;seq=" +
                      String(rtpSequence) + ";rtptime=" + String(rtpTimestamp) + "\r\n\r\n";
        client.print(resp);
        // (no client.flush(): on arduino-esp32 that discards the receive buffer)

        if (startStreamSession(client)) {
            simplePrintln("STREAMING STARTED for " + rtspClientIp);
        } else {
            // 200 OK is already on the wire; do not leave the client waiting for RTP that never comes.
            simplePrintln("Session start failed after PLAY; closing client");
            client.stop();
            rtspParseBufferPos = 0;
        }

    } else if (request.startsWith("TEARDOWN")) {
        // Only reached before PLAY (during a session the sender answers TEARDOWN itself)
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n\r\n");
        simplePrintln("RTSP TEARDOWN before PLAY");
    } else if (request.startsWith("GET_PARAMETER")) {
        // Many RTSP clients send GET_PARAMETER as keep-alive.
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n\r\n");
    }
}

// RTSP request assembly (loop task, only called when !isStreaming)
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    if (client.available()) {
        int available = client.available();
        int spaceLeft = sizeof(rtspParseBuffer) - rtspParseBufferPos - 1;

        if (available > spaceLeft) {
            available = spaceLeft;
        }

        if (available <= 0) {
            static unsigned long lastOverflowWarning = 0;
            if (millis() - lastOverflowWarning > 5000) {
                simplePrintln("RTSP buffer full - resetting");
                lastOverflowWarning = millis();
            }
            rtspParseBufferPos = 0;
            return;
        }

        client.read(rtspParseBuffer + rtspParseBufferPos, available);
        rtspParseBufferPos += available;
        rtspParseBuffer[rtspParseBufferPos] = '\0';

        char* endOfHeader = strstr((char*)rtspParseBuffer, "\r\n\r\n");
        if (endOfHeader != nullptr) {
            *endOfHeader = '\0';
            String request = String((char*)rtspParseBuffer);

            handleRTSPCommand(client, request);

            int headerLen = (endOfHeader - (char*)rtspParseBuffer) + 4;
            int remaining = rtspParseBufferPos - headerLen;
            if (remaining > 0) {
                memmove(rtspParseBuffer, rtspParseBuffer + headerLen, remaining);
            }
            rtspParseBufferPos = remaining;
        }
    }
}


// Web UI is a separate module (WebUI.*)

void setup() {
    // TX ring buffer so serial prints return immediately instead of blocking on the 128-byte FIFO
    // (must precede Serial.begin, which M5.begin performs).
    Serial.setTxBufferSize(1024);
    M5.begin(true, false, false);
    // Atom Echo has one LED; the library default clocks out a 25-LED matrix on every show().
    M5.dis.begin(1);
    M5.dis.setTaskName("LEDs");
    M5.dis.setTaskPriority(2);
    M5.dis.setCore(1);
    M5.dis.start();

    delay(200);
    Serial.println("\n\n=== ESP32 RTSP Mic Starting ===");
    Serial.println("Board: M5Stack Atom Echo");

    senderExitSem = xSemaphoreCreateBinary();

    // Set LED to indicate startup (the capture task takes over the LED once it runs)
    M5.dis.drawpix(0, CRGB(128, 128, 0));  // Yellow for startup

    randomSeed((uint32_t)micros() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF));

    bootTime = millis(); // Store boot time
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);

    // Load settings from flash
    Serial.println("Loading settings...");
    loadAudioSettings();

    // WiFi: power save is applied from settings after the connection is up
    Serial.println("Initializing WiFi...");
    WiFi.setSleep(false);

    WiFiManager wm;
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect("ESP32-RTSP-Mic-AP")) {
        simplePrintln("WiFi failed, restarting...");
        ESP.restart();
    }

    simplePrintln("WiFi connected: " + WiFi.localIP().toString());

    // NTP time sync (EST = UTC-5, no DST)
    configTime(-5 * 3600, 0, "pool.ntp.org");
    Serial.print("Waiting for NTP time sync...");
    time_t now = 0;
    for (int i = 0; i < 20 && now < 100000; i++) {
        delay(250);
        time(&now);
    }
    if (now > 100000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        Serial.printf(" OK: %s EST\n", buf);
    } else {
        Serial.println(" failed (will use uptime)");
    }

    // Apply configured WiFi TX power / power save after connect
    applyWifiTxPower(true);
    applyWifiPowerSave(true);

    // mDNS: allows rtsp://atomecho.local:8554/audio
    if (MDNS.begin("atomecho")) {
        MDNS.addService("rtsp", "tcp", 8554);
        MDNS.addService("http", "tcp", 80);
        simplePrintln("mDNS: atomecho.local");
    }

    // CPU clock before the audio pipeline starts (WiFi needs >= 80 MHz)
    if (!setCpuFrequencyMhz(cpuFrequencyMhz)) {
        simplePrintln("WARNING: CPU frequency " + String(cpuFrequencyMhz) + " MHz rejected");
        cpuFrequencyMhz = (uint8_t)getCpuFrequencyMhz();
    }
    simplePrintln("CPU frequency set to " + String(getCpuFrequencyMhz()) + " MHz");

    // Audio front end: I2S driver, frame pool, HPF coefficients, then the always-on capture task
    setup_i2s_driver();
    if (!allocFramePool()) {
        simplePrintln("Continuing without a frame pool: streaming disabled");
    }
    updateHighpassCoeffs();
    startCaptureTask();

    if (!overheatLatched) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    } else {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    // Web UI
    webui_begin();

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    float initialTemp = temperatureRead();
    if (isTemperatureValid(initialTemp)) {
        maxTemperature = initialTemp;
        lastTemperatureC = initialTemp;
        lastTemperatureValid = true;
        overheatSensorFault = false;
    } else {
        maxTemperature = 0.0f;
        lastTemperatureC = 0.0f;
        lastTemperatureValid = false;
        overheatSensorFault = true;
        overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
        overheatLastTimestamp = "";
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        persistOverheatNote();
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    if (!overheatLatched) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL: rtsp://" + WiFi.localIP().toString() + ":8554/audio");
        simplePrintln("RTSP URL: rtsp://atomecho.local:8554/audio");
    } else {
        simplePrintln("RTSP server paused due to thermal latch. Clear via Web UI before resuming streaming.");
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
    Serial.printf("Free heap after init: %u KB\n", ESP.getFreeHeap() / 1024);
}

void loop() {
    // Update M5Atom (for button handling)
    M5.update();

    webui_handleClient();

    if (millis() - lastTempCheck > 60000) { // 1 min
        checkTemperature();
        lastTempCheck = millis();
    }

    // Heap monitoring (every 10 minutes — useful for detecting leaks in long deployments)
    if (millis() - lastMemoryCheck > 600000) { // 10 min
        uint32_t currentHeap = ESP.getFreeHeap();
        if (currentHeap < minFreeHeap) minFreeHeap = currentHeap;
        Serial.printf("[Heap] Current: %u KB, Min: %u KB\n", currentHeap / 1024, minFreeHeap / 1024);
        lastMemoryCheck = millis();
    }

    if (millis() - lastPerformanceCheck > (performanceCheckInterval * 60000UL)) {
        checkPerformance();
        lastPerformanceCheck = millis();
    }

    if (millis() - lastWiFiCheck > 30000) { // 30 s
        checkWiFiHealth(); // without TX power log spam
        lastWiFiCheck = millis();
    }

    checkScheduledReset();

    // WiFi power save can starve the TCP send window (5760 B per beacon-period RTT). If a session
    // drops frames while it is on, switch it off until reboot rather than keep dropping audio.
    static unsigned long lastPsCheck = 0;
    if (isStreaming && wifiPowerSave && !wifiPsSuspended && millis() - lastPsCheck > 5000) {
        lastPsCheck = millis();
        if (framesDroppedQueueFull >= WIFI_PS_DROP_LIMIT) {
            wifiPsSuspended = true;
            WiFi.setSleep(false);
            simplePrintln("WiFi power save suspended until reboot: " + String(framesDroppedQueueFull) +
                          " frames dropped (AP DTIM period too long for the TCP window?)");
        }
    }

    // Phase: the sender ended the session (on its own, or after a stop request). Only tear down
    // once it has confirmed exit; otherwise retry on the next pass.
    if (senderTaskHandle && !isStreaming) {
        if (xSemaphoreTake(senderExitSem, pdMS_TO_TICKS(200)) == pdTRUE) {
            finishStreamSession(streamEndReason);
        }
    }
    if (senderTaskHandle && stopStreamRequested && stopRequestedAt != 0 &&
        millis() - stopRequestedAt > SENDER_JOIN_LIMIT_MS && scheduledRebootAt == 0) {
        simplePrintln("Sender task did not exit for 30 s; rebooting to recover");
        scheduleReboot(false, 200);
    }

    // RTSP idle timeout — disconnect clients that connect but never stream (60s)
    if (!isStreaming && senderTaskHandle == NULL && rtspClient && rtspClient.connected()) {
        if (millis() - lastRTSPActivity > 60000) {
            simplePrintln("RTSP idle timeout — disconnecting");
            rtspClient.stop();
            rtspParseBufferPos = 0;
        }
    }

    static unsigned long lastRtspPoll = 0;
    if (rtspServerEnabled) {
        if (!isStreaming && senderTaskHandle == NULL && millis() - lastRtspPoll >= 10) {
            lastRtspPoll = millis();
            // Phase: accept a new client (never while a previous sender is still being joined)
            if (!rtspClient || !rtspClient.connected()) {
                WiFiClient newClient = rtspServer.available();
                if (newClient) {
                    rtspClient = newClient;
                    rtspClient.setNoDelay(true);   // snappy negotiation; switched off for streaming
                    rtspClientIp = rtspClient.remoteIP().toString();
                    rtspParseBufferPos = 0;
                    lastRTSPActivity = millis();
                    lastRtspClientConnectMs = millis();
                    rtspConnectCount++;
                    simplePrintln("New RTSP client: " + rtspClientIp);
                }
            }
            // Phase: RTSP negotiation
            if (rtspClient && rtspClient.connected()) {
                processRTSP(rtspClient);
            }
        }
    } else {
        // RTSP server disabled (overheat lockout / UI)
        requestStreamStop("server disabled");
    }

    // Handle deferred reboot/reset safely here
    if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt) {
        if (scheduledFactoryReset) {
            resetToDefaultSettings();
        }
        delay(50);
        ESP.restart();
    }

    // Everything above is poll-based. WebServer::handleClient already sleeps 1 ms when no HTTP
    // client is connected; this yield also covers the in-request path and bounds how often the
    // RTSP socket is polled, so core 1 idles (WFI) instead of spinning.
    delay(2);
}
