#include <WiFi.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <driver/i2s_pdm.h>
#include <Preferences.h>
#include <math.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/stream_buffer.h>
#include "App.h"
#include "AudioDsp.h"
#include "RestartInfo.h"
#include "RtspRequest.h"
#include "WebUI.h"
#include "WifiSetup.h"

// ================== PIPELINE ARCHITECTURE ==================
// Capture task (core 1, high priority):  I2S DMA -> HPF/gain/AGC -> complete RTP frame -> readyQ
// Sender task  (core 0, one per session): readyQ -> non-blocking send() on the raw socket -> freeQ
// Arduino loop (core 1, priority 1):      RTSP negotiation, Web UI, diagnostics; owns the WiFiClient
//
// Frames are allocated once (up to TARGET_QUEUE_MS of audio) and travel between cores as pointers
// in two FreeRTOS queues. The sender is the only task that touches the socket during a session, and
// it only ever sends whole frames, so the interleaved RTSP framing cannot desynchronise. The loop
// closes the WiFiClient only after the sender has confirmed exit through senderExitSem.
// PDM microphone outputs 16-bit samples directly (no bit shift).

// A frame may sit in IRAM, which takes 32-bit loads and stores only (see copyWords): hence the
// 32-bit len, which also keeps data[] on a word boundary for copyWords.
struct TxFrame {
    uint32_t len;      // valid bytes in data[]
    uint8_t  data[];   // '$' interleave header (4) + RTP header (12) + big-endian L16 PCM
};
static_assert(offsetof(TxFrame, data) % 4 == 0, "copyWords copies frame data by 32-bit words");
#define FRAME_HDR_BYTES   16
#define I2S_DMA_BUF_COUNT 8
#define I2S_DMA_BUF_LEN   512    // frames per DMA buffer; one buffer is lost per queue overflow
#define I2S_READ_TIMEOUT_MS 1000 // longest wait for one DMA buffer in i2s_channel_read()
#define TARGET_QUEUE_MS   3000   // software buffer in front of the socket; lwIP resends a lost segment after 0.4-0.6 s, then 1.2 s later
#define MIN_QUEUE_FRAMES  4
#define MAX_QUEUE_FRAMES  384    // 3 s of 256-sample frames at 32 kHz is 375
#define MAX_POOL_BYTES    (196 * 1024) // cap on the frame pool regardless of rate/buffer choice; holds 3 s at 32 kHz
#define HEAP_HEADROOM_BYTES (80 * 1024) // free heap left to WiFi/lwIP/HTTP once the pool is allocated
#define CAPTURE_STALL_MS  3000   // no complete I2S block for this long -> reinstall the driver
#define SEND_STALL_LIMIT_MS 10000 // no byte accepted by the socket for this long -> drop the client
#define SENDER_JOIN_LIMIT_MS 30000 // unconfirmed sender exit for this long -> reboot (never close under it)
#define WIFI_PS_DROP_LIMIT 10     // dropped frames in a session that suspend WiFi power save until reboot
#define RTSP_METHODS "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, SET_PARAMETER"

static TxFrame*  framePool[MAX_QUEUE_FRAMES];
static uint16_t  framePoolCount = 0;
static uint16_t  framePoolIram = 0;      // how many of them are in IRAM
static uint32_t  frameSlotBytes = 0;
static uint8_t*  frameStage = NULL;      // the capture task builds a frame for IRAM here
static uint8_t*  sendBounce = NULL;      // the sender copies a frame out of IRAM here
volatile uint16_t pipelineSamples = 0;   // samples per packet the pool is sized for; changes only while capture is parked
uint32_t appliedSampleRate = 0;          // what the I2S driver actually runs (for reverting a failed reconfiguration)
static uint16_t appliedBufferSize = 0;
static QueueHandle_t freeQ = NULL;
static QueueHandle_t readyQ = NULL;
static i2s_chan_handle_t i2sRx = NULL;        // PDM RX channel; replaced only while capture is parked
static volatile uint32_t i2sLostBufs = 0;     // DMA buffers the driver discarded (ISR counter, never reset)

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
static volatile UBaseType_t senderStackFree = 0; // sender's stack high-water mark, measured by the sender
volatile uint32_t lastCaptureOkMs = 0;         // last complete I2S read (capture watchdog)
volatile uint32_t lastMicSignalMs = 0;         // last block whose raw samples were not all identical
bool micFlat = false;                          // no signal from the microphone for MIC_FLAT_MS
#define MIC_FLAT_MS 10000

portMUX_TYPE hpfMux = portMUX_INITIALIZER_UNLOCKED;  // guards HPF coefficient hand-off

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "4.1.0"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 32000  // audio up to 16 kHz: BirdNET analyses 0-15 kHz (16 kHz cut it at 8)
#define DEFAULT_GAIN_FACTOR 3.0f
#define DEFAULT_BUFFER_SIZE 1024   // 32 ms per RTP packet at 32 kHz
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm
#define DEFAULT_CPU_MHZ 80         // Plenty for 48 kHz DSP + ~800 kbit/s TCP; runs noticeably cooler than 160
#define DEFAULT_WIFI_POWER_SAVE false
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 300

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_CONSECUTIVE_READINGS 3  // readings (1/min) at/above the limit before tripping

// -- Pins (M5Stack Atom Echo: SPM1423 PDM microphone, NS4168 speaker amplifier)
#define I2S_CLK_PIN     33  // PDM clock to the microphone (also the amplifier's LRCLK)
#define I2S_DATA_IN_PIN 23  // PDM data from the microphone
#define SPK_BCLK_PIN    19  // amplifier bit clock: unused, held low
#define SPK_DATA_PIN    22  // amplifier data: unused, held low
#define LED_PIN         27  // SK6812 RGB LED
#define BUTTON_PIN      39  // front button: low while pressed (external pull-up)
#define BUTTON_FORGET_WIFI_MS 5000  // hold this long to forget the WiFi network

// -- Network identity
#define DEFAULT_DEVICE_NAME "atomecho"        // DHCP and mDNS name (atomecho.local); a setting
#define SETUP_AP_NAME "ESP32-RTSP-Mic-AP"     // open access point of the WiFi setup portal
#define WIFI_JOIN_TIMEOUT_MS 60000            // stored network not joined within this -> setup portal
#define WIFI_DOWN_RESTART_MS (10UL * 60 * 1000) // link lost this long at runtime -> restart (stuck driver)

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
// RTSP receive buffer. Before PLAY, loop() assembles requests in it (processRTSP, length
// rtspParseBufferPos). From PLAY until the sender has confirmed its exit, the sender task owns its
// contents: it starts with the bytes that followed PLAY, and the sender keeps its own length.
uint8_t rtspParseBuffer[1024];
int rtspParseBufferPos = 0;

// -- Global state
bool rtspServerEnabled = false;   // set by setRtspServer()

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
// The PDM decoder delivers 16-bit samples; they are used as they are, with no bit shift.

// -- Audio metering / clipping diagnostics
volatile uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
volatile uint32_t audioClipCount = 0;      // total blocks where clipping occurred
volatile uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldSinceMs = 0;         // when the held peak was set (cleared after 3 s)

// -- LED mode: 0=off, 1=static, 2=level
uint8_t ledMode = 1;  // Default: static green during streaming

// -- Automatic Gain Control (AGC); the control law is agcUpdate() in AudioDsp.h
bool agcEnabled = false;
volatile float agcMultiplier = 1.0f;   // Current AGC multiplier (read by WebUI)

// -- High-pass filter (biquad, AudioDsp.h) to cut low-frequency rumble
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
Biquad hpf;                              // coefficients published by the loop task (under hpfMux)
volatile uint32_t hpfVersion = 0;        // bumped on every coefficient update
#define DC_BLOCK_HZ 10.0f                // corner of the always-on DC blocker ahead of the HPF

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastTempCheck = 0;
// Deferred reboot (requested from an HTTP handler, carried out by loop() after the reply is out)
RebootKind rebootKind = RebootKind::Plain;
bool rebootScheduled = false;
unsigned long rebootRequestedAt = 0;
uint32_t rebootDelayMs = 0;
const char* rebootNote = "";               // reported as the restart reason after the reboot
unsigned long lastI2SReset = 0;
float maxTemperature = NAN;              // highest valid reading since boot, NAN before the first
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
String overheatLastReason = "";
bool overheatSensorFault = false;
bool overheatLatched = false;          // set by a trip, kept across reboots until cleared in the Web UI

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours
static uint32_t resetIntervalFrom = 0;   // uptime (s) the interval counts from; 0 = boot

uint8_t cpuFrequencyMhz = DEFAULT_CPU_MHZ;

char deviceName[33] = DEFAULT_DEVICE_NAME;

// -- WiFi TX power / power save (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
bool wifiPowerSave = DEFAULT_WIFI_POWER_SAVE;  // WIFI_PS_MIN_MODEM when true
bool wifiPsSuspended = false;                  // power save turned off for this boot because the stream dropped frames

// -- WiFi link state: written by the WiFi event task, acted on (and logged) by loop()
volatile bool wifiLinkUp = false;
volatile uint8_t wifiDropReason = 0;
uint32_t wifiDrops = 0;                        // link losses since boot
uint8_t wifiLastDropReason = 0;                // reason code of the latest loss (0 = none yet)
uint32_t wifiLastDropAt = 0;                   // uptime (s) of the latest loss

// -- RTSP connect/PLAY statistics
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;

// ===============================================

static_assert(WIFI_POWER_MINUS_1dBm == -4 && WIFI_POWER_8_5dBm == 34 && WIFI_POWER_19_5dBm == 78,
              "applyWifiTxPower converts dBm to wifi_power_t, which counts quarter dBm");

// Apply the configured transmit power unless the driver already uses it (it falls back to its
// default whenever the station interface restarts). Logs only if log is set and it changed.
void applyWifiTxPower(bool log) {
    const wifi_power_t desired = (wifi_power_t)lroundf(snapWifiTxDbm(wifiTxPowerDbm) * 4.0f);
    if (WiFi.getTxPower() == desired) return;
    WiFi.setTxPower(desired);
    if (log) simplePrintln("WiFi TX power set to " + String((int)desired * 0.25f, 1) + " dBm");
}

// Apply WiFi modem power save. MIN_MODEM lets the radio sleep between DTIM beacons, which is the
// single largest power (heat) lever on this board, at the cost of delayed ACKs from the AP.
void applyWifiPowerSave(bool log) {
    wifiPsSuspended = false;
    WiFi.setSleep(wifiPowerSave);
    if (log) {
        simplePrintln(String("WiFi power save ") + (wifiPowerSave ? "ON (modem sleep)" : "OFF"));
    }
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass) and publish them to the capture task.
// The capture task keeps its own filter state, so changing the cutoff does not reset the filter.
void updateHighpassCoeffs() {
    if (!highpassEnabled) return;
    const float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;
    if (fc < 10.0f) fc = 10.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f;
    Biquad c;
    designHighpass(c, fs, fc);

    portENTER_CRITICAL(&hpfMux);
    hpf.b0 = c.b0; hpf.b1 = c.b1; hpf.b2 = c.b2; hpf.a1 = c.a1; hpf.a2 = c.a2;
    hpfVersion = hpfVersion + 1;
    portEXIT_CRITICAL(&hpfMux);
}

// Seconds since boot. esp_timer is 64-bit, so this does not wrap after 49 days like millis().
uint32_t uptimeSeconds() {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// Rename the device. mDNS answers to the new name at once; the DHCP client sends it from its next
// request (lease renewal or reconnect).
void setDeviceName(const char* name) {
    strlcpy(deviceName, name, sizeof(deviceName));
    WiFi.setHostname(deviceName);   // only used when the station interface starts (boot)
    if (esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_set_hostname(sta, deviceName);
    mdns_hostname_set(deviceName);
    simplePrintln(String("Device name: ") + deviceName + " (mDNS " + deviceName + ".local now, DHCP from its next request)");
}

// The name mDNS answers to: deviceName, unless another device on the network already had it, in
// which case the mDNS component picked a free one (atomecho -> atomecho-2). "" while mDNS is not
// running. One static buffer: loop task only.
const char* mdnsHostName() {
    static char name[MDNS_NAME_BUF_LEN];
    if (mdns_hostname_get(name) != ESP_OK) name[0] = '\0';
    return name;
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

static bool isTemperatureValid(float temp) {
    if (isnan(temp) || isinf(temp)) return false;
    if (temp < -20.0f || temp > 130.0f) return false;
    return true;
}

static void persistOverheatNote() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();
}

void recordOverheatTrip(float temp) {
    char when[40];
    time_t now;
    time(&now);
    if (now > 1600000000) {   // SNTP has synced
        struct tm ti;
        gmtime_r(&now, &ti);
        strftime(when, sizeof(when), "on %Y-%m-%d at %H:%M UTC", &ti);
    } else {
        snprintf(when, sizeof(when), "after %s of uptime", formatUptime(uptimeSeconds()).c_str());
    }
    overheatLastReason = "Thermal protection stopped the stream " + String(when) + ": " + String(temp, 1) +
                         " °C for " + String(OVERHEAT_CONSECUTIVE_READINGS) + " minutes (limit " +
                         String(overheatShutdownC, 0) + " °C).";
    overheatLatched = true;
    simplePrintln(overheatLastReason);
    simplePrintln("TIP: The ESP32 internal sensor reads high. Try WiFi power save, 80 MHz CPU, or a higher limit.");
    persistOverheatNote();
}

static void stopListening(const char* why);   // LIVE LISTENING

// Temperature monitoring + thermal protection.
// The ESP32 (classic) die sensor is uncalibrated and typically reads 10-20 C above the real die
// temperature, so a single reading is not trusted: the limit must be met on
// OVERHEAT_CONSECUTIVE_READINGS consecutive checks (one per minute) before the stream is stopped.
void checkTemperature() {
    static uint8_t overLimitCount = 0;
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    if (!isTemperatureValid(temp)) {
        lastTemperatureValid = false;
        overLimitCount = 0;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal protection paused.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;
    if (overheatSensorFault) {
        overheatSensorFault = false;
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }
    if (isnan(maxTemperature) || temp > maxTemperature) {
        maxTemperature = temp;
    }

    // Once latched the server stays off until the latch is cleared in the Web UI.
    if (overheatProtectionEnabled && !overheatLatched && temp >= overheatShutdownC) {
        overLimitCount++;
        if (overLimitCount >= OVERHEAT_CONSECUTIVE_READINGS) {
            overLimitCount = 0;
            recordOverheatTrip(temp);
            setRtspServer(false);
            stopListening("thermal protection");
        } else {
            simplePrintln("Temperature " + String(temp, 1) + " °C at/above limit (" +
                          String(overLimitCount) + "/" + String(OVERHEAT_CONSECUTIVE_READINGS) + ")");
        }
    } else {
        overLimitCount = 0;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    float warnThreshold = max(overheatShutdownC - 5.0f, (float)OVERHEAT_MIN_LIMIT_C);
    if (temp > warnThreshold && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected (" + String(temp, 1) + " °C). Approaching shutdown limit.");
        lastTempWarn = millis();
    }
}

// Start or stop the RTSP listener. Stopping ends the current session and drops a client that is
// still negotiating. Starting is refused (returns false) while the thermal latch is set.
bool setRtspServer(bool on) {
    if (on) {
        if (overheatLatched) return false;
        if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
        }
        return true;
    }
    rtspServerEnabled = false;
    rtspServer.stop();
    requestStreamStop("server stopped");   // if the sender does not confirm, loop() keeps trying
    if (!senderTaskHandle && rtspClient) {
        rtspClient.stop();
        rtspParseBufferPos = 0;
    }
    return true;
}

void clearThermalLatch() {
    if (!overheatLatched) return;
    overheatLatched = false;
    overheatLastReason = "";
    persistOverheatNote();
    setRtspServer(true);
}

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        wifiLinkUp = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        wifiDropReason = info.wifi_sta_disconnected.reason;
        wifiLinkUp = false;
    }
}

// WiFi link supervision, once a second from loop(). The core reconnects by itself for most
// disconnect reasons; this logs the transitions and nudges it if the link stays down. A running
// RTSP session is left to the sender: a disconnection clears the station's address and lwIP then
// aborts every TCP connection on it (ESP-IDF 5.5 has no option to keep them), so the sender's
// next send fails and the session ends; the client reconnects once the link is back.
static void checkWiFi() {
    static bool wasUp = true;
    static unsigned long downSince = 0, lastKick = 0, lastWeakWarn = 0;
    const unsigned long now = millis();
    const bool up = wifiLinkUp;
    if (up != wasUp) {
        wasUp = up;
        if (!up) {
            downSince = lastKick = now;
            wifiDrops++;
            wifiLastDropReason = wifiDropReason;
            wifiLastDropAt = uptimeSeconds();
            simplePrintln(String("WiFi lost (") + WiFi.disconnectReasonName((wifi_err_reason_t)wifiDropReason) +
                          "), reconnecting");
        } else {
            simplePrintln("WiFi back after " + String((now - downSince) / 1000) + " s: " +
                          WiFi.localIP().toString() + ", RSSI " + String(WiFi.RSSI()) + " dBm");
            applyWifiTxPower(false);
        }
    }
    if (!up && now - lastKick >= 15000) {
        lastKick = now;
        WiFi.reconnect();
    }
    if (!up && now - downSince >= WIFI_DOWN_RESTART_MS) {
        // Nothing streams without the link; a restart also resets a WiFi driver that stopped retrying.
        simplePrintln("WiFi down for 10 minutes; restarting");
        restartWithNote("recovery: WiFi down for 10 min");
    }
    if (up && now - lastWeakWarn >= 600000) {
        const int32_t rssi = WiFi.RSSI();
        if (rssi < -80) {
            lastWeakWarn = now;
            simplePrintln("WARNING: weak WiFi signal (" + String(rssi) + " dBm); expect retransmissions and stalls");
        }
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled) return;

    if (uptimeSeconds() - resetIntervalFrom >= resetIntervalHours * 3600) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        restartWithNote("scheduled reboot");
    }
}

// Change the reboot schedule (Web UI). The interval counts from boot, or from this change when the
// device has already been up for the new interval, so that saving it does not restart the device
// at once. Saving the same values changes nothing.
void setScheduledReset(bool enabled, uint32_t hours) {
    if (enabled == scheduledResetEnabled && hours == resetIntervalHours) return;
    scheduledResetEnabled = enabled;
    resetIntervalHours = hours;
    const uint32_t up = uptimeSeconds();
    resetIntervalFrom = up >= hours * 3600 ? up : 0;
}

// Load settings from flash
void loadAudioSettings() {
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    scheduledResetEnabled = audioPrefs.getBool("schedReset", false);
    resetIntervalHours = audioPrefs.getUInt("resetHours", 24);
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", DEFAULT_CPU_MHZ);
    if (cpuFrequencyMhz != 80 && cpuFrequencyMhz != 160 && cpuFrequencyMhz != 240) {
        cpuFrequencyMhz = DEFAULT_CPU_MHZ;   // 2.3.x could store 120 (rejected by the PLL) or 40 (breaks WiFi)
    }
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    wifiPowerSave = audioPrefs.getBool("wifiPs", DEFAULT_WIFI_POWER_SAVE);
    if (audioPrefs.isKey("hostname")) audioPrefs.getString("hostname", deviceName, sizeof(deviceName));
    if (!isValidDeviceName(deviceName)) strlcpy(deviceName, DEFAULT_DEVICE_NAME, sizeof(deviceName));
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
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    audioPrefs.end();

    // Sanity clamps (corrupted flash, or a value older versions accepted, must not produce an
    // unusable pipeline). 8 kHz was allowed before but clocks the microphone below its range.
    if (!isSupportedSampleRate(currentSampleRate)) currentSampleRate = DEFAULT_SAMPLE_RATE;
    if (currentBufferSize < MIN_BUFFER_SIZE || currentBufferSize > MAX_BUFFER_SIZE) currentBufferSize = DEFAULT_BUFFER_SIZE;
    if (!(currentGainFactor >= 0.1f && currentGainFactor <= 100.0f)) currentGainFactor = DEFAULT_GAIN_FACTOR;
    if (resetIntervalHours < 1 || resetIntervalHours > 168) resetIntervalHours = 24;
    if (highpassCutoffHz < 10 || highpassCutoffHz > 10000) highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = snapWifiTxDbm(wifiTxPowerDbm);
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
    audioPrefs.putBool("schedReset", scheduledResetEnabled);
    audioPrefs.putUInt("resetHours", resetIntervalHours);
    audioPrefs.putUChar("cpuFreq", cpuFrequencyMhz);
    audioPrefs.putFloat("wifiTxDbm", wifiTxPowerDbm);
    audioPrefs.putBool("wifiPs", wifiPowerSave);
    audioPrefs.putString("hostname", deviceName);
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
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// Reboot (after restoring defaults or forgetting the WiFi network, if asked) from loop() once
// delayMs has passed, so an HTTP handler can send its reply before the device goes away. `why` is
// shown as the restart reason after the reboot; it must be a string literal.
void scheduleReboot(RebootKind kind, uint32_t delayMs, const char* why) {
    rebootKind = kind;
    rebootRequestedAt = millis();
    rebootDelayMs = delayMs;
    rebootNote = why;
    rebootScheduled = true;
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
    scheduledResetEnabled = false;
    resetIntervalHours = 24;
    cpuFrequencyMhz = DEFAULT_CPU_MHZ;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
    wifiPowerSave = DEFAULT_WIFI_POWER_SAVE;
    highpassEnabled = DEFAULT_HPF_ENABLED;
    highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    agcEnabled = false;
    agcMultiplier = 1.0f;
    ledMode = 1;
    strlcpy(deviceName, DEFAULT_DEVICE_NAME, sizeof(deviceName));
    overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLastReason = "";
    overheatLatched = false;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Minimal print helpers: Serial + buffered for Web UI
// Timestamp prefix for log messages (NTP time if available, uptime fallback)
// "[14:03:12Z] " in UTC once SNTP has synced, "[+00:00:12] " (time since boot) before that.
static String logTimestamp() {
    char buf[24];
    time_t now;
    time(&now);
    if (now > 1600000000) {
        struct tm ti;
        gmtime_r(&now, &ti);
        strftime(buf, sizeof(buf), "[%H:%M:%SZ] ", &ti);
        return String(buf);
    }
    unsigned long s = uptimeSeconds();
    unsigned long h = s / 3600; s %= 3600;
    unsigned long m = s / 60; s %= 60;
    snprintf(buf, sizeof(buf), "[+%02lu:%02lu:%02lu] ", h, m, s);
    return String(buf);
}

void simplePrintln(String message) {
    String stamped = logTimestamp() + message;
    Serial.println(stamped);
    webui_pushLog(stamped.c_str());
}

// ================== AUDIO PIPELINE: FRAME POOL ==================

// IRAM takes 32-bit loads and stores only; a byte or 16-bit access raises LoadStoreError, and
// memcpy, lwIP and the DSP make them. Frames in IRAM are filled and emptied with this loop
// (volatile stops the compiler from turning it into a memcpy call). Copies whole words: buffers
// are sized in words (allocFramePool).
static void copyWords(void* dst, const void* src, size_t bytes) {
    volatile uint32_t* d = (volatile uint32_t*)dst;
    const volatile uint32_t* s = (const volatile uint32_t*)src;
    for (size_t i = 0; i < (bytes + 3) / 4; i++) d[i] = s[i];
}

static void freeFramePool() {
    if (freeQ)  { vQueueDelete(freeQ);  freeQ = NULL; }
    if (readyQ) { vQueueDelete(readyQ); readyQ = NULL; }
    for (uint16_t i = 0; i < framePoolCount; i++) {
        free(framePool[i]);
        framePool[i] = NULL;
    }
    free(frameStage);
    free(sendBounce);
    frameStage = NULL;
    sendBounce = NULL;
    framePoolCount = 0;
    framePoolIram = 0;
    frameSlotBytes = 0;
}

// Allocate frames for TARGET_QUEUE_MS of audio at the current rate/buffer size, one heap block
// per frame. The heap is split over several memory regions, so its largest block is much smaller
// than the total free and a single allocation would cap the queue far below what fits. Frames go
// first into the IRAM that code leaves free (about 48 KB, which nothing else allocates from), then
// into byte-addressable RAM until HEAP_HEADROOM_BYTES are left.
// Only call while the capture task is parked (or not yet started) and no sender is running.
static bool allocFramePool() {
    freeFramePool();
    pipelineSamples = currentBufferSize;   // the capture task reads this, never currentBufferSize directly
                                           // (set even if the pool fails so metering keeps running)
    const uint32_t perFrame = (uint32_t)currentBufferSize * 1000UL;   // frames = ms * rate / perFrame
    uint32_t want = (TARGET_QUEUE_MS * currentSampleRate + perFrame - 1) / perFrame;
    if (want > MAX_QUEUE_FRAMES) want = MAX_QUEUE_FRAMES;
    const uint32_t frameBytes = (FRAME_HDR_BYTES + (uint32_t)currentBufferSize * 2 + 3) & ~3u;   // whole words
    frameSlotBytes = sizeof(TxFrame) + frameBytes;
    if (want > MAX_POOL_BYTES / frameSlotBytes) want = MAX_POOL_BYTES / frameSlotBytes;

    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
    const size_t heapBefore = heap_caps_get_free_size(caps);
    uint16_t n = 0;
    frameStage = (uint8_t*)heap_caps_malloc(frameBytes, caps);
    sendBounce = (uint8_t*)heap_caps_malloc(frameBytes, caps);
    while (frameStage && sendBounce && n < want) {
        TxFrame* f = (TxFrame*)heap_caps_malloc(frameSlotBytes, MALLOC_CAP_EXEC);
        // Once IRAM is full, MALLOC_CAP_EXEC hands out byte-addressable RAM through its IRAM alias
        // (D/IRAM, RTC fast memory), which HEAP_HEADROOM_BYTES must keep. esp_ptr_in_iram() takes
        // in the first 40 KB of the D/IRAM alias, hence the second test.
        if (f && (!esp_ptr_in_iram(f) || esp_ptr_in_diram_iram(f))) { free(f); f = NULL; }
        if (!f) break;
        f->len = 0;
        framePool[n++] = f;
    }
    if (n <= 2) {   // IRAM frames that do not outnumber the two buffers they need gain nothing
        while (n) { free(framePool[--n]); framePool[n] = NULL; }
        free(frameStage);
        free(sendBounce);
        frameStage = NULL;
        sendBounce = NULL;
    }
    framePoolIram = n;
    while (n < want && heap_caps_get_free_size(caps) >= HEAP_HEADROOM_BYTES + frameSlotBytes) {
        TxFrame* f = (TxFrame*)heap_caps_malloc(frameSlotBytes, caps);
        if (!f) break;
        f->len = 0;
        framePool[n++] = f;
    }
    framePoolCount = n;
    if (n >= MIN_QUEUE_FRAMES) {
        freeQ = xQueueCreate(n, sizeof(TxFrame*));
        readyQ = xQueueCreate(n, sizeof(TxFrame*));
    }
    if (!freeQ || !readyQ) {
        freeFramePool();
        simplePrintln("FATAL: cannot allocate the audio frame pool (free heap " + String(heapBefore / 1024) + " KB)");
        return false;
    }
    for (uint16_t i = 0; i < n; i++) xQueueSend(freeQ, &framePool[i], 0);
    appliedBufferSize = currentBufferSize;
    const uint32_t poolMs = (uint32_t)((uint64_t)n * perFrame / currentSampleRate);
    simplePrintln("Frame pool: " + String(n) + " x " + String(frameSlotBytes) + " B (" + String(poolMs) +
                  " ms of audio), " + String(framePoolIram) + " in IRAM, free heap " +
                  String(heapBefore / 1024) + " -> " + String(heap_caps_get_free_size(caps) / 1024) + " KB");
    if (poolMs < TARGET_QUEUE_MS) {
        // n == want: a size cap set want below the target; n < want: free heap ran out first.
        simplePrintln("NOTE: frame pool below the " + String(TARGET_QUEUE_MS) + " ms target (" +
                      (n < want ? "free heap" : "size cap") + "); WiFi stalls over " +
                      String(poolMs) + " ms drop audio. A lower sample rate lengthens it.");
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

// The RTSP client's address while one is connected (negotiating or streaming), else "". Never
// touches the socket during a session: the sender task owns it then.
const char* currentRtspClient() {
    if (senderTaskHandle) return rtspClientIp.c_str();
    return rtspClient.connected() ? rtspClientIp.c_str() : "";
}

uint16_t streamQueueDepth() {
    return readyQ ? (uint16_t)uxQueueMessagesWaiting(readyQ) : 0;
}

uint16_t streamQueueFrames() {
    return framePoolCount;
}

// ================== I2S (PDM microphone) ==================
// Called from the driver's DMA interrupt when a buffer finishes while the queue of finished
// buffers is full: the driver has just discarded the oldest one. The core is built with
// CONFIG_I2S_ISR_IRAM_SAFE, so the callback must be in IRAM.
static bool IRAM_ATTR onI2sRxOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    i2sLostBufs = i2sLostBufs + 1;
    return false;
}

// Returns true once the channel runs at currentSampleRate.
bool setup_i2s_driver() {
    if (i2sRx) {
        i2s_channel_disable(i2sRx);
        i2s_del_channel(i2sRx);
        i2sRx = NULL;
    }

    // Hardware ring: 8 buffers x 512 frames. The driver queues up to 7 finished buffers for the
    // capture task, so 7 x 512 = 3584 samples (112 ms at 32 kHz, 224 ms at 16 kHz) can wait
    // before samples are lost. The capture task reads whole buffers only: once 6 of them wait, the
    // driver moves on and drops the unread rest of a partly read buffer without reporting it.
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chanCfg.dma_frame_num = I2S_DMA_BUF_LEN;

    // PDM RX needs only the clock and data in. M5's example also routes the speaker's bit clock
    // and data because it shares the config with playback; a microphone does not. The SPM1423 is
    // on the right slot (the legacy driver's ALL_RIGHT); the PDM clock is 64 x the sample rate.
    i2s_pdm_rx_config_t pdmCfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(currentSampleRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {},
    };
    pdmCfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
    pdmCfg.gpio_cfg.clk = (gpio_num_t)I2S_CLK_PIN;
    pdmCfg.gpio_cfg.din = (gpio_num_t)I2S_DATA_IN_PIN;

    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = onI2sRxOverflow;

    esp_err_t err = i2s_new_channel(&chanCfg, NULL, &i2sRx);
    if (err == ESP_OK) err = i2s_channel_init_pdm_rx_mode(i2sRx, &pdmCfg);
    if (err == ESP_OK) err = i2s_channel_register_event_callback(i2sRx, &callbacks, NULL);
    if (err == ESP_OK) err = i2s_channel_enable(i2sRx);
    if (err != ESP_OK) {
        // appliedSampleRate keeps the last rate that ran. With no channel the capture task reads
        // nothing, so the capture watchdog in loop() retries the setup.
        simplePrintln("ERROR: I2S setup failed: " + String((int)err));
        if (i2sRx) i2s_del_channel(i2sRx);
        i2sRx = NULL;
        return false;
    }
    appliedSampleRate = currentSampleRate;

    simplePrintln("I2S ready (PDM mode): " + String(currentSampleRate) + " Hz, " +
                  String(currentBufferSize) + " samples/packet, DMA slack " +
                  String((I2S_DMA_BUF_COUNT - 1) * I2S_DMA_BUF_LEN * 1000UL / currentSampleRate) + " ms");
    return true;
}

// ================== CAPTURE TASK (core 1) ==================
// Runs from boot for the life of the device. Always meters the signal (so the Web UI level works
// without a client) and, while a session is active, produces complete RTP frames into readyQ.
// It is also the only LED writer after setup(), which removes the LED ownership race.

// Returns the number of DMA buffers the driver discarded since the last call.
static inline uint32_t takeLostI2sBufs() {
    static uint32_t seen = 0;
    const uint32_t total = i2sLostBufs;
    const uint32_t lost = total - seen;
    seen = total;
    if (lost) i2sOverruns += lost;
    return lost;
}

// LED colours as 0xRRGGBB, already scaled to the brightness the M5Atom/FastLED setup used
// (20/255), so the LED looks and draws the same as before.
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
#define LED_STARTUP rgb(10, 10, 0)    // yellow
#define LED_SETUP   rgb(0, 10, 10)    // cyan: WiFi setup portal

static void ledShow(uint32_t c) {
    rgbLedWrite(LED_PIN, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);   // core RMT driver
}

volatile bool buttonHeld = false;   // held long enough to show that a WiFi reset is coming

// Live listening (see LIVE LISTENING below): the stream buffer the capture task copies processed
// audio into while someone listens, NULL otherwise, and a count of finished capture blocks that
// tells loop() when the capture task is done with a buffer it no longer feeds.
static StreamBufferHandle_t volatile listenFeed = NULL;
static volatile uint32_t captureBlocks = 0;

// Set by the capture task while it cannot allocate its sample buffer; loop() logs it (the capture
// task does not print).
static volatile bool captureBufFailing = false;

// Faults (thermal latch, no WiFi) show even with the LED set to off: then it is the only sign.
static uint32_t ledColorFor(bool streaming, bool clipped, float pct) {
    if (buttonHeld)      return LED_SETUP;                            // cyan: release now or the WiFi setup opens
    if (overheatLatched) return rgb(10, 0, 0);                        // red: thermal latch
    if (!wifiLinkUp)     return LED_STARTUP;                          // yellow: no WiFi, as while joining at boot
    if (!streaming) return (ledMode > 0) ? rgb(0, 0, 10) : 0;         // blue: ready
    if (ledMode == 0) return 0;
    if (ledMode == 1) return rgb(0, 10, 0);                           // static green: streaming
    if (clipped)      return rgb(20, 0, 0);                           // red: clipping
    if (pct > 0.7f)   return rgb(20, 13, 0);                          // orange: hot signal
    if (pct > 0.3f)   return rgb(0, 20, 0);                           // bright green: good level
    if (pct > 0.05f)  return rgb(0, 5, 0);                            // dim green: low signal
    return rgb(2, 0, 2);                                              // dim purple: very quiet
}

static void captureTask(void* arg) {
    int16_t* captureBuf = NULL;
    uint16_t capSamples = 0;
    uint32_t carried = 0;                  // samples in captureBuf read past the last packet

    DcBlocker dc;                          // removes the PDM decoder's DC offset (state lives here)
    uint32_t dcRate = 0;                   // sample rate dc was designed for
    Biquad localHpf;                       // coefficients + filter state live here
    uint32_t localHpfVersion = 0xFFFFFFFF;
    bool hpfWasEnabled = false;
    float localAgcMult = 1.0f;
    float appliedGain = -1.0f;             // gain at the end of the previous block (ramp start)

    uint32_t lastLed = 0xFFFFFFFF;         // impossible value forces the first draw
    unsigned long lastLedUpdate = 0;
    uint8_t* listenBlock = NULL;           // one block, little-endian, while someone listens

    for (;;) {
        // --- pause protocol: the loop task reinstalls I2S / reallocates the pool while we are parked ---
        if (capturePauseRequested) {
            if (captureBuf) { free(captureBuf); captureBuf = NULL; capSamples = 0; carried = 0; }
            if (listenBlock) { free(listenBlock); listenBlock = NULL; }   // block size may change
            capturePaused = true;
            __asm__ __volatile__("memw" ::: "memory");
            while (capturePauseRequested) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            capturePaused = false;
            localHpfVersion = 0xFFFFFFFF;  // sample rate may have changed: refetch coefficients
            localHpf.reset();
            dcRate = 0;
            lastCaptureOkMs = millis();
            lastMicSignalMs = lastCaptureOkMs;
            continue;
        }

        if (pipelineSamples == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (!captureBuf || capSamples != pipelineSamples) {
            if (captureBuf) free(captureBuf);
            capSamples = pipelineSamples;
            carried = 0;
            // One DMA buffer more than a packet: reads take whole buffers (see below).
            captureBuf = (int16_t*)malloc(((size_t)capSamples + I2S_DMA_BUF_LEN) * sizeof(int16_t));
            if (!captureBuf) {
                captureBufFailing = true;
                capSamples = 0;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            captureBufFailing = false;
        }
        const uint32_t rate = appliedSampleRate;   // changes only while this task is parked
        const i2s_chan_handle_t rx = i2sRx;        // likewise
        if (rate == 0 || !rx) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // I2S not set up
        if (rate != dcRate) {
            dc.design((float)rate, DC_BLOCK_HZ);
            dc.reset();
            dcRate = rate;
        }

        // --- one packet worth of samples (blocks in the driver until the DMA has them). Each read
        //     takes the fewest whole DMA buffers that complete the packet, appended to the samples
        //     carried over, so the driver never holds a partly read buffer (setup_i2s_driver) ---
        size_t bytesRead = 0, want = 0;
        esp_err_t r = ESP_OK;
        if (carried < capSamples) {   // else the carried samples make a whole packet
            const uint32_t bufs = (capSamples - carried + I2S_DMA_BUF_LEN - 1) / I2S_DMA_BUF_LEN;
            want = (size_t)bufs * I2S_DMA_BUF_LEN * sizeof(int16_t);
            r = i2s_channel_read(rx, captureBuf + carried, want, &bytesRead, I2S_READ_TIMEOUT_MS);
            const uint32_t lostBufs = takeLostI2sBufs();
            if (isStreaming && lostBufs) {
                // Audio the DMA discarded still happened: keep the RTP clock honest. Counted only
                // with a read: the driver drops the oldest waiting buffer, which comes after the
                // carried samples, so the gap belongs to the packet that reads past it.
                rtpTimestamp += lostBufs * I2S_DMA_BUF_LEN;
            }
        }
        if (r != ESP_OK || bytesRead != want) {
            i2sReadErrors = i2sReadErrors + 1;
            if (isStreaming) {                  // the packet is lost: count the samples thrown away
                uint32_t gap = carried + (uint32_t)(bytesRead / sizeof(int16_t));
                if (r == ESP_ERR_TIMEOUT) gap += rate * I2S_READ_TIMEOUT_MS / 1000;   // and the wait without data
                rtpTimestamp += gap;
                rtpSequence++;
            }
            carried = 0;
            if (r != ESP_OK && r != ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(20));   // a timeout has waited already
            continue;
        }
        carried += (uint32_t)(bytesRead / sizeof(int16_t));
        lastCaptureOkMs = millis();

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

        // Gain ramps linearly from the last block's value to this block's target, so AGC steps and
        // manual changes glide over one block instead of clicking at the boundary.
        const bool agcOn = agcEnabled;
        const float targetGain = currentGainFactor * (agcOn ? localAgcMult : 1.0f);
        if (appliedGain < 0.0f) appliedGain = targetGain;

        // --- output frame: only while streaming; when the pool is exhausted, recycle the OLDEST
        //     queued frame so the receiver gets the freshest audio and latency stays bounded ---
        TxFrame* frame = NULL;
        const bool streaming = isStreaming;
        if (streaming && freeQ) {
            if (xQueueReceive(freeQ, &frame, 0) != pdTRUE) {
                framesDroppedQueueFull = framesDroppedQueueFull + 1;   // either the recycled oldest frame or (never, with >= 2 frames) nothing
                xQueueReceive(readyQ, &frame, 0);
            }
        }

        // Read once per block; loop() frees a buffer it stopped feeding only after captureBlocks
        // has moved on, i.e. after this block is done with it.
        const StreamBufferHandle_t feed = listenFeed;
        const size_t blockBytes = (size_t)capSamples * sizeof(int16_t);
        if (feed && !listenBlock) listenBlock = (uint8_t*)malloc(blockBytes);
        if (!feed && listenBlock) { free(listenBlock); listenBlock = NULL; }

        // --- DSP (AudioDsp.h), written big-endian straight into the frame (or, with no RTSP
        //     session, into the listening block). A frame in IRAM is built in frameStage and
        //     copied in by words once complete ---
        uint8_t* out = frame ? (esp_ptr_in_iram(frame) ? frameStage : frame->data) : NULL;
        uint8_t* pcm = out ? out + FRAME_HDR_BYTES : (feed ? listenBlock : NULL);
        const BlockStats st = processBlock(captureBuf, capSamples, dc, hpfOn ? &localHpf : NULL,
                                           appliedGain, targetGain, pcm);
        appliedGain = targetGain;

        // Samples read past this packet start the next one (none when capSamples is a multiple of
        // I2S_DMA_BUF_LEN). Nothing below reads captureBuf.
        carried -= capSamples;
        if (carried) memmove(captureBuf, captureBuf + capSamples, carried * sizeof(int16_t));

        // --- live listening: the same audio, little-endian (WAV), whole blocks only so the
        //     listener never gets half a sample; a block that does not fit is skipped ---
        if (feed && listenBlock && pcm) {
            for (size_t i = 0; i < blockBytes; i += 2) {
                const uint8_t hi = pcm[i], lo = pcm[i + 1];
                listenBlock[i] = lo;
                listenBlock[i + 1] = hi;
            }
            if (xStreamBufferSpacesAvailable(feed) >= blockBytes) {
                xStreamBufferSend(feed, listenBlock, blockBytes, 0);
            }
        }

        if (agcOn) {
            const float rms = sqrtf(st.sumSquares / (float)capSamples) / 32767.0f;
            localAgcMult = agcUpdate(localAgcMult, rms, (float)capSamples / (float)rate);
            agcMultiplier = localAgcMult;
        } else {
            localAgcMult = 1.0f;
        }

        // --- RTP framing + enqueue ---
        if (streaming) {
            if (frame) {
                uint8_t* d = out;
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
                if (out != frame->data) copyWords(frame->data, out, FRAME_HDR_BYTES + payload);
                frame->len = FRAME_HDR_BYTES + payload;
                if (xQueueSend(readyQ, &frame, 0) != pdTRUE) {
                    xQueueSend(freeQ, &frame, 0);
                    framesDroppedQueueFull = framesDroppedQueueFull + 1;
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
        const float peakAbs = st.peak > 32767.0f ? 32767.0f : st.peak;
        lastPeakAbs16 = (uint16_t)peakAbs;
        if (st.clipped) audioClipCount = audioClipCount + 1;
        const unsigned long now = millis();
        if (!st.flat) lastMicSignalMs = now;
        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldSinceMs = now;
        } else if (peakHoldAbs16 > 0 && now - peakHoldSinceMs > 3000UL) {
            peakHoldAbs16 = 0;
        }

        // --- LED (~10 Hz, only redrawn when the colour changes) ---
        if (now - lastLedUpdate > 100) {
            lastLedUpdate = now;
            uint32_t c = ledColorFor(streaming, st.clipped, peakAbs / 32767.0f);
            if (c != lastLed) {
                ledShow(c);
                lastLed = c;
            }
        }
        captureBlocks = captureBlocks + 1;
    }
}

// Park the capture task (blocks up to ~2.5 s). Returns false if it did not park.
static bool pauseCapture() {
    if (!captureTaskHandle) return true;
    capturePauseRequested = true;
    __asm__ __volatile__("memw" ::: "memory");
    const unsigned long start = millis();
    while (!capturePaused && millis() - start < 2500) vTaskDelay(pdMS_TO_TICKS(5));
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
    lastCaptureOkMs = millis();
    lastMicSignalMs = lastCaptureOkMs;
    BaseType_t ok = xTaskCreatePinnedToCore(captureTask, "AudioCapture", 4096, NULL, 20,
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
// gives up early if the loop asks the sender to stop. On failure *reason says why.
static bool senderRespond(int fd, const char* resp, int len, const char** reason) {
    int off = 0;
    unsigned long lastProgress = millis();
    while (off < len) {
        if (stopStreamRequested) { *reason = "stop requested"; return false; }
        int w = send(fd, resp + off, len - off, MSG_DONTWAIT);
        if (w > 0) { off += w; lastProgress = millis(); continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
            *reason = "socket write error";
            return false;
        }
        if (millis() - lastProgress > SEND_STALL_LIMIT_MS) { *reason = "client stalled"; return false; }
        fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
        struct timeval tv = { 0, 50000 };
        select(fd + 1, NULL, &ws, NULL, &tv);
    }
    return true;
}

// Answer what is already in rx (at first, the bytes that came after PLAY), then drain the socket
// and answer whatever the client sent. Returns false when the session must end.
static bool senderServiceRtsp(int fd, char* rx, int& rxlen, const int rxcap, int& skipBytes, const char** reason) {
    for (;;) {
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
            const size_t headLen = (size_t)(end - rx);
            int consumed = (int)headLen + 4;
            *end = '\0';
            // Header values as rtspRequestLength() reads them during negotiation (RtspRequest.h).
            char hv[12];
            long bodyLen = 0;
            if (rtspHeaderValue(rx, headLen, "Content-Length", hv, sizeof(hv))) {
                char* e = nullptr;
                bodyLen = strtol(hv, &e, 10);
                if (e == hv || *e != '\0') bodyLen = -1;   // not a number
            }
            if (bodyLen < 0 || bodyLen > 4096) { *reason = "malformed RTSP request"; return false; }
            if (rxlen < consumed + bodyLen) {          // body not complete yet
                if (consumed + bodyLen > rxcap - 1) { skipBytes = consumed + bodyLen - rxlen; rxlen = 0; break; }
                *end = '\r';
                break;
            }
            consumed += bodyLen;

            int cseq = 1;
            if (rtspHeaderValue(rx, headLen, "CSeq", hv, sizeof(hv))) cseq = atoi(hv);
            char resp[224];
            int rl;
            bool endSession = false;
            if (strncmp(rx, "TEARDOWN", 8) == 0) {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", cseq, streamSessionId);
                endSession = true;
            } else if (strncmp(rx, "OPTIONS", 7) == 0) {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nPublic: " RTSP_METHODS "\r\n\r\n", cseq);
            } else if (strncmp(rx, "GET_PARAMETER", 13) == 0 || strncmp(rx, "SET_PARAMETER", 13) == 0 ||
                       strncmp(rx, "PLAY", 4) == 0) {
                // keepalive; PLAY while playing changes nothing
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", cseq, streamSessionId);
            } else {
                rl = snprintf(resp, sizeof(resp), "RTSP/1.0 501 Not Implemented\r\nCSeq: %d\r\n\r\n", cseq);
            }
            if (!senderRespond(fd, resp, rl, reason)) return false;
            lastRTSPActivity = millis();
            if (endSession) { *reason = "TEARDOWN"; return false; }

            memmove(rx, rx + consumed, rxlen - consumed);
            rxlen -= consumed;
        }

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
    }
}

// arg: the number of bytes startStreamSession() put in rtspParseBuffer (those that followed PLAY).
static void senderTask(void* arg) {
    const int fd = streamFd;
    TxFrame* cur = NULL;
    const uint8_t* src = NULL;   // cur's bytes: its data, or sendBounce for a frame in IRAM
    size_t off = 0;
    char* const rx = (char*)rtspParseBuffer;   // this task's until it exits
    int rxlen = (int)(intptr_t)arg, skipBytes = 0;
    unsigned long lastProgress = millis();
    unsigned long lastRtspPoll = 0;
    unsigned long stallStart = 0;
    const char* reason = "stop requested";
    uint32_t sent = 0;

    Serial.printf("[Sender] session started on core %d, fd %d\n", xPortGetCoreID(), fd);

    while (!stopStreamRequested) {
        if (!cur) {
            const unsigned long now = millis();
            // Frame boundary: the only safe point to interleave RTSP responses.
            if (now - lastRtspPoll >= 200) {
                lastRtspPoll = now;
                senderStackFree = uxTaskGetStackHighWaterMark(NULL);
                if (!senderServiceRtsp(fd, rx, rxlen, sizeof(rtspParseBuffer), skipBytes, &reason)) break;
            }
            if (xQueueReceive(readyQ, &cur, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            lastProgress = millis();   // a frame's stall clock starts here: the poll and the wait are no stall
            off = 0;
            src = cur->data;
            if (esp_ptr_in_iram(cur)) {   // lwIP copies it with byte loads
                copyWords(sendBounce, cur->data, cur->len);
                src = sendBounce;
            }
        }

        const unsigned long now = millis();   // after the RTSP poll and the queue wait, which can take seconds
        int w = send(fd, src + off, cur->len - off, MSG_DONTWAIT);
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
        if (!stallStart) { stallStart = now; sendStallCount = sendStallCount + 1; }
        if (now - lastProgress > SEND_STALL_LIMIT_MS) { reason = "client stalled"; break; }
        fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
        struct timeval tv = { 0, 50000 };
        select(fd + 1, NULL, &ws, NULL, &tv);
    }

    // A stall still open at exit (a client that stalled out, or a stop while stalled) counts too;
    // set before isStreaming drops so the session summary in loop() reads it.
    if (stallStart) {
        const uint32_t d = (uint32_t)(millis() - stallStart);
        if (d > sendStallMaxMs) sendStallMaxMs = d;
    }

    // --- cleanup: leave the socket alone (the loop closes it), recycle frames, confirm exit ---
    streamEndReason = reason;
    isStreaming = false;
    __asm__ __volatile__("memw" ::: "memory");
    if (cur) xQueueSend(freeQ, &cur, 0);
    flushReadyQueue();
    Serial.printf("[Sender] session ended: %s (sent %lu)\n", reason, (unsigned long)sent);
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

// Sums of earlier sessions' counters: the counters restart at every session, /metrics needs totals.
static StreamTotals pastSessions = {0, 0, 0, 0, 0};

StreamTotals streamTotals() {
    return {pastSessions.sent + audioPacketsSent, pastSessions.dropped + framesDroppedQueueFull,
            pastSessions.overruns + i2sOverruns, pastSessions.readErrors + i2sReadErrors,
            pastSessions.stalls + sendStallCount};
}

// Loop task: hand the negotiated client to a new sender task. Called right after the PLAY reply.
static bool startStreamSession(WiFiClient& client) {
    if (senderTaskHandle || !readyQ) return false;
    // The sender reads the raw socket, so everything the client sent after PLAY goes to it in
    // rtspParseBuffer: what processRTSP() left there, then all the WiFiClient holds (available()
    // counts its own buffer and the socket's).
    int staged = rtspParseBufferPos;
    for (int avail = client.available(); avail > 0; avail = client.available()) {
        if (avail > (int)sizeof(rtspParseBuffer) - 1 - staged) return false;   // does not fit
        const int n = client.read(rtspParseBuffer + staged, avail);
        if (n <= 0) break;
        staged += n;
    }
    flushReadyQueue();
    strlcpy(streamSessionId, rtspSessionId.c_str(), sizeof(streamSessionId));
    pastSessions = streamTotals();
    audioPacketsSent = 0;
    framesDroppedQueueFull = 0;
    i2sOverruns = 0;
    i2sReadErrors = 0;
    queueHighWater = 0;
    sendStallMaxMs = 0;
    sendStallCount = 0;
    senderStackFree = 0;
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

    BaseType_t ok = xTaskCreatePinnedToCore(senderTask, "RtpSender", 6144, (void*)(intptr_t)staged, 8,
                                            &senderTaskHandle, 0);
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

// ================== LIVE LISTENING (Web UI, core 0) ==================
// GET /api/listen hands its HTTP connection to a listener task that sends the audio as an endless
// WAV file. The capture task copies every processed block into listener.buf (little-endian) and
// the task forwards it with non-blocking writes. Nothing comes from the RTSP frame pool, so
// listening does not disturb a BirdNET-Go session, and when the listener falls behind only its
// own blocks are skipped. One listener at a time.
// Ownership: loop() creates the buffer and keeps the WiFiClient; the task is the only user of the
// socket while it runs; loop() closes the socket after the task has exited and frees the buffer
// once the capture task has also finished a block without it.

#define LISTEN_BUFFER_MS 250             // audio buffered for the listener
#define LISTEN_HEAP_MARGIN (32 * 1024)   // free heap that must remain once listening has started
#define LISTEN_TASK_STACK 4096
#define LISTEN_TASK_NAME "Listen"        // also how restartI2S finds a listener task not yet freed
#define LISTEN_STALL_LIMIT_MS 5000       // listener accepted no data for this long -> drop it

static struct {
    StreamBufferHandle_t buf = NULL;
    TaskHandle_t task = NULL;
    WiFiClient client;                   // keeps the socket open once WebServer drops its copy
    int fd = -1;
    uint8_t head[160];                   // HTTP response head + WAV header, sent by the task first
    size_t headLen = 0;
    volatile bool stop = false;          // loop() asks the task to end
    volatile bool exited = false;        // the task's last action
    const char* volatile endReason = "";
    uint32_t cutAtBlock = 0;             // captureBlocks when the feed was cut
    unsigned long startedMs = 0;
    String ip;
} listener;

static bool listenSendAll(int fd, const uint8_t* p, size_t n, const char** reason) {
    size_t off = 0;
    unsigned long stallStart = 0;
    while (off < n) {
        if (listener.stop) return false;
        const int w = send(fd, p + off, n - off, MSG_DONTWAIT);
        if (w > 0) { off += (size_t)w; stallStart = 0; continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
            *reason = "listener closed the connection";
            return false;
        }
        const unsigned long now = millis();
        if (!stallStart) stallStart = now;
        else if (now - stallStart > LISTEN_STALL_LIMIT_MS) { *reason = "listener stalled"; return false; }
        fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
        struct timeval tv = { 0, 50000 };
        select(fd + 1, NULL, &ws, NULL, &tv);
    }
    return true;
}

static void listenTask(void* arg) {
    uint8_t chunk[1024];                 // on the task's stack: memory only while someone listens
    const int fd = listener.fd;
    const char* reason = "stopped";
    unsigned long lastPoll = 0;
    if (listenSendAll(fd, listener.head, listener.headLen, &reason)) {
        while (!listener.stop) {
            if (millis() - lastPoll >= 200) {   // notice a closed connection even while no audio flows
                lastPoll = millis();
                char rx[32];
                const int r = recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
                if (r == 0) { reason = "listener closed the connection"; break; }
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { reason = "socket error"; break; }
            }
            const size_t n = xStreamBufferReceive(listener.buf, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
            if (n && !listenSendAll(fd, chunk, n, &reason)) break;
        }
    }
    if (!listener.stop) listener.endReason = reason;   // else loop() already gave the reason
    __asm__ __volatile__("memw" ::: "memory");
    listener.exited = true;
    vTaskDelete(NULL);
}

// Stop copying audio for the listener. The buffer stays allocated until the capture task has
// finished a block (serviceListener).
static void cutListenFeed() {
    if (listenFeed == NULL) return;
    listenFeed = NULL;
    __asm__ __volatile__("memw" ::: "memory");
    listener.cutAtBlock = captureBlocks;
}

// Ask the listener to end, e.g. before the sample rate changes under its WAV header.
static void stopListening(const char* why) {
    if (!listener.task || listener.stop) return;
    cutListenFeed();
    listener.endReason = why;
    __asm__ __volatile__("memw" ::: "memory");
    listener.stop = true;
}

// Loop task, every pass: clean up after the listener task.
static void serviceListener() {
    if (listener.task && listener.exited) {   // gone: no longer touches the socket or the buffer
        listener.task = NULL;
        cutListenFeed();
        listener.client = WiFiClient();   // last reference: closes the socket
        listener.fd = -1;
        simplePrintln("Listening ended (" + String(listener.endReason) + ") for " + listener.ip + " after " +
                      String((millis() - listener.startedMs) / 1000) + " s");
    }
    if (!listener.task && listener.buf && listenFeed == NULL &&
        (captureBlocks != listener.cutAtBlock || capturePaused || !captureTaskHandle)) {
        vStreamBufferDelete(listener.buf);
        listener.buf = NULL;
    }
}

// Loop task, inside the /api/listen handler. Returns 0 once the listener task owns the
// connection (it answers 200 and streams), else an HTTP status with the reason in *why.
int startListening(WiFiClient client, const char** why) {
    // The same address asking again (a reloaded page, or Stop and Listen in quick succession)
    // replaces its stream. Give a listener that is ending up to a second to clear.
    if (listener.task && listener.ip == client.remoteIP().toString()) stopListening("replaced by a new request");
    for (int i = 0; i < 100 && listener.buf && (!listener.task || listener.stop || listener.exited); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        serviceListener();
    }
    if (listener.task || listener.buf) { *why = "someone is already listening"; return 409; }
    if (overheatLatched) { *why = "thermal protection is latched"; return 503; }
    const uint32_t rate = appliedSampleRate;
    if (!captureTaskHandle || rate == 0 || capturePauseRequested) { *why = "audio capture is not running"; return 503; }
    const size_t blockBytes = (size_t)pipelineSamples * 2;
    size_t bytes = (size_t)rate * 2 * LISTEN_BUFFER_MS / 1000;
    if (bytes < 2 * blockBytes) bytes = 2 * blockBytes;
    // Listening also takes the task (stack and control block) and the capture task's copy of one
    // block; the margin must remain after all of them.
    StreamBufferHandle_t buf = NULL;
    const size_t need = bytes + LISTEN_TASK_STACK + 512 + blockBytes + LISTEN_HEAP_MARGIN;
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) >= need) {
        buf = xStreamBufferCreate(bytes, 1);
    }
    if (!buf) { *why = "not enough free memory"; return 503; }

    // An endless WAV file: the header announces ~9 hours, more than anyone listens.
    int n = snprintf((char*)listener.head, sizeof(listener.head),
                     "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    wavHeader(listener.head + n, rate, 0x7FFF0000u);
    listener.headLen = (size_t)n + 44;
    listener.client = client;
    listener.client.setNoDelay(false);   // let Nagle fill segments; latency does not matter here
    listener.fd = listener.client.fd();
    listener.buf = buf;
    listener.stop = false;
    listener.exited = false;
    listener.endReason = "";
    listener.startedMs = millis();
    listener.ip = listener.client.remoteIP().toString();
    if (xTaskCreatePinnedToCore(listenTask, LISTEN_TASK_NAME, LISTEN_TASK_STACK, NULL, 5, &listener.task, 0) != pdPASS) {
        listener.task = NULL;
        listener.client = WiFiClient();   // WebServer still holds the connection and answers with the error
        listener.buf = NULL;
        vStreamBufferDelete(buf);
        *why = "cannot start the listener";
        return 503;
    }
    __asm__ __volatile__("memw" ::: "memory");
    listenFeed = buf;
    simplePrintln("Listening started for " + listener.ip);
    return 0;
}

const char* currentListener() {
    return listener.task ? listener.ip.c_str() : "";
}

// Put the last working rate/buffer back into the settings (and flash) after a failed change.
static void revertAudioConfig(uint32_t rate, uint16_t buf, const char* why) {
    if (rate != 0 && buf != 0 && (currentSampleRate != rate || currentBufferSize != buf)) {
        currentSampleRate = rate;
        currentBufferSize = buf;
        saveAudioSettings();
    }
    simplePrintln(String("Audio reconfiguration aborted (") + why + "); keeping " +
                  String(currentSampleRate) + " Hz / " + String(currentBufferSize) + " samples");
}

// Loop task, with capture parked, after stopListening(): wait up to a second for the listener to
// end and for serviceListener() to release its connection and buffer, so that memory is free when
// the frame pool is sized. A task that deletes itself is freed later by an idle task, and until
// then xTaskGetHandle() still finds it.
static void awaitListenerGone() {
    for (int i = 0; i < 100; i++) {
        serviceListener();
        if (!listener.task && !listener.buf && xTaskGetHandle(LISTEN_TASK_NAME) == NULL) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Reconfigure the audio front end (sample rate / buffer size). Ends any active session; the
// client reconnects and gets the new parameters through DESCRIBE. Returns true when the driver
// runs and the frame pool holds frames for currentSampleRate / currentBufferSize; false when it
// gave up before touching the driver (sender or capture task busy) or had to put the previous
// values back. Without working previous values (none ran since boot) a failure leaves the
// settings as they are.
bool restartI2S() {
    const uint32_t prevRate = appliedSampleRate;   // last configuration that actually ran
    const uint16_t prevBuf = appliedBufferSize;
    simplePrintln("Reconfiguring audio: " + String(currentSampleRate) + " Hz, " +
                  String(currentBufferSize) + " samples/packet");
    stopListening("audio reconfiguration");   // its WAV header has the old rate
    if (!requestStreamStop("audio reconfiguration")) {
        revertAudioConfig(prevRate, prevBuf, "sender task busy");   // pool/queues must not change under a live sender
        return false;
    }
    if (rtspClient) {
        // Still negotiating: the SDP it got from DESCRIBE may name the old rate. It reconnects
        // and asks again.
        simplePrintln("Closing RTSP client " + rtspClientIp + " that was still negotiating");
        rtspClient.stop();
        rtspParseBufferPos = 0;
    }
    if (!pauseCapture()) {
        revertAudioConfig(prevRate, prevBuf, "capture task busy");
        return false;
    }
    awaitListenerGone();

    const char* failed = NULL;
    if (!setup_i2s_driver()) failed = "I2S setup failed";
    else if (!allocFramePool()) failed = "frame pool allocation failed";
    bool ok = failed == NULL;
    if (failed && prevRate != 0 && prevBuf != 0) {
        // Go back to the values that worked so streaming stays possible. When they are the ones
        // just tried (a restart without changes), this is a second attempt.
        const bool unchanged = currentSampleRate == prevRate && currentBufferSize == prevBuf;
        revertAudioConfig(prevRate, prevBuf, failed);
        ok = setup_i2s_driver() && allocFramePool() && unchanged;
    }
    updateHighpassCoeffs();
    resumeCapture();
    if (!i2sRx) simplePrintln("Audio input not running: the capture watchdog retries the I2S setup every 30 s");
    else if (!readyQ) simplePrintln("Frame pool unavailable: PLAY will be refused until the audio restarts with enough memory");
    else simplePrintln("Audio pipeline restarted");
    return ok;
}

// ================== RTSP NEGOTIATION (loop task, only while not streaming) ==================

// Value of a header in a request's header block, "" when absent (RtspRequest.h).
static String rtspHeader(const String& req, const char* name) {
    char v[160];
    return rtspHeaderValue(req.c_str(), req.length(), name, v, sizeof(v)) ? String(v) : String();
}

// Replies are written with one print each: the socket has TCP_NODELAY set during negotiation, so
// every print would be its own TCP segment.
void handleRTSPCommand(WiFiClient &client, const String& request) {
    String cseq = rtspHeader(request, "CSeq");
    if (cseq.length() == 0) cseq = "0";
    const String ok = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n";

    lastRTSPActivity = millis();

    if (request.startsWith("OPTIONS")) {
        client.print(ok + "Public: " RTSP_METHODS "\r\n\r\n");

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
        client.print(ok + "Content-Type: application/sdp\r\nContent-Base: rtsp://" + ip + ":8554/audio/\r\n"
                     "Content-Length: " + String(sdp.length()) + "\r\n\r\n" + sdp);

    } else if (request.startsWith("SETUP")) {
        // Only TCP interleaved is offered. Answering 461 to a UDP request makes clients (VLC, some
        // ffmpeg configs) fall back to TCP immediately instead of failing their first attempt.
        const String transport = rtspHeader(request, "Transport");
        if (transport.length() && transport.indexOf("RTP/AVP/TCP") < 0) {
            client.print("RTSP/1.0 461 Unsupported Transport\r\nCSeq: " + cseq + "\r\n\r\n");
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
        client.print(ok + "Session: " + rtspSessionId + ";timeout=60\r\nTransport: RTP/AVP/TCP;unicast;interleaved=" +
                     String(ch) + "-" + String(ch + 1) + "\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        if (rtspSessionId.length() == 0) {
            client.print("RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: " + cseq + "\r\n\r\n");
            return;
        }
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
        client.print(ok + "Session: " + rtspSessionId + "\r\nRange: npt=0.000-\r\nRTP-Info: url=rtsp://" + ip +
                     ":8554/audio/track1;seq=" + String(rtpSequence) + ";rtptime=" + String(rtpTimestamp) + "\r\n\r\n");
        // (no client.clear(): it discards the receive buffer)

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
        client.print(ok + "Session: " + rtspSessionId + "\r\n\r\n");
        rtspSessionId = "";
        simplePrintln("RTSP TEARDOWN before PLAY");

    } else if (request.startsWith("GET_PARAMETER") || request.startsWith("SET_PARAMETER")) {
        client.print(ok + "\r\n");   // keepalive

    } else {
        client.print("RTSP/1.0 501 Not Implemented\r\nCSeq: " + cseq + "\r\n\r\n");
    }
}

// RTSP request assembly (loop task, only while no session runs). Answers every complete request in
// the buffer, so a pipelined second request is not left waiting for more data, and skips request
// bodies (Content-Length) instead of parsing them as the next request.
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    int available = client.available();
    const int spaceLeft = sizeof(rtspParseBuffer) - 1 - rtspParseBufferPos;
    if (available > spaceLeft) available = spaceLeft;
    if (available > 0) {
        const int n = client.read(rtspParseBuffer + rtspParseBufferPos, available);
        if (n > 0) rtspParseBufferPos += n;
    }

    while (rtspParseBufferPos > 0 && senderTaskHandle == NULL && client.connected()) {
        char* buf = (char*)rtspParseBuffer;
        int headerLen = 0;
        const int used = rtspRequestLength(buf, rtspParseBufferPos, sizeof(rtspParseBuffer) - 1, &headerLen);
        if (used == 0) return;   // wait for the rest
        if (used < 0) {
            static unsigned long lastOverflowWarning = 0;
            if (millis() - lastOverflowWarning > 5000) {
                simplePrintln("RTSP request too large or malformed - discarded");
                lastOverflowWarning = millis();
            }
            rtspParseBufferPos = 0;
            return;
        }
        const char saved = buf[headerLen];
        buf[headerLen] = '\0';
        const String request(buf);
        buf[headerLen] = saved;
        // Out of the buffer before it is handled: after PLAY the buffer holds just what followed
        // it, which startStreamSession() hands to the sender.
        memmove(buf, buf + used, rtspParseBufferPos - used);
        rtspParseBufferPos -= used;

        handleRTSPCommand(client, request);

        if (senderTaskHandle != NULL || !client.connected()) {   // streaming now (or closed): the sender owns the socket
            rtspParseBufferPos = 0;
            return;
        }
    }
}


// Holding the button for BUTTON_FORGET_WIFI_MS forgets the WiFi network and restarts into the setup
// portal: the way to move the device to another network without the Web UI. After 1 s the LED
// turns cyan so the hold is visibly registering.
static void checkButton() {
    static unsigned long downSince = 0;
    if (digitalRead(BUTTON_PIN) != LOW) {
        downSince = 0;
        buttonHeld = false;
        return;
    }
    const unsigned long now = millis();
    if (downSince == 0) {
        downSince = now ? now : 1;
        return;
    }
    buttonHeld = now - downSince >= 1000;
    if (now - downSince >= BUTTON_FORGET_WIFI_MS) {
        simplePrintln("Button held for 5 s: forgetting the WiFi network, restarting into the setup portal");
        wifiForgetCredentials();
        restartWithNote("WiFi network forgotten (button)");
    }
}

// ================== LOOP WATCHDOG ==================
// loop() serves the Web UI and RTSP negotiation and runs every recovery check, so a pass that never
// ends leaves the device unreachable until a power cycle. The core's WebServer gives up on a client
// that has gone or stalls, but it reads a request body that keeps arriving to its end inside one
// pass, before the request's handler runs; a multipart field value, for instance, may have any
// number of lines. An esp_timer callback (core 0) restarts the device once a pass has taken
// LOOP_STUCK_MS. The setup portal's loop is watched the same way. Long work inside a single pass
// (a firmware upload, a WiFi join) calls keepLoopAlive().
#define LOOP_STUCK_MS 60000
static volatile uint32_t loopAliveMs = 0;

void keepLoopAlive() {
    loopAliveMs = millis();
}

static void checkLoopAlive(void*) {
    const uint32_t alive = loopAliveMs;   // read before millis(), as with lastCaptureOkMs
    if (millis() - alive > LOOP_STUCK_MS) restartWithNote("recovery: main loop stuck for 60 s");
}

static void startLoopWatchdog() {
    keepLoopAlive();
    const esp_timer_create_args_t args = {checkLoopAlive, NULL, ESP_TIMER_TASK, "loopWatch", false};
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, 5000000) != ESP_OK) {
        simplePrintln("WARNING: loop watchdog not started");
    }
}

// Firmware updates run on probation. The core calls this before setup(); returning true keeps a
// freshly written image in the "pending verify" state instead of accepting it at once, so if it
// restarts (crash, watchdog, a failed WiFi join ending in the portal's restart) before
// confirmFirmware() accepts it, the bootloader goes back to the previous firmware.
extern "C" bool verifyRollbackLater() {
    return true;
}

// True while the running firmware is an update not yet accepted: a restart now rolls it back.
bool firmwareOnProbation() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Accept the running firmware once it has kept WiFi for a minute (a no-op unless it was just
// installed by an update).
static void confirmFirmware() {
    static bool done = false;
    if (done || !wifiLinkUp || uptimeSeconds() < 60) return;
    done = true;
    if (firmwareOnProbation()) {
        esp_ota_mark_app_valid_cancel_rollback();
        simplePrintln("Firmware update confirmed: ran for a minute with WiFi");
    }
    audioPrefs.begin("audio", false);
    if (audioPrefs.isKey("otaFrom")) audioPrefs.remove("otaFrom");   // can no longer be rolled back
    audioPrefs.end();
}

// Once an update is written, remember the slot it was installed from. Booting from that slot
// again while the other one is marked invalid means the bootloader rolled the update back.
void noteFirmwareUpdateInstalled() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("otaFrom", esp_ota_get_running_partition()->label);
    audioPrefs.end();
}

static void checkUpdateRollback() {
    audioPrefs.begin("audio", false);
    bool rolledBack = false;
    if (audioPrefs.isKey("otaFrom")) {
        char from[17] = "";
        audioPrefs.getString("otaFrom", from, sizeof(from));
        if (strcmp(from, esp_ota_get_running_partition()->label) == 0) {
            audioPrefs.remove("otaFrom");
            rolledBack = esp_ota_get_last_invalid_partition() != NULL;
        }
    }
    audioPrefs.end();
    if (rolledBack) restartInfoMarkRollback();
}

// Web UI is a separate module (WebUI.*)

void setup() {
    // TX ring buffer so serial prints return immediately instead of blocking on the 128-byte FIFO
    // (must precede Serial.begin).
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);

    // The NS4168 speaker amplifier shares the PDM clock line with the microphone. Hold its bit
    // clock and data inputs low so it never sees a usable I2S stream and stays idle.
    pinMode(SPK_BCLK_PIN, OUTPUT);
    digitalWrite(SPK_BCLK_PIN, LOW);
    pinMode(SPK_DATA_PIN, OUTPUT);
    digitalWrite(SPK_DATA_PIN, LOW);
    pinMode(BUTTON_PIN, INPUT);   // input-only pin; the board pulls it up

    // The first write also sets up the RMT channel, here on the loop task (core 1); the capture
    // task takes over the LED once it runs.
    ledShow(LED_STARTUP);

    Serial.println("\n\n=== ESP32 RTSP Mic Starting ===");
    Serial.println("Board: M5Stack Atom Echo");
    restartInfoInit();
    checkUpdateRollback();
    simplePrintln(String("Firmware ") + FW_VERSION + ", last restart: " + restartReason());
    if (restartCrashDetails()[0]) {
        simplePrintln(String("Crash: ") + restartCrashDetails());
    }

    senderExitSem = xSemaphoreCreateBinary();

    // random() draws from the hardware generator (esp_random) as long as randomSeed() is never
    // called; a seed would switch it to libc rand() for good.
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);

    // Load settings from flash
    Serial.println("Loading settings...");
    loadAudioSettings();

    // WiFi: power save is applied from settings after the connection is up
    Serial.println("Initializing WiFi...");
    WiFi.onEvent(onWifiEvent);
    WiFi.setSleep(false);
    if (!wifiJoinStored(deviceName, WIFI_JOIN_TIMEOUT_MS)) {
        ledShow(LED_SETUP);
        simplePrintln(wifiHasStoredCredentials() ? "WiFi: stored network not reachable, starting setup portal"
                                                 : "WiFi: no network configured, starting setup portal");
        startLoopWatchdog();
        wifiRunSetupPortal(SETUP_AP_NAME, deviceName, FW_VERSION);   // restarts the device
    }
    simplePrintln("WiFi connected: " + WiFi.localIP().toString() + " on " + WiFi.SSID() +
                  ", RSSI " + String(WiFi.RSSI()) + " dBm, channel " + String(WiFi.channel()));

    // Log timestamps in UTC once SNTP has synced (in the background); uptime until then.
    configTime(0, 0, "pool.ntp.org");

    // Apply configured WiFi TX power / power save after connect
    applyWifiTxPower(true);
    applyWifiPowerSave(true);

    // mDNS: allows rtsp://<name>.local:8554/audio
    if (MDNS.begin(deviceName)) {
        MDNS.addService("rtsp", "tcp", 8554);
        MDNS.addService("http", "tcp", 80);
        simplePrintln(String("mDNS: ") + deviceName + ".local");
    }

    // CPU clock before the audio pipeline starts (WiFi needs >= 80 MHz)
    if (!setCpuFrequencyMhz(cpuFrequencyMhz)) {
        simplePrintln("WARNING: CPU frequency " + String(cpuFrequencyMhz) + " MHz rejected");
        cpuFrequencyMhz = (uint8_t)getCpuFrequencyMhz();
    }
    simplePrintln("CPU frequency set to " + String(getCpuFrequencyMhz()) + " MHz");

    // Audio front end: I2S driver, frame pool, HPF coefficients, then the always-on capture task.
    // Without I2S the rest starts anyway (Web UI, RTSP) and the capture watchdog in loop() retries.
    if (!setup_i2s_driver()) {
        simplePrintln("Continuing without audio input: the capture watchdog retries the I2S setup every 30 s");
    }
    if (!allocFramePool()) {
        simplePrintln("Continuing without a frame pool: streaming disabled");
    }
    updateHighpassCoeffs();
    startCaptureTask();

    setRtspServer(true);   // stays off while the thermal latch is set
    // Web UI
    webui_begin();

    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    float initialTemp = temperatureRead();
    if (isTemperatureValid(initialTemp)) {
        maxTemperature = initialTemp;
        lastTemperatureC = initialTemp;
        lastTemperatureValid = true;
    } else {
        overheatSensorFault = true;
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    if (!overheatLatched) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL: rtsp://" + WiFi.localIP().toString() + ":8554/audio");
        simplePrintln(String("RTSP URL: rtsp://") + deviceName + ".local:8554/audio");
    } else {
        simplePrintln("RTSP server paused due to thermal latch. Clear via Web UI before resuming streaming.");
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
    Serial.printf("Free heap after init: %u KB\n", heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) / 1024);
    startLoopWatchdog();
}

void loop() {
    keepLoopAlive();
    webui_handleClient();
    wifiImprovPoll(deviceName, FW_VERSION);

    if (millis() - lastTempCheck > 60000) { // 1 min
        checkTemperature();
        lastTempCheck = millis();
    }

    // Heap monitoring (every 10 minutes — useful for detecting leaks in long deployments)
    if (millis() - lastMemoryCheck > 600000) { // 10 min
        const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;   // not IRAM, which ESP.getFreeHeap() counts
        Serial.printf("[Heap] Current: %u KB, Min: %u KB\n", heap_caps_get_free_size(caps) / 1024,
                      heap_caps_get_minimum_free_size(caps) / 1024);
        lastMemoryCheck = millis();
    }

    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck >= 1000) {
        lastWiFiCheck = millis();
        checkWiFi();
    }

    // Session statistics on serial every 30 s. Printed here, not by the capture task: a full
    // UART buffer would block a priority-20 task that has only ~100 ms of DMA slack at 32 kHz.
    static unsigned long lastStatsLog = 0;
    if (isStreaming && millis() - lastStatsLog >= 30000) {
        lastStatsLog = millis();
        // The sender measures its own stack: it can exit at any moment, and its handle then points
        // at freed memory until finishStreamSession() clears it.
        Serial.printf("[Stream] sent=%lu dropped=%lu i2s_ovf=%lu q=%u/%u peak=%u stall_max=%lums clip=%lu "
                      "agc=%.2f stack_free capture=%u sender=%u\n",
                      (unsigned long)audioPacketsSent, (unsigned long)framesDroppedQueueFull,
                      (unsigned long)i2sOverruns, streamQueueDepth(), framePoolCount, queueHighWater,
                      (unsigned long)sendStallMaxMs, (unsigned long)audioClipCount, agcMultiplier,
                      captureTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(captureTaskHandle) : 0,
                      (unsigned)senderStackFree);
    }

    checkScheduledReset();
    checkButton();
    confirmFirmware();
    serviceListener();

    // The capture task (higher priority, same core) stamps these at any moment. Read them before
    // millis(): the other order can pair a newer stamp with an older "now", and the unsigned
    // difference then wraps to ~49 days.
    const uint32_t captureOkAt = lastCaptureOkMs, micSignalAt = lastMicSignalMs;
    const uint32_t nowMs = millis();

    // Capture watchdog. A read gives up after 1 s, so CAPTURE_STALL_MS without one complete block
    // means the driver stopped delivering; reinstalling it ends any session (the client reconnects).
    // With no I2S channel (its setup failed, at boot or since) the capture task reads nothing, so
    // this also retries the setup.
    if (captureTaskHandle && !capturePauseRequested &&
        nowMs - captureOkAt > CAPTURE_STALL_MS && nowMs - lastI2SReset > 30000) {
        lastI2SReset = nowMs;
        if (i2sRx) {
            simplePrintln("Capture stalled: no I2S data for " + String((nowMs - captureOkAt) / 1000) +
                          " s (read errors " + String(i2sReadErrors) + "); restarting audio");
        } else {
            simplePrintln("Audio input not running; retrying the I2S setup");
        }
        restartI2S();
    }

    // The capture task cannot print; it raises this flag while it cannot allocate its buffer.
    static bool captureBufFailLogged = false;
    const bool captureBufFailNow = captureBufFailing;
    if (captureBufFailNow != captureBufFailLogged) {
        captureBufFailLogged = captureBufFailNow;
        if (captureBufFailNow) simplePrintln("ERROR: the capture task has no memory for its sample buffer; retrying every second");
    }

    // Microphone check. A stuck PDM data line decodes to one constant value; a working microphone
    // varies by at least 1 LSB even in a silent room. Only judged while capture is delivering.
    const bool flatNow = nowMs - captureOkAt < 2000 && nowMs - micSignalAt > MIC_FLAT_MS;
    if (flatNow != micFlat) {
        micFlat = flatNow;
        simplePrintln(micFlat ? "WARNING: no signal from the microphone (constant samples for 10 s); check the hardware"
                              : "Microphone signal back");
    }

    // WiFi power save can starve the TCP send window (16 KB per beacon-period RTT, set in
    // platformio.ini). If a session drops frames while it is on, switch it off until reboot
    // rather than keep dropping audio.
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
        millis() - stopRequestedAt > SENDER_JOIN_LIMIT_MS && !rebootScheduled) {
        simplePrintln("Sender task did not exit for 30 s; rebooting to recover");
        scheduleReboot(RebootKind::Plain, 200, "recovery: sender task stuck");
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
                WiFiClient newClient = rtspServer.accept();
                if (newClient) {
                    rtspClient = newClient;
                    rtspClient.setNoDelay(true);   // snappy negotiation; switched off for streaming
                    rtspClientIp = rtspClient.remoteIP().toString();
                    rtspSessionId = "";   // PLAY needs a SETUP on this connection
                    rtspParseBufferPos = 0;
                    lastRTSPActivity = millis();
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
    if (rebootScheduled && millis() - rebootRequestedAt >= rebootDelayMs) {
        if (rebootKind == RebootKind::RestoreDefaults) resetToDefaultSettings();
        if (rebootKind == RebootKind::ForgetWifi) wifiForgetCredentials();   // the next boot opens the setup portal
        restartWithNote(rebootNote);
    }

    // Everything above is poll-based. WebServer::handleClient already sleeps 1 ms when no HTTP
    // client is connected; this yield also covers the in-request path and bounds how often the
    // RTSP socket is polled, so core 1 idles (WFI) instead of spinning.
    delay(2);
}
