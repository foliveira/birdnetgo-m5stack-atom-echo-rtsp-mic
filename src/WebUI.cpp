#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include "WebUI.h"

// External variables and functions from main (.ino) – ESP32 RTSP Mic for BirdNET-Go
extern WiFiServer rtspServer;
extern WiFiClient rtspClient;
extern volatile bool isStreaming;
extern uint16_t rtpSequence;
extern uint32_t rtpTimestamp;
extern unsigned long lastStatsReset;
extern unsigned long lastRtspPlayMs;
extern uint32_t rtspPlayCount;
extern unsigned long lastRtspClientConnectMs;
extern unsigned long bootTime;
extern unsigned long lastRTSPActivity;
extern unsigned long lastWiFiCheck;
extern unsigned long lastTempCheck;
extern uint32_t minFreeHeap;
extern float maxTemperature;
extern bool rtspServerEnabled;
extern volatile uint32_t audioPacketsSent;
extern volatile uint32_t framesDroppedQueueFull;
extern volatile uint32_t i2sOverruns;
extern volatile uint32_t i2sReadErrors;
extern volatile uint16_t queueHighWater;
extern volatile uint32_t sendStallMaxMs;
extern volatile uint32_t sendStallCount;
extern uint16_t streamQueueDepth();
extern uint16_t streamQueueFrames();
extern bool wifiPowerSave;
extern bool wifiPsSuspended;
extern String rtspClientIp;
extern void applyWifiPowerSave(bool log);
extern uint32_t currentSampleRate;
extern float currentGainFactor;
extern uint16_t currentBufferSize;
extern uint8_t i2sShiftBits;
extern uint32_t minAcceptableRate;
extern uint32_t performanceCheckInterval;
extern bool autoRecoveryEnabled;
extern uint8_t cpuFrequencyMhz;
extern wifi_power_t currentWifiPowerLevel;
extern void resetToDefaultSettings();
extern bool autoThresholdEnabled;
extern uint32_t computeRecommendedMinRate();
extern bool scheduledResetEnabled;
extern uint32_t resetIntervalHours;
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);
extern volatile uint16_t lastPeakAbs16;
extern volatile uint32_t audioClipCount;
extern volatile bool audioClippedLastBlock;
extern volatile uint16_t peakHoldAbs16;
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool overheatLockoutActive;
extern float overheatTripTemp;
extern unsigned long overheatTriggeredAt;
extern String overheatLastReason;
extern String overheatLastTimestamp;
extern bool overheatSensorFault;
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern bool overheatLatched;
extern bool agcEnabled;
extern volatile float agcMultiplier;
extern uint8_t ledMode;

// Local helper: snap requested Wi‑Fi TX power (dBm) to nearest supported step
static float snapWifiTxDbm(float dbm) {
    static const float steps[] = {-1.0f, 2.0f, 5.0f, 7.0f, 8.5f, 11.0f, 13.0f, 15.0f, 17.0f, 18.5f, 19.0f, 19.5f};
    float best = steps[0];
    float bestd = fabsf(dbm - steps[0]);
    for (size_t i=1;i<sizeof(steps)/sizeof(steps[0]);++i){
        float d = fabsf(dbm - steps[i]);
        if (d < bestd){ bestd = d; best = steps[i]; }
    }
    return best;
}

static const uint32_t OH_MIN = 30;
static const uint32_t OH_MAX = 95;
static const uint32_t OH_STEP = 5;

// Async reboot/factory-reset task to avoid restarting from HTTP context
static void rebootTask(void* arg){
    bool doFactory = ((uintptr_t)arg) != 0;
    if (doFactory) {
        resetToDefaultSettings();
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP.restart();
    vTaskDelete(NULL);
}

// Helper functions in main
extern float wifiPowerLevelToDbm(wifi_power_t lvl);
extern String formatUptime(unsigned long seconds);
extern String formatSince(unsigned long eventMs);
extern void restartI2S();
extern void saveAudioSettings();
extern void applyWifiTxPower(bool log);
extern const char* FW_VERSION_STR;

// Web server and in-memory log ring buffer
static WebServer web(80);
// Fixed-size ring: no heap allocation while the spinlock is held, no fragmentation over long uptimes.
static const size_t LOG_CAP = 64;
static const size_t LOG_LINE = 192;   // the session summary line is ~160 chars
static char logBuffer[LOG_CAP][LOG_LINE];
static size_t logHead = 0;
static size_t logCount = 0;

extern portMUX_TYPE logMux;

void webui_pushLog(const String &line) {
    portENTER_CRITICAL(&logMux);
    strlcpy(logBuffer[logHead], line.c_str(), LOG_LINE);
    logHead = (logHead + 1) % LOG_CAP;
    if (logCount < LOG_CAP) logCount++;
    portEXIT_CRITICAL(&logMux);
}

static String jsonEscape(const String &s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i]; if(c=='"'||c=='\\'){o+='\\';o+=c;} else if(c=='\n'){o+="\\n";} else {o+=c;}}
    return o;
}

static String profileName(uint16_t buf) {
    // Server-side fallback (English). UI localizes on client by buffer size.
    if (buf <= 256) return F("Low latency (62 packets/s, most WiFi airtime)");
    if (buf <= 512) return F("Balanced (31 packets/s)");
    if (buf <= 1024) return F("Recommended for BirdNET-Go (16 packets/s)");
    return F("Fewest packets (highest per-packet latency)");
}

static void apiSendJSON(const String &json) {
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "application/json", json);
}

// HTML UI
// The page is served straight from flash. Dynamic values (IP, version, RTSP URL) are filled by JS.
static const char INDEX_HTML[] PROGMEM =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>M5Stack Atom Echo - RTSP Microphone</title>"
        "<style>:root{--bg:#0b1020;--fg:#e7ebf2;--muted:#9aa3b2;--card:#121a2e;--border:#1b2745;--acc:#4ea1f3;--acc2:#36d399;--warn:#f59e0b;--bad:#ef4444}"
        "body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;background:linear-gradient(180deg,#0b1020 0%,#0f1530 100%);color:var(--fg)}"
        ".page{max-width:1000px;margin:0 auto;padding:16px}"
        ".hero{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
        ".brand{display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
        ".title{font-weight:700;font-size:18px;letter-spacing:.2px} .subtitle{color:var(--muted);font-size:13px}"
        ".badge{display:inline-block;border:1px solid var(--border);color:var(--muted);padding:2px 6px;border-radius:8px;font-size:12px;margin-left:8px}"
        ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:12px;margin-bottom:12px;box-shadow:0 1px 1px rgba(0,0,0,.2)}"
        ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:12px} h1{font-size:20px;margin:0 0 4px} h2{font-size:15px;margin:4px 0 10px;color:var(--muted);font-weight:600;letter-spacing:.2px}"
        "table{width:100%;border-collapse:collapse} td{padding:8px 6px;border-bottom:1px solid var(--border)} td.k{color:var(--muted);width:44%} td.v{font-weight:600}"
        "button,select,input{font:inherit;padding:8px 10px;border-radius:10px;border:1px solid var(--border);background:#0d1427;color:var(--fg)}"
        "button{background:#0e152a} button:hover{border-color:var(--acc)} button.active{background:var(--acc);color:#061120;border-color:#2a7dd4}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px} .ok{color:var(--acc2)} .warn{color:var(--warn)} .bad{color:var(--bad)} .lang{float:right} .mono{font-family:ui-monospace,Consolas,Menlo,monospace}"
        "input[type=number]{width:130px} select{min-width:110px} .muted{color:var(--muted)}"
        ".field{display:flex;align-items:center;gap:8px} .unit{color:var(--muted);font-size:12px} .help{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px;border:1px solid var(--acc);border-radius:50%;font-size:12px;color:var(--fg);margin-left:6px;background:#0a1224;cursor:pointer} .help:hover{filter:brightness(1.1)} .hint{margin-top:6px;padding:8px;border:1px solid var(--border);border-radius:8px;background:#0d162c;color:var(--fg);font-size:12px;line-height:1.35}"
        ".dirty{border-color:var(--bad)!important; box-shadow:0 0 0 2px rgba(239,68,68,.25) inset; background:#1a0d12}"
        ".gh{margin-right:10px;color:var(--acc);text-decoration:none;border:1px solid var(--border);padding:4px 8px;border-radius:8px} .gh:hover{border-color:var(--acc)}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#0c1325;border:1px solid var(--border);border-radius:10px;padding:10px;overflow:auto} pre#logs{height:45vh}"
        ".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.6);z-index:9999} .overlay .box{background:var(--card);border:1px solid var(--border);padding:16px 20px;border-radius:12px;color:var(--fg);text-align:center;min-width:260px}"
        "</style></head><body>"
        "<div id='ovr' class='overlay'><div class='box' id='ovr_msg'>Restarting…</div></div>"
        "<div class='page'>"
        "<div class='card'><div class='hero'><div><div class='brand'><div class='title' id='t_title'>M5Stack Atom Echo</div><span class='badge' id='fwv'></span></div><div class='subtitle'>URL: <a id='rtsp' class='mono' href='#' target='_blank'></a></div></div>"
        "<div class='lang'><a href='https://github.com/stedrow/birdnetgo-m5stack-atom-echo-rtsp-mic' target='_blank' class='gh'>GitHub</a>Lang: <select id='langSel'><option value='en'>English</option><option value='cs'>Čeština</option></select></div></div></div>"
        "<div class='row'>"
        "<div class='card'><h2 id='t_status'>Status</h2><table>"
        "<tr><td class='k' id='t_ip'>IP Address</td><td class='v' id='ip'></td></tr>"
        "<tr><td class='k' id='t_wifi_rssi'>WiFi RSSI</td><td class='v' id='rssi'></td></tr>"
        "<tr><td class='k' id='t_wifi_tx'>WiFi TX Power</td><td class='v' id='wtx'></td></tr>"
        "<tr><td class='k' id='t_heap'>Free Heap (min)</td><td class='v' id='heap'></td></tr>"
        "<tr><td class='k' id='t_uptime'>Uptime</td><td class='v' id='uptime'></td></tr>"
        "<tr><td class='k' id='t_rtsp_server'>RTSP Server</td><td class='v' id='srv'></td></tr>"
        "<tr><td class='k' id='t_client'>Client</td><td class='v' id='client'></td></tr>"
        "<tr><td class='k' id='t_streaming'>Streaming</td><td class='v' id='stream'></td></tr>"
        "<tr><td class='k' id='t_pkt_rate'>Packet Rate</td><td class='v' id='rate'></td></tr>"
        "<tr><td class='k'><span id='t_health'>Stream Health</span><span class='help' id='h_health'>?</span></td><td class='v' id='health'></td></tr>"
        "<tr id='row_health_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_health_hint'></div></td></tr>"
        "<tr><td class='k' id='t_last_connect'>Last RTSP Connect</td><td class='v' id='lcon'></td></tr>"
        "<tr><td class='k' id='t_last_play'>Last Stream Start</td><td class='v' id='lplay'></td></tr>"
        "</table><div class='actions'>"
        "<button onclick=\"act('server_start')\" id='b_srv_on'>Server ON</button>"
        "<button onclick=\"act('server_stop')\" id='b_srv_off'>Server OFF</button>"
        "<button onclick=\"act('reset_i2s')\" id='b_reset'>Reset I2S</button>"
        "<button onclick=\"rebootNow()\" id='b_reboot'>Reboot</button>"
        "<button onclick=\"defaultsNow()\" id='b_defaults'>Defaults</button>"
        "<div id='adv' class='footer muted'></div></div>"

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k'><span id='t_rate'>Sample Rate</span><span class='help' id='h_rate'>?</span><div class='hint' id='rate_hint' style='display:none'></div></td><td class='v'><div class='field'><input id='in_rate' type='number' step='1000' min='8000' max='48000'><span class='unit'>Hz</span><button id='btn_rate_set' onclick=\"setv('rate',in_rate.value)\">Set</button></div></td></tr>"
        "<tr id='row_rate_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_rate_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_gain'>Gain</span><span class='help' id='h_gain'>?</span></td><td class='v'><div class='field'><input id='in_gain' type='number' step='0.1' min='0.1' max='100'><span class='unit'>×</span><button id='btn_gain_set' onclick=\"setv('gain',in_gain.value)\">Set</button></div></td></tr>"
        "<tr id='row_gain_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_gain_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf'>High-pass</span><span class='help' id='h_hpf'>?</span></td><td class='v'><div class='field'><select id='sel_hp'><option value='off'>OFF</option><option value='on'>ON</option></select><button onclick=\"setv('hp_enable',sel_hp.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf_cut'>HPF Cutoff</span><span class='help' id='h_hpf_cut'>?</span></td><td class='v'><div class='field'><input id='in_hp_cutoff' type='number' step='10' min='10' max='10000'><span class='unit'>Hz</span><button onclick=\"setv('hp_cutoff',in_hp_cutoff.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_cut_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_cut_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_agc'>AGC</span><span class='help' id='h_agc'>?</span></td><td class='v'><div class='field'><select id='sel_agc'><option value='off'>OFF</option><option value='on'>ON</option></select><button id='btn_agc_set' onclick=\"setv('agc_enable',sel_agc.value)\">Set</button><span class='unit' id='agc_info'></span></div></td></tr>"
        "<tr id='row_agc_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_agc_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_led'>LED Mode</span><span class='help' id='h_led'>?</span></td><td class='v'><div class='field'><select id='sel_led'><option value='0'>OFF</option><option value='1' selected>Static</option><option value='2'>Level</option></select><button id='btn_led_set' onclick=\"setv('led_mode',sel_led.value)\">Set</button></div></td></tr>"
        "<tr id='row_led_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_led_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_buf'>Buffer Size</span><span class='help' id='h_buf'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_buf'><option>256</option><option>512</option><option selected>1024</option><option>2048</option><option>4096</option><option>8192</option></select>"
        "<span class='unit'>samples</span><button id='btn_buf_set' onclick=\"setv('buffer',sel_buf.value)\">Set</button></div></td></tr>"
        "<tr id='row_buf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_buf_hint'></div></td></tr>"
        "<tr><td class='k' id='t_latency'>Latency</td><td class='v' id='lat'></td></tr>"
        "<tr><td class='k'><span id='t_level'>Signal Level</span><span class='help' id='h_level'>?</span></td><td class='v' id='level'></td></tr>"
        "<tr id='row_level_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_level_hint'></div></td></tr>"
        "<tr><td class='k' id='t_profile'>Profile</td><td class='v' id='profile'></td></tr>"
        "</table></div>"

        "<div class='card'><h2 id='t_perf'>Reliability</h2><table>"
        "<tr><td class='k'><span id='t_auto'>Auto Recovery</span><span class='help' id='h_auto'>?</span></td><td class='v'><div class='field'><select id='in_auto'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_auto_set' onclick=\"setv('auto_recovery',in_auto.value)\">Set</button></div></td></tr>"
        "<tr id='row_auto_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_auto_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_thr_mode'>Threshold Mode</span><span class='help' id='h_thr_mode'>?</span></td><td class='v'><div class='field'><select id='in_thr_mode'><option value='auto'>Auto</option><option value='manual'>Manual</option></select><button id='btn_thrmode_set' onclick=\"setv('thr_mode',in_thr_mode.value)\">Set</button></div></td></tr>"
        "<tr id='row_thrmode_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_mode_hint'></div></td></tr>"
        "<tr id='row_thr_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_hint'></div></td></tr>"
        "<tr id='row_min_rate'><td class='k'><span id='t_thr'>Restart Threshold</span><span class='help' id='h_thr'>?</span></td><td class='v'><div class='field'><input id='in_thr' type='number' step='1' min='5' max='200'><span class='unit'>pkt/s</span><button id='btn_thr_set' onclick=\"setv('min_rate',in_thr.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_sched'>Scheduled Reset</span><span class='help' id='h_sched'>?</span></td><td class='v'><div class='field'><select id='in_sched'><option value='on'>ON</option><option value='off' selected>OFF</option></select><button id='btn_sched_set' onclick=\"setv('sched_reset',in_sched.value)\">Set</button></div></td></tr>"
        "<tr id='row_sched_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_sched_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hours'>Reset After</span><span class='help' id='h_hours'>?</span></td><td class='v'><div class='field'><input id='in_hours' type='number' step='1' min='1' max='168'><span class='unit'>h</span><button id='btn_hours_set' onclick=\"setv('reset_hours',in_hours.value)\">Set</button></div></td></tr>"
        "<tr id='row_hours_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hours_hint'></div></td></tr>"
        "</table></div>"

        ""

        "<div class='card'><h2 id='t_thermal'>Thermal</h2><table>"
        "<tr><td class='k'><span id='t_therm_protect'>Overheat Protection</span><span class='help' id='h_therm_protect'>?</span></td><td class='v'><div class='field'><select id='sel_oh_enable'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_oh_enable' onclick=\"setv('oh_enable',sel_oh_enable.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_protect' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_protect'></div></td></tr>"
        "<tr><td class='k'><span id='t_therm_limit'>Shutdown Limit</span><span class='help' id='h_therm_limit'>?</span></td><td class='v'><div class='field'><select id='sel_oh_limit'><option>30</option><option>35</option><option>40</option><option>45</option><option>50</option><option>55</option><option>60</option><option>65</option><option>70</option><option>75</option><option selected>80</option><option>85</option><option>90</option><option>95</option></select><span class='unit'>&deg;C</span><button id='btn_oh_limit' onclick=\"setv('oh_limit',sel_oh_limit.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_limit' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_limit'></div></td></tr>"
        "<tr><td class='k' id='t_therm_status'>Status</td><td class='v' id='therm_status'></td></tr>"
        "<tr><td class='k' id='t_therm_now'>Current Temp</td><td class='v' id='therm_now'></td></tr>"
        "<tr><td class='k' id='t_therm_max'>Peak Temp</td><td class='v' id='therm_max'></td></tr>"
"<tr><td class='k' id='t_therm_cpu'>CPU Clock</td><td class='v' id='therm_cpu'></td></tr>"
"<tr><td class='k'><span id='t_therm_last'>Last Shutdown</span></td><td class='v'><div id='therm_last' class='hint'></div></td></tr>"
"<tr id='row_therm_latch' style='display:none'><td colspan='2'><div class='hint warn' id='txt_therm_latch'></div><div class='field' style='margin-top:8px'><button id='btn_therm_clear' class='danger' onclick=\"clearThermalLatch()\"></button></div></td></tr>"
"</table></div>"

        "<div id='advsec'>"
        "<div class='card'><h2 id='t_advanced_settings'>Advanced Settings</h2><table>"
        "<tr><td class='k'><span id='t_shift'>I2S Shift</span><span class='help' id='h_shift'>?</span></td><td class='v'><span id=.val_shift. class=.val.>0</span> bits <span style=.color:#888;font-size:0.9em.>(fixed for PDM)</span></td></tr>"
        "<tr id='row_shift_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_shift_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_chk'>Check Interval</span><span class='help' id='h_chk'>?</span></td><td class='v'><div class='field'><input id='in_chk' type='number' step='1' min='1' max='60'><span class='unit'>min</span><button id='btn_chk_set' onclick=\"setv('check_interval',in_chk.value)\">Set</button></div></td></tr>"
        "<tr id='row_chk_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_chk_hint'></div></td></tr>"
        "<tr id='row_tx_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_tx_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wifi_tx2'>TX Power</span><span class='help' id='h_tx'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_tx'><option>-1.0</option><option>2.0</option><option>5.0</option><option>7.0</option><option>8.5</option><option>11.0</option><option>13.0</option><option selected>15.0</option><option>17.0</option><option>18.5</option><option>19.0</option><option>19.5</option></select>"
        "<span class='unit'>dBm</span><button id='btn_tx_set' onclick=\"setv('wifi_tx',sel_tx.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_wifi_ps'>WiFi Power Save</span><span class='help' id='h_wifi_ps'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_wifi_ps'><option value='off'>OFF</option><option value='on'>ON</option></select><button id='btn_wifi_ps_set' onclick=\"setv('wifi_ps',sel_wifi_ps.value)\">Set</button></div></td></tr>"
        "<tr id='row_wifi_ps_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_wifi_ps_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_cpu'>CPU Frequency</span><span class='help' id='h_cpu'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_cpu'><option>80</option><option>160</option><option>240</option></select><span class='unit'>MHz</span><button id='btn_cpu_set' onclick=\"setv('cpu_freq',sel_cpu.value)\">Set</button></div></td></tr>"
        "<tr id='row_cpu_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_cpu_hint'></div></td></tr>"
        "</table></div>"
        "</div>"

        "<div class='card'><div style='display:flex;justify-content:space-between;align-items:center'><h2 id='t_logs'>Logs</h2><button id='btn_copy_logs' onclick='copyLogs()' title='Copy logs' style='background:none;border:1px solid var(--border);padding:4px 8px;cursor:pointer;border-radius:8px;line-height:1'><svg width='16' height='16' viewBox='0 0 16 16' fill='none' stroke='currentColor' stroke-width='1.5'><rect x='5.5' y='5.5' width='8' height='8' rx='1.5'/><path d='M10.5 5.5V3a1.5 1.5 0 00-1.5-1.5H3A1.5 1.5 0 001.5 3v6A1.5 1.5 0 003 10.5h2.5'/></svg></button></div><pre id='logs' class='mono'></pre></div>"

        "</div>"
        "</div>"
        "<script>"
"const T={en:{title:'ESP32 RTSP Mic for BirdNET-Go',status:'Status',ip:'IP Address',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi TX Power',heap:'Free Heap (min)',uptime:'Uptime',rtsp_server:'RTSP Server',client:'Client',streaming:'Streaming',pkt_rate:'Packet Rate',last_connect:'Last RTSP Connect',last_play:'Last Stream Start',audio:'Audio',rate:'Sample Rate',gain:'Gain',buf:'Buffer Size',latency:'Latency',profile:'Profile',perf:'Reliability',auto:'Auto Recovery',wifi:'WiFi',wifi_tx2:'TX Power (dBm)',thermal:'Thermal',logs:'Logs',bsrvon:'Server ON',bsrvoff:'Server OFF',breset:'Reset I2S',breboot:'Reboot',bdefaults:'Defaults',confirm_reboot:'Restart device now?',confirm_reset:'Reset to defaults and reboot?',restarting:'Restarting device…',resetting:'Restoring defaults and rebooting…',advanced_settings:'Advanced Settings',shift:'I2S Shift',thr:'Restart Threshold',chk:'Check Interval',thr_mode:'Threshold Mode',auto_m:'Auto',manual_m:'Manual',sched:'Scheduled Reset',hours:'Reset After',cpu:'CPU Frequency',set:'Set',profile_ultra:'Low latency (62 packets/s, most WiFi airtime)',profile_balanced:'Balanced (31 packets/s)',profile_stable:'Recommended for BirdNET-Go (16 packets/s)',profile_high:'Fewest packets (highest per-packet latency)',help_rate:'Higher sample-rate = more detail, more bandwidth.',help_gain:'Amplifies audio after I²S shift; too high clips.',help_buf:'More samples per packet = higher latency, more stability.',help_auto:'Auto-restarts the pipeline when packet-rate collapses.',help_tx:'Wi‑Fi TX power; lowering can reduce RF noise.',help_shift:'Digital right shift applied before scaling.',help_thr:'Minimum packet-rate before auto-recovery triggers.',help_chk:'How often performance is checked.',help_sched:'Periodic device restart for stability.',help_hours:'Interval between scheduled restarts.',help_cpu:'Lower MHz = cooler, higher latency possible.',adv_buf512:'Buffer below 512 samples sends 30+ packets/s; use 1024 for BirdNET-Go.',adv_buf1024:'Buffer 1024 is the sweet spot for BirdNET-Go.',adv_gain:'Gain above 20x usually clips; prefer AGC.',health:'Stream Health',wifi_ps:'WiFi Power Save',help_health:'Live view of the send path: frames waiting in the software queue (depth/capacity and the session peak), frames dropped because the client could not keep up, I2S DMA overruns (samples lost before capture; should stay 0), and the longest time the WiFi link refused data. Non-zero drops or overruns explain clicks and gaps.',help_wifi_ps:'ON lets the WiFi modem sleep between access-point beacons (WIFI_PS_MIN_MODEM), the biggest thermal lever on the Atom Echo. Caveat: the ESP32 TCP send window is 5760 bytes, so the stream only keeps up if your AP delivers ACKs within ~150 ms (DTIM period 1). With DTIM 2-3 it cannot, and the firmware then suspends power save until reboot after 10 dropped frames (shown in Stream Health). Try it once the stream is stable.',therm_protect:'Overheat Protection',therm_limit:'Shutdown Limit',therm_status:'Status',therm_now:'Current Temp',therm_max:'Peak Temp',therm_cpu:'CPU Clock',therm_last:'Last Shutdown',therm_status_ready:'Protection ready',therm_status_disabled:'Protection disabled',therm_status_latched:'Cooling required – restart manually',therm_status_sensor_fault:'Sensor unavailable – protection paused',therm_status_latched_persist:'Protection latched — acknowledge to re-enable',therm_hint:'80 °C suits most ESP32 boards; drop to 70–75 °C for sealed enclosures.',therm_last_none:'No shutdown recorded yet.',therm_last_fmt:'Stopped at %TEMP% °C (limit %LIMIT% °C) after %TIME% uptime (%AGO%).',therm_last_sensor_fault:'Thermal protection disabled: temperature sensor unavailable.',therm_latch_notice:'Thermal shutdown latched the RTSP server. Confirm only after hardware cools down.',therm_clear_btn:'Acknowledge & re-enable RTSP',therm_time_unknown:'unknown time',therm_time_ago_unknown:'just now',help_therm_protect:'Automatically stops streaming when the ESP32 exceeds the limit to protect the board and microphone preamp.',help_therm_limit:'Temperature threshold for thermal shutdown. 80 °C is a safe default; use 70–75 °C if airflow is poor.'},cs:{title:'ESP32 RTSP Mic pro BirdNET-Go',status:'Stav',ip:'IP adresa',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi výkon',heap:'Volná RAM (min)',uptime:'Doba běhu',rtsp_server:'RTSP server',client:'Klient',streaming:'Streamování',pkt_rate:'Rychlost paketů',last_connect:'Poslední RTSP připojení',last_play:'Poslední start streamu',audio:'Audio',rate:'Vzorkovací frekvence',gain:'Zisk',buf:'Velikost bufferu',latency:'Latence',profile:'Profil',perf:'Spolehlivost',auto:'Automatická obnova',wifi:'WiFi',wifi_tx2:'TX výkon (dBm)',thermal:'Teplota',logs:'Logy',bsrvon:'Server ZAP',bsrvoff:'Server VYP',breset:'Reset I2S',breboot:'Restart',bdefaults:'Výchozí',confirm_reboot:'Restartovat zařízení nyní?',confirm_reset:'Obnovit výchozí nastavení a restartovat?',restarting:'Zařízení se restartuje…',resetting:'Obnovuji výchozí nastavení a restartuji…',advanced_settings:'Pokročilá nastavení',shift:'I2S posun',thr:'Prahová hodnota restartu',chk:'Interval kontroly',thr_mode:'Režim prahu',auto_m:'Automaticky',manual_m:'Manuálně',sched:'Plánovaný restart',hours:'Po kolika hodinách',cpu:'Frekvence CPU',set:'Nastavit',profile_ultra:'Nízká latence (62 paketů/s, nejvíce WiFi provozu)',profile_balanced:'Vyvážené (31 paketů/s)',profile_stable:'Doporučeno pro BirdNET-Go (16 paketů/s)',profile_high:'Nejméně paketů (nejvyšší latence paketu)',help_rate:'Vyšší frekvence = více detailů, větší datový tok.',help_gain:'Zesílení po I²S posunu; příliš vysoké klipuje.',help_buf:'Více vzorků v paketu = vyšší latence, větší stabilita.',help_auto:'Při poklesu rychlosti paketů dojde k obnově.',help_tx:'Výkon vysílače Wi‑Fi; snížení může zlepšit šum.',help_shift:'Digitální bitový posun před škálováním.',help_thr:'Minimální rychlost paketů pro spuštění obnovy.',help_chk:'Jak často se provádí kontrola výkonu.',help_sched:'Pravidelný restart zařízení kvůli stabilitě.',help_hours:'Interval mezi plánovanými restarty.',help_cpu:'Nižší MHz = chladnější, může přidat latenci.',adv_buf512:'Buffer pod 512 vzorků posílá 30+ paketů/s; pro BirdNET-Go použijte 1024.',adv_buf1024:'Buffer 1024 je pro BirdNET-Go ideální.',adv_gain:'Zisk nad 20x obvykle klipuje; raději AGC.',health:'Stav streamu',wifi_ps:'Úspora WiFi',help_health:'Živý pohled na odesílací cestu: snímky čekající v softwarové frontě (hloubka/kapacita a maximum relace), zahozené snímky (klient nestíhal), přetečení I2S DMA (ztracené vzorky před zachycením; má být 0) a nejdelší doba, kdy WiFi odmítala data. Nenulové hodnoty vysvětlují lupání a výpadky.',help_wifi_ps:'ZAP nechá WiFi modem spát mezi beacony přístupového bodu (WIFI_PS_MIN_MODEM), největší úspora tepla na Atom Echo. Pozor: odesílací okno TCP v ESP32 má 5760 bajtů, stream tedy stíhá jen když AP doručí potvrzení do ~150 ms (DTIM perioda 1). Při DTIM 2-3 nestíhá a firmware po 10 zahozených snímcích úsporu do restartu pozastaví (viz Stav streamu). Zkuste až po stabilizaci streamu.',therm_protect:'Ochrana proti přehřátí',therm_limit:'Vypínací teplota',therm_status:'Stav',therm_now:'Aktuální teplota',therm_max:'Maximální teplota',therm_cpu:'Takt CPU',therm_last:'Poslední zásah',therm_status_ready:'Ochrana připravena',therm_status_disabled:'Ochrana vypnuta',therm_status_latched:'Přehřátí – nejprve vychlaďte a spusťte ručně',therm_status_sensor_fault:'Senzor teploty nedostupný – ochrana pozastavena',therm_status_latched_persist:'Ochrana zůstává blokovaná – potvrďte znovuspuštění',therm_hint:'80 °C je bezpečné pro většinu ESP32; v uzavřených krabičkách volte 70–75 °C.',therm_last_none:'Zatím žádné přehřátí.',therm_last_fmt:'Stream vypnut při %TEMP% °C (limit %LIMIT% °C) po %TIME% běhu (%AGO%).',therm_last_sensor_fault:'Tepelná ochrana vypnuta: teplota není k dispozici.',therm_latch_notice:'Tepelná ochrana odstavila RTSP server. Zapínejte až po vychladnutí.',therm_clear_btn:'Potvrdit a znovu povolit RTSP',therm_time_unknown:'neznámý čas',therm_time_ago_unknown:'právě teď',help_therm_protect:'Při překročení limitu zastaví stream, aby chránila desku a předzesilovač.',help_therm_limit:'Teplota, při které se stream vypne. 80 °C vyhoví odkrytým deskám; v teplém prostředí nastavte 70–75 °C.'}};"
        "const HELP_EXT_EN={led:'LED Mode', help_led:'Off: LED stays dark during streaming. Static: Solid color (blue=ready, green=streaming). Level: Color changes with audio level — green=good, orange=hot, red=clipping, dim purple=quiet.',agc:'AGC (Auto Gain)', help_agc:'Automatic Gain Control adjusts volume automatically. Fast attack prevents clipping on loud sounds; slow release gradually boosts quiet periods. Great for outdoor bird recording where distance varies. Base Gain still applies — AGC adjusts on top of it.', hpf:'High-pass', hpf_cut:'HPF Cutoff', help_hpf:'High-pass filter (2nd-order, ~12 dB/oct) removes low-frequency rumble such as distant traffic, wind or handling noise. Turn ON to attenuate frequencies below the cutoff while keeping most bird vocalizations intact.', help_hpf_cut:'Cutoff frequency for the high-pass filter. Typical: 300–800 Hz. Lower values (300–400 Hz) keep more ambience and low calls; higher values (600–800 Hz) strongly reduce road noise. Very high settings may suppress low-pitched species.', help_rate:'Samples per second from the PDM microphone. 16 kHz is right for the Atom Echo and for BirdNET-Go, which resamples to 48 kHz itself; higher rates only add WiFi traffic and memory use (the send queue is capped at 64 KB, so it gets shorter). Changing it restarts the pipeline and disconnects the client.',help_gain:'Software amplification after the I2S shift. Use to boost loudness. Too high causes clipping (distortion). With default shift, 1.0× is neutral. Adjust while watching the stream.',help_buf:'Samples per RTP packet. Stability comes from the 1.5 s send queue regardless of this value; smaller buffers only mean more packets per second (more WiFi airtime), larger ones more per-packet latency. 1024 is right for BirdNET-Go.',help_auto:'When enabled, the device restarts the audio pipeline if packet rate drops below the threshold. Helps recover from glitches without manual intervention.',help_tx:'Wi‑Fi transmit power in dBm. Lower values can reduce RF self-noise near the microphone and power draw, but reduce range. Only specific steps are supported by the radio. Change carefully if your signal is weak.',help_shift:'Right bit-shift applied to 32‑bit I2S samples before converting to 16‑bit. Higher shift lowers volume and avoids clipping; lower shift raises volume but may clip.',help_thr:'Minimum packet rate (packets per second) considered healthy while streaming. If measured rate stays below this at a check, auto recovery restarts I2S. In Auto mode this comes from sample rate and buffer size (about 70% of expected).',help_chk:'How often performance is checked (minutes). Shorter intervals react faster with small CPU cost; longer intervals reduce checks.',help_sched:'Optional periodic device reboot for long-term stability on problematic networks. Leave OFF unless you need it.',help_hours:'Number of hours between scheduled reboots. Applies only when Scheduled Reset is ON.',help_cpu:'Processor clock. 80 MHz is enough for the 16 kHz pipeline and runs coolest; use 160 MHz only if Stream Health shows drops on a busy network. WiFi needs at least 80 MHz.',help_thr_mode:'Auto: Threshold is computed from Sample Rate and Buffer; recommended for most users. Manual: You set the exact minimum packet rate; use if you know your network and latency constraints.', level:'Signal Level', help_level:'Highest peak in the last 3 s, updated whether or not a client is connected. Aim for 30–70% (about −10 to −3 dBFS) so loud calls keep headroom. If it says CLIPPING, reduce Gain or enable AGC; the High‑pass filter (300–600 Hz) removes rumble that eats headroom.', clip_ok:'OK', clip_warn:'High level — close to clipping (reduce Gain or enable AGC).', clip_bad:'CLIPPING! Reduce Gain or enable AGC; try High‑pass 300–600 Hz.'};"
        "const HELP_EXT_CS={led:'Režim LED', help_led:'Vyp: LED je zhasnutá. Statická: Pevná barva (modrá=připraveno, zelená=streamuje). Úroveň: Barva se mění podle hlasitosti — zelená=ok, oranžová=vysoko, červená=přebuzení, tmavě fialová=ticho.',agc:'AGC (Auto zisk)', help_agc:'Automatické řízení zisku přizpůsobuje hlasitost. Rychlý útlum zabrání přebuzení u hlasitých zvuků; pomalé uvolnění postupně zesiluje tiché úseky. Ideální pro venkovní nahrávání ptáků, kde se vzdálenost mění. Základní zisk se stále uplatňuje — AGC upravuje nad ním.', hpf:'Vysokopropustný filtr', hpf_cut:'Mezní frekvence HPF', help_hpf:'Vysokopropustný filtr (2. řád, ~12 dB/okt.) potlačí nízké frekvence jako vzdálená silnice, vítr nebo manipulační hluk. Zapněte pro zeslabení pásem pod mezní frekvencí a zachování většiny ptačích hlasů.', help_hpf_cut:'Mezní frekvence vysokopropustného filtru. Typicky 300–800 Hz. Nižší hodnoty (300–400 Hz) ponechají více atmosféry a nízkých zvuků; vyšší (600–800 Hz) silněji potlačí silniční hluk. Příliš vysoké nastavení může omezit nízko posazené druhy.', help_rate:'Vzorků za sekundu z PDM mikrofonu. 16 kHz je správně pro Atom Echo i BirdNET-Go, který si sám převzorkuje na 48 kHz; vyšší frekvence jen přidají WiFi provoz a paměť (fronta je omezena na 64 KB, takže se zkrátí). Změna restartuje pipeline a odpojí klienta.',help_gain:'Softwarové zesílení po I2S posunu. 1,0× je neutrální s výchozím posunem. Příliš vysoká hodnota způsobí ořez (zkreslení). Upravujte podle poslechu a spektra.',help_buf:'Počet vzorků v jednom RTP paketu. Stabilitu zajišťuje 1,5 s odesílací fronta nezávisle na této hodnotě; menší buffer znamená jen více paketů za sekundu (více WiFi provozu), větší vyšší latenci paketu. Pro BirdNET-Go je správně 1024.',help_auto:'Při poklesu rychlosti odchozích paketů pod práh zařízení automaticky restartuje audio pipeline. Pomáhá zotavit se z výpadků bez zásahu.',help_tx:'Vysílací výkon Wi‑Fi v dBm. Snížení může omezit vlastní RF šum u mikrofonu a spotřebu, ale zmenší dosah. Čip podporuje jen určité kroky. Pokud máte slabý signál, měňte opatrně.',help_shift:'Pravý bitový posun na 32bitových I2S vzorcích před převodem na 16bit audio. Vyšší posun snižuje hlasitost a brání klipování; nižší posun zvyšuje hlasitost, ale může klipovat.',help_thr:'Minimální rychlost paketů (paketů za sekundu), považovaná při streamování za zdravou. Pokud při kontrole klesne pod tuto hodnotu, automatická obnova restartuje I2S. V režimu Auto se práh odvozuje z frekvence a bufferu (asi 70 % očekávané hodnoty).',help_chk:'Jak často se kontroluje výkon (minuty). Kratší interval reaguje rychleji s malou zátěží CPU; delší interval snižuje počet kontrol.',help_sched:'Volitelný pravidelný restart zařízení pro dlouhodobou stabilitu na problematických sítích. Nechte VYP, pokud není nutné.',help_hours:'Počet hodin mezi plánovanými restarty. Platí pouze pokud je Plánovaný restart ZAP.',help_cpu:'Frekvence procesoru. 80 MHz stačí pro 16 kHz pipeline a hřeje nejméně; 160 MHz použijte jen pokud Stav streamu ukazuje zahazování na rušné síti. WiFi vyžaduje minimálně 80 MHz.',help_thr_mode:'Auto: Práh restartu se počítá z Vzorkovací frekvence a Bufferu; doporučeno pro většinu uživatelů. Manuálně: Nastavíte přesný minimální počet paketů za sekundu; použijte, pokud znáte svou síť a požadavky na latenci.', level:'Úroveň signálu', help_level:'Nejvyšší špička za poslední 3 s, měří se i bez připojeného klienta. Cíl je 30–70 % (asi −10 až −3 dBFS), aby hlasitým hlasům zbyla rezerva. Při CLIPPING snižte Gain nebo zapněte AGC; High‑pass (300–600 Hz) odstraní hluk, který rezervu ubírá.', clip_ok:'OK', clip_warn:'Vysoká úroveň — blízko klipu (snižte Gain nebo zapněte AGC).', clip_bad:'CLIPPING! Snižte Gain nebo zapněte AGC; zkuste High‑pass 300–600 Hz.'};"
        "Object.assign(T.en, HELP_EXT_EN); Object.assign(T.cs, HELP_EXT_CS);"
        "let lang=localStorage.getItem('lang')||'en'; const $=id=>document.getElementById(id);"
"function applyLang(){const L=T[lang]; const st=(id,t)=>{const e=$(id); if(e) e.textContent=t}; const help=(k)=>{const b=L[k]||''; return b}; st('t_title',L.title); st('t_status',L.status); st('t_ip',L.ip); st('t_wifi_rssi',L.wifi_rssi); st('t_wifi_tx',L.wifi_tx); st('t_heap',L.heap); st('t_uptime',L.uptime); st('t_rtsp_server',L.rtsp_server); st('t_client',L.client); st('t_streaming',L.streaming); st('t_pkt_rate',L.pkt_rate); st('t_last_connect',L.last_connect); st('t_last_play',L.last_play); st('t_audio',L.audio); st('t_rate',L.rate); st('t_gain',L.gain); st('t_buf',L.buf); st('t_latency',L.latency); st('t_level',L.level); st('t_profile',L.profile); st('t_perf',L.perf); st('t_auto',L.auto); st('t_wifi',L.wifi); st('t_wifi_tx2',L.wifi_tx2); st('t_thermal',L.thermal); st('t_therm_protect',L.therm_protect); st('t_therm_limit',L.therm_limit); st('t_therm_status',L.therm_status); st('t_therm_now',L.therm_now); st('t_therm_max',L.therm_max); st('t_therm_cpu',L.therm_cpu); st('t_therm_last',L.therm_last); st('t_logs',L.logs); st('b_srv_on',L.bsrvon); st('b_srv_off',L.bsrvoff); st('b_reset',L.breset); st('b_reboot',L.breboot); st('b_defaults',L.bdefaults); st('t_advanced_settings',L.advanced_settings); st('t_shift',L.shift); st('t_thr',L.thr); st('t_chk',L.chk); st('t_thr_mode',L.thr_mode); st('t_sched',L.sched); st('t_hours',L.hours); st('t_cpu',L.cpu); st('t_health',L.health); st('t_wifi_ps',L.wifi_ps); const hm=(id,k)=>{const e=$(id); if(e) e.setAttribute('title',help(k))}; hm('h_rate','help_rate'); hm('h_gain','help_gain'); hm('h_hpf','help_hpf'); hm('h_hpf_cut','help_hpf_cut'); hm('h_buf','help_buf'); hm('h_auto','help_auto'); hm('h_tx','help_tx'); hm('h_thr','help_thr'); hm('h_chk','help_chk'); hm('h_shift','help_shift'); hm('h_sched','help_sched'); hm('h_hours','help_hours'); hm('h_cpu','help_cpu'); hm('h_health','help_health'); hm('h_wifi_ps','help_wifi_ps'); hm('h_thr_mode','help_thr_mode'); hm('h_level','help_level'); hm('h_therm_protect','help_therm_protect'); hm('h_therm_limit','help_therm_limit'); st('btn_rate_set',L.set); st('btn_gain_set',L.set); st('btn_buf_set',L.set); st('btn_auto_set',L.set); st('btn_thrmode_set',L.set); st('btn_thr_set',L.set); st('btn_sched_set',L.set); st('btn_hours_set',L.set); st('btn_shift_set',L.set); st('btn_chk_set',L.set); st('btn_tx_set',L.set); st('btn_cpu_set',L.set); st('btn_wifi_ps_set',L.set); st('btn_oh_enable',L.set); st('btn_oh_limit',L.set); const sht=(id,k)=>{const e=$(id); if(e) e.textContent=help(k)}; sht('txt_rate_hint','help_rate'); sht('txt_gain_hint','help_gain'); sht('txt_hpf_hint','help_hpf'); sht('txt_hpf_cut_hint','help_hpf_cut'); sht('txt_buf_hint','help_buf'); sht('txt_auto_hint','help_auto'); sht('txt_thr_hint','help_thr'); sht('txt_thr_mode_hint','help_thr_mode'); sht('txt_sched_hint','help_sched'); sht('txt_hours_hint','help_hours'); sht('txt_shift_hint','help_shift'); sht('txt_chk_hint','help_chk'); sht('txt_tx_hint','help_tx'); sht('txt_cpu_hint','help_cpu'); sht('txt_health_hint','help_health'); sht('txt_wifi_ps_hint','help_wifi_ps'); sht('txt_level_hint','help_level'); sht('txt_therm_hint_protect','help_therm_protect'); sht('txt_therm_hint_limit','help_therm_limit'); st('t_hpf',L.hpf); st('t_hpf_cut',L.hpf_cut); st('t_agc',L.agc); hm('h_agc','help_agc'); st('btn_agc_set',L.set); sht('txt_agc_hint','help_agc'); st('t_led',L.led); hm('h_led','help_led'); st('btn_led_set',L.set); sht('txt_led_hint','help_led'); document.title=L.title;}"
        "function profileText(buf){const L=T[lang]; buf=parseInt(buf,10)||0; if(buf<=256) return L.profile_ultra; if(buf<=512) return L.profile_balanced; if(buf<=1024) return L.profile_stable; return L.profile_high;}"
        "function fmtBool(b){return b?'<span class=ok>YES</span>':'<span class=bad>NO</span>'}"
        "function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}"
        "function showOverlay(msg){ $('ovr_msg').textContent=msg; $('ovr').style.display='flex'; }"
        "function copyLogs(){const t=$('logs').textContent;const b=$('btn_copy_logs');const orig=b.innerHTML;const ta=document.createElement('textarea');ta.value=t;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();try{document.execCommand('copy');b.innerHTML='<svg width=16 height=16 viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"var(--acc2)\" stroke-width=\"1.5\"><path d=\"M3 9l3 3 7-7\"/></svg>';setTimeout(()=>{b.innerHTML=orig},1500)}catch(e){}document.body.removeChild(ta)}"
        "function rebootSequence(kind){ const L=T[lang]; const msg=(kind==='factory_reset')?L.resetting:L.restarting; showOverlay(msg); function tick(){ fetch('/api/status',{cache:'no-store'}).then(r=>{ if(r.ok){ location.reload(); } else { setTimeout(tick,2000); } }).catch(()=>setTimeout(tick,2000)); } setTimeout(tick,4000); }"
        "function act(a){fetch('/api/action/'+a,{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function rebootNow(){ rebootSequence('reboot'); act('reboot'); }"
        "function defaultsNow(){ rebootSequence('factory_reset'); act('factory_reset'); }"
        "const locks={}; const edits={};"
        "function setv(k,v){v=String(v?\?'').trim().replace(',', '.'); if(v==='')return; locks[k]=Date.now()+5000; delete edits[k]; fetch('/api/set?key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v),{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function bindSaver(el,key){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){setv(key,el.value)}})}"
        "function trackEdit(el,key){if(!el)return; const bump=()=>{edits[key]=Date.now()+10000; toggleDirty(el,key)}; el.addEventListener('input',bump); el.addEventListener('change',bump)}"
        "function toggleDirty(el,key){ if(!el)return; const now=Date.now(); const d=(edits[key]&&now<edits[key]); el.classList.toggle('dirty', !!d); if(!d){ delete edits[key]; } }"
        "function setToggleState(on){const onb=$('b_srv_on'), offb=$('b_srv_off'); if(onb&&offb){onb.classList.toggle('active',on); offb.classList.toggle('active',!on); onb.disabled=on; offb.disabled=!on;}}"
        "function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); setToggleState(j.rtsp_server_enabled); $('client').textContent=j.client || 'Waiting...'; $('stream').innerHTML=fmtBool(j.streaming); $('rate').textContent=j.current_rate_pkt_s+' pkt/s'; $('lcon').textContent=j.last_rtsp_connect; $('lplay').textContent=j.last_stream_start; const stx=$('sel_tx'); const now=Date.now(); if(stx){ const editing=(edits['wifi_tx']&&now<edits['wifi_tx']); if(!(locks['wifi_tx']&&now<locks['wifi_tx']) && !editing) stx.value=j.wifi_tx_dbm.toFixed(1); toggleDirty(stx,'wifi_tx'); } const fv=$('fwv'); if(fv && j.fw_version){ fv.textContent='v'+j.fw_version; } const rl=$('rtsp'); if(rl && j.ip){ const u='rtsp://'+j.ip+':8554/audio'; rl.href=u; rl.textContent=u; } const hl=$('health'); if(hl){ if(j.streaming){ const bad=(j.frames_dropped>0||j.i2s_overruns>0); const cls=bad?'warn':'ok'; const ps=j.wifi_ps_suspended?' — WiFi power save suspended':''; hl.innerHTML='<span class='+cls+'>Queue '+j.queue_depth+'/'+j.queue_frames+' (peak '+j.queue_high_water+')</span>, dropped '+j.frames_dropped+', I2S overruns '+j.i2s_overruns+', longest stall '+j.send_stall_max_ms+' ms'+ps; } else if(j.queue_high_water>0||j.frames_dropped>0||j.send_stall_max_ms>0){ hl.textContent='Idle. Last session: dropped '+j.frames_dropped+', I2S overruns '+j.i2s_overruns+', queue peak '+j.queue_high_water+'/'+j.queue_frames+', longest stall '+j.send_stall_max_ms+' ms'+(j.wifi_ps_suspended?' (WiFi power save suspended)':''); } else { hl.textContent='Idle'; } } const ps=$('sel_wifi_ps'); if(ps){ const editing=(edits['wifi_ps']&&now<edits['wifi_ps']); if(!(locks['wifi_ps']&&now<locks['wifi_ps']) && !editing) ps.value=j.wifi_ps?'on':'off'; toggleDirty(ps,'wifi_ps'); } })}"
        "function loadAudio(){fetch('/api/audio_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const r=$('in_rate'); const g=$('in_gain'); const sb=$('sel_buf'); const s=$('in_shift'); const hp=$('sel_hp'); const hpc=$('in_hp_cutoff'); const now=Date.now(); if(r){ const editing=(edits['rate']&&now<edits['rate']); if(!(locks['rate']&&now<locks['rate']) && !editing) r.value=j.sample_rate; toggleDirty(r,'rate'); } if(g){ const editing=(edits['gain']&&now<edits['gain']); if(!(locks['gain']&&now<locks['gain']) && !editing) g.value=j.gain.toFixed(2); toggleDirty(g,'gain'); } if(sb){ const editing=(edits['buffer']&&now<edits['buffer']); if(!(locks['buffer']&&now<locks['buffer']) && !editing) sb.value=j.buffer_size; toggleDirty(sb,'buffer'); } if(s){ const editing=(edits['shift']&&now<edits['shift']); if(!(locks['shift']&&now<locks['shift']) && !editing) s.value=j.i2s_shift; toggleDirty(s,'shift'); } if(hp){ const editing=(edits['hp_enable']&&now<edits['hp_enable']); if(!(locks['hp_enable']&&now<locks['hp_enable']) && !editing) hp.value=j.hp_enable?'on':'off'; toggleDirty(hp,'hp_enable'); } if(hpc){ const editing=(edits['hp_cutoff']&&now<edits['hp_cutoff']); if(!(locks['hp_cutoff']&&now<locks['hp_cutoff']) && !editing) hpc.value=j.hp_cutoff_hz; toggleDirty(hpc,'hp_cutoff'); } const agc=$('sel_agc'); if(agc){ const editing=(edits['agc_enable']&&now<edits['agc_enable']); if(!(locks['agc_enable']&&now<locks['agc_enable']) && !editing) agc.value=j.agc_enable?'on':'off'; toggleDirty(agc,'agc_enable'); } const agi=$('agc_info'); if(agi){ if(j.agc_enable) agi.textContent='x'+j.agc_multiplier.toFixed(1)+' (eff: '+j.effective_gain.toFixed(1)+'x)'; else agi.textContent=''; } const led=$('sel_led'); if(led){ const editing=(edits['led_mode']&&now<edits['led_mode']); if(!(locks['led_mode']&&now<locks['led_mode']) && !editing) led.value=String(j.led_mode||0); toggleDirty(led,'led_mode'); } $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=profileText(j.buffer_size); const L=T[lang]; const lvl=$('level'); if(lvl){ const pct=j.peak_pct||0, db=j.peak_dbfs||-90, clip=j.clip, cc=j.clip_count||0; if(clip){ lvl.innerHTML = `<span class='bad'>${L.clip_bad}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS), clips: ${cc}`; } else if(pct>=90){ lvl.innerHTML = `<span class='warn'>${L.clip_warn}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS)`; } else { lvl.textContent = `Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS) — ${L.clip_ok}`; } } updateAdvice(j); })}"
        "function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024); if(a.gain>20) tips.push(L.adv_gain); $('adv').textContent=tips.join(' ');}"
        "function loadPerf(){fetch('/api/perf_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const el=$('in_auto'); if(el) el.value=j.auto_recovery?'on':'off'; const thr=$('in_thr'); const chk=$('in_chk'); const mode=$('in_thr_mode'); const sch=$('in_sched'); const hrs=$('in_hours'); const now=Date.now(); if(mode){ const editing=(edits['thr_mode']&&now<edits['thr_mode']); if(!(locks['thr_mode']&&now<locks['thr_mode']) && !editing) mode.value=j.auto_threshold?'auto':'manual'; toggleDirty(mode,'thr_mode'); } if(thr){ const editing=(edits['min_rate']&&now<edits['min_rate']); if(!(locks['min_rate']&&now<locks['min_rate']) && !editing) thr.value=j.restart_threshold_pkt_s; toggleDirty(thr,'min_rate'); } if(chk){ const editing=(edits['check_interval']&&now<edits['check_interval']); if(!(locks['check_interval']&&now<locks['check_interval']) && !editing) chk.value=j.check_interval_min; toggleDirty(chk,'check_interval'); } if(sch){ const editing=(edits['sched_reset']&&now<edits['sched_reset']); if(!(locks['sched_reset']&&now<locks['sched_reset']) && !editing) sch.value=j.scheduled_reset?'on':'off'; toggleDirty(sch,'sched_reset'); } if(hrs){ const editing=(edits['reset_hours']&&now<edits['reset_hours']); if(!(locks['reset_hours']&&now<locks['reset_hours']) && !editing) hrs.value=j.reset_hours; toggleDirty(hrs,'reset_hours'); } $('row_min_rate').style.display=j.auto_threshold?'none':''; })}"
"function loadTherm(){fetch('/api/thermal',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const now=Date.now(); const L=T[lang]; const en=$('sel_oh_enable'); if(en){ const editing=(edits['oh_enable']&&now<edits['oh_enable']); if(!(locks['oh_enable']&&now<locks['oh_enable']) && !editing) en.value=j.protection_enabled?'on':'off'; toggleDirty(en,'oh_enable'); } const lim=$('sel_oh_limit'); if(lim){ const editing=(edits['oh_limit']&&now<edits['oh_limit']); if(!(locks['oh_limit']&&now<locks['oh_limit']) && !editing) lim.value=(Number(j.shutdown_c)||80).toFixed(0); toggleDirty(lim,'oh_limit'); } const sc=$('sel_cpu'); if(sc && !(locks['cpu_freq']&&now<locks['cpu_freq'])){ sc.value=j.cpu_mhz; } const currentValid=(j.current_valid&&typeof j.current_c==='number'&&isFinite(j.current_c)); const cur=$('therm_now'); if(cur) cur.textContent=currentValid?j.current_c.toFixed(1)+' °C':'N/A'; const max=$('therm_max'); if(max){ const maxValid=(typeof j.max_c==='number'&&isFinite(j.max_c)); max.textContent=maxValid?j.max_c.toFixed(1)+' °C':'N/A'; } const cpu=$('therm_cpu'); if(cpu) cpu.textContent=j.cpu_mhz+' MHz'; const status=$('therm_status'); if(status){ if(j.sensor_fault){ status.innerHTML='<span class=warn>'+L.therm_status_sensor_fault+'</span>'; } else if(j.latched_persist){ status.innerHTML='<span class=warn>'+L.therm_status_latched_persist+'</span>'; } else if(!j.protection_enabled){ status.innerHTML='<span class=bad>'+L.therm_status_disabled+'</span>'; } else if(j.manual_restart || j.latched){ status.innerHTML='<span class=warn>'+L.therm_status_latched+'</span>'; } else { status.innerHTML='<span class=ok>'+L.therm_status_ready+'</span>'; } } const latchRow=$('row_therm_latch'); const latchMsg=$('txt_therm_latch'); const latchBtn=$('btn_therm_clear'); if(latchRow){ if(j.latched_persist){ latchRow.style.display=''; if(latchMsg) latchMsg.textContent=L.therm_latch_notice; if(latchBtn){ latchBtn.textContent=L.therm_clear_btn; latchBtn.disabled=false; } } else { latchRow.style.display='none'; if(latchBtn){ latchBtn.disabled=true; } } } const last=$('therm_last'); if(last){ if(j.sensor_fault){ last.textContent=L.therm_last_sensor_fault; } else if(j.last_trip_ts && j.last_trip_ts.length){ let msg=L.therm_last_fmt; const temp=(typeof j.last_trip_c==='number'&&isFinite(j.last_trip_c)&&j.last_trip_c>0)?j.last_trip_c.toFixed(1):'0'; const limit=(Number(j.shutdown_c)||0).toFixed(0); const ts=j.last_trip_ts||L.therm_time_unknown; const ago=j.last_trip_since||L.therm_time_ago_unknown; msg=msg.replace('%TEMP%',temp).replace('%LIMIT%',limit).replace('%TIME%',ts).replace('%AGO%',ago); last.textContent=msg; if(j.latched_persist){ last.textContent+=' — '+L.therm_status_latched_persist; } else if(j.manual_restart){ last.textContent+=' — '+L.therm_status_latched; } } else if(j.last_reason && j.last_reason.length){ last.textContent=j.last_reason; } else { last.textContent=L.therm_last_none; } } })}"
"function loadLogs(){fetch('/api/logs',{cache:'no-store'}).then(r=>r.text()).then(t=>{ const lg=$('logs'); lg.textContent=t; lg.scrollTop=lg.scrollHeight; })}"
"function loadAll(){loadStatus();loadAudio();loadPerf();loadTherm();loadLogs()}"
"function clearThermalLatch(){ const btn=$('btn_therm_clear'); if(btn) btn.disabled=true; fetch('/api/thermal/clear',{method:'POST',cache:'no-store'}).then(r=>r.json()).then(j=>{ if(!j.ok){ console.warn('Thermal latch clear rejected'); } loadAll(); }).catch(()=>loadAll());}"
"setInterval(loadAll,3000);"
        "const sel=document.getElementById('langSel'); sel.value=lang; sel.onchange=()=>{lang=sel.value;localStorage.setItem('lang',lang);applyLang()}; applyLang();"
        "bindSaver($('in_rate'),'rate'); bindSaver($('in_gain'),'gain'); bindSaver($('in_shift'),'shift'); bindSaver($('in_thr'),'min_rate'); bindSaver($('in_chk'),'check_interval'); bindSaver($('in_hours'),'reset_hours'); bindSaver($('in_hp_cutoff'),'hp_cutoff');"
        "trackEdit($('in_rate'),'rate'); trackEdit($('in_gain'),'gain'); trackEdit($('in_shift'),'shift'); trackEdit($('in_thr'),'min_rate'); trackEdit($('in_chk'),'check_interval'); trackEdit($('in_hours'),'reset_hours'); trackEdit($('in_hp_cutoff'),'hp_cutoff');"
"trackEdit($('sel_led'),'led_mode'); trackEdit($('in_auto'),'auto_recovery'); trackEdit($('in_thr_mode'),'thr_mode'); trackEdit($('in_sched'),'sched_reset'); trackEdit($('sel_buf'),'buffer'); trackEdit($('sel_tx'),'wifi_tx'); trackEdit($('sel_hp'),'hp_enable'); trackEdit($('sel_agc'),'agc_enable'); trackEdit($('sel_cpu'),'cpu_freq'); trackEdit($('sel_wifi_ps'),'wifi_ps'); trackEdit($('sel_oh_enable'),'oh_enable'); trackEdit($('sel_oh_limit'),'oh_limit');"
        "const H=(hid,rid)=>{const h=$(hid), r=$(rid); if(h&&r){ h.onclick=()=>{ r.style.display = (r.style.display==='none'||!r.style.display)?'block':'none'; }; }};"
"H('h_led','row_led_hint'); H('h_rate','row_rate_hint'); H('h_gain','row_gain_hint'); H('h_hpf','row_hpf_hint'); H('h_hpf_cut','row_hpf_cut_hint'); H('h_agc','row_agc_hint'); H('h_buf','row_buf_hint'); H('h_auto','row_auto_hint'); H('h_thr','row_thr_hint'); H('h_thr_mode','row_thrmode_hint'); H('h_chk','row_chk_hint'); H('h_sched','row_sched_hint'); H('h_hours','row_hours_hint'); H('h_tx','row_tx_hint'); H('h_shift','row_shift_hint'); H('h_cpu','row_cpu_hint'); H('h_health','row_health_hint'); H('h_wifi_ps','row_wifi_ps_hint'); H('h_level','row_level_hint'); H('h_therm_protect','row_therm_hint_protect'); H('h_therm_limit','row_therm_hint_limit');"
        "loadAll();"
        "</script></body></html>";

// HTTP handlery
static void httpIndex() { web.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    String json = "{";
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(ESP.getFreeHeap()/1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap/1024) + ",";
    json += "\"uptime\":\"" + uptimeStr + "\",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled?"true":"false") + ",";
    if (isStreaming) json += "\"client\":\"" + rtspClientIp + "\",";
    else if (rtspClient && rtspClient.connected()) json += "\"client\":\"" + rtspClient.remoteIP().toString() + "\","; else json += "\"client\":\"\",";
    json += "\"streaming\":" + String(isStreaming?"true":"false") + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"queue_depth\":" + String(streamQueueDepth()) + ",";
    json += "\"queue_frames\":" + String(streamQueueFrames()) + ",";
    json += "\"queue_high_water\":" + String(queueHighWater) + ",";
    json += "\"frames_dropped\":" + String(framesDroppedQueueFull) + ",";
    json += "\"i2s_overruns\":" + String(i2sOverruns) + ",";
    json += "\"i2s_read_errors\":" + String(i2sReadErrors) + ",";
    json += "\"send_stall_max_ms\":" + String(sendStallMaxMs) + ",";
    json += "\"send_stalls\":" + String(sendStallCount) + ",";
    json += "\"wifi_ps\":" + String(wifiPowerSave?"true":"false") + ",";
    json += "\"wifi_ps_suspended\":" + String(wifiPsSuspended?"true":"false") + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + ",";
    json += "\"last_rtsp_connect\":\"" + jsonEscape(formatSince(lastRtspClientConnectMs)) + "\",";
    json += "\"last_stream_start\":\"" + jsonEscape(formatSince(lastRtspPlayMs)) + "\"";
    json += "}";
    apiSendJSON(json);
}

static void httpAudioStatus() {
    float latency_ms = (float)currentBufferSize / currentSampleRate * 1000.0f;
    String json = "{";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"gain\":" + String(currentGainFactor,2) + ",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"i2s_shift\":" + String(i2sShiftBits) + ",";
    json += "\"latency_ms\":" + String(latency_ms,1) + ",";
    extern bool highpassEnabled; extern uint16_t highpassCutoffHz;
    json += "\"profile\":\"" + jsonEscape(profileName(currentBufferSize)) + "\",";
    json += "\"hp_enable\":" + String(highpassEnabled?"true":"false") + ",";
    json += "\"hp_cutoff_hz\":" + String((uint32_t)highpassCutoffHz) + ",";
    json += "\"agc_enable\":" + String(agcEnabled?"true":"false") + ",";
    json += "\"agc_multiplier\":" + String(agcMultiplier, 2) + ",";
    float effectiveGain = agcEnabled ? (currentGainFactor * agcMultiplier) : currentGainFactor;
    json += "\"effective_gain\":" + String(effectiveGain, 2) + ",";
    // Metering/clipping
    uint16_t p = (peakHoldAbs16 > 0) ? peakHoldAbs16 : lastPeakAbs16;
    float peak_pct = (p <= 0) ? 0.0f : (100.0f * (float)p / 32767.0f);
    float peak_dbfs = (p <= 0) ? -90.0f : (20.0f * log10f((float)p / 32767.0f));
    json += "\"peak_pct\":" + String(peak_pct,1) + ",";
    json += "\"peak_dbfs\":" + String(peak_dbfs,1) + ",";
    json += "\"clip\":" + String(audioClippedLastBlock?"true":"false") + ",";
    json += "\"clip_count\":" + String(audioClipCount) + ",";
    json += "\"led_mode\":" + String(ledMode);
    json += "}";
    apiSendJSON(json);
}

static void httpPerfStatus() {
    String json = "{";
    json += "\"restart_threshold_pkt_s\":" + String(minAcceptableRate) + ",";
    json += "\"check_interval_min\":" + String(performanceCheckInterval) + ",";
    json += "\"auto_recovery\":" + String(autoRecoveryEnabled?"true":"false") + ",";
    json += "\"auto_threshold\":" + String(autoThresholdEnabled?"true":"false") + ",";
    json += "\"recommended_min_rate\":" + String(computeRecommendedMinRate()) + ",";
    json += "\"scheduled_reset\":" + String(scheduledResetEnabled?"true":"false") + ",";
    json += "\"reset_hours\":" + String(resetIntervalHours) + "}";
    apiSendJSON(json);
}

static void httpThermal() {
    String since = "";
    if (overheatTripTemp > 0.0f && overheatTriggeredAt != 0) {
        since = formatSince(overheatTriggeredAt);
    }
    bool manualRequired = overheatLatched || (!rtspServerEnabled && overheatProtectionEnabled && overheatTripTemp > 0.0f);
    String json = "{";
    if (lastTemperatureValid) {
        json += "\"current_c\":" + String(lastTemperatureC,1) + ",";
    } else {
        json += "\"current_c\":null,";
    }
    json += "\"current_valid\":" + String(lastTemperatureValid?"true":"false") + ",";
    json += "\"max_c\":" + String(maxTemperature,1) + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + ",";
    json += "\"protection_enabled\":" + String(overheatProtectionEnabled?"true":"false") + ",";
    json += "\"shutdown_c\":" + String(overheatShutdownC,0) + ",";
    json += "\"latched\":" + String(overheatLockoutActive?"true":"false") + ",";
    json += "\"latched_persist\":" + String(overheatLatched?"true":"false") + ",";
    json += "\"sensor_fault\":" + String(overheatSensorFault?"true":"false") + ",";
    json += "\"last_trip_c\":" + String(overheatTripTemp,1) + ",";
    json += "\"last_reason\":\"" + jsonEscape(overheatLastReason) + "\",";
    json += "\"last_trip_ts\":\"" + jsonEscape(overheatLastTimestamp) + "\",";
    json += "\"last_trip_since\":\"" + jsonEscape(since) + "\",";
    json += "\"manual_restart\":" + String(manualRequired?"true":"false");
    json += "}";
    apiSendJSON(json);
}

static void httpThermalClear() {
    if (overheatLatched) {
        overheatLatched = false;
        overheatLockoutActive = false;
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        overheatLastReason = String("Thermal latch cleared manually.");
        overheatLastTimestamp = String("");
        if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
        }
        saveAudioSettings();
        webui_pushLog(F("UI action: thermal_latch_clear"));
        apiSendJSON(F("{\"ok\":true}"));
    } else {
        apiSendJSON(F("{\"ok\":false}"));
    }
}

static void httpLogs() {
    // Writers and this reader both run on the loop task, so a snapshot without the lock is safe.
    String out;
    out.reserve(logCount * 80);
    for (size_t i=0;i<logCount;i++){
        size_t idx = (logHead + LOG_CAP - logCount + i) % LOG_CAP;
        out += logBuffer[idx]; out += '\n';
    }
    web.send(200, "text/plain; charset=utf-8", out);
}

static void httpActionServerStart(){
    if (overheatLatched) {
        webui_pushLog(F("Server start blocked: thermal protection latched"));
        apiSendJSON(F("{\"ok\":false,\"error\":\"thermal_latched\"}"));
        return;
    }
    if (!rtspServerEnabled) {
        rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true);
        overheatLockoutActive = false;
    }
    webui_pushLog(F("UI action: server_start"));
    apiSendJSON(F("{\"ok\":true}"));
}
extern bool requestStreamStop(const char* reason);
static void httpActionServerStop(){
    if (isStreaming) {
        requestStreamStop("server_stop");
    }
    rtspServerEnabled=false;
    rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    webui_pushLog(F("UI action: reset_i2s"));
    restartI2S(); apiSendJSON(F("{\"ok\":true}"));
}

static inline bool argToFloat(const String &name, float &out) { if (!web.hasArg("value")) return false; out = web.arg("value").toFloat(); return true; }
static inline bool argToUInt(const String &name, uint32_t &out) { if (!web.hasArg("value")) return false; out = (uint32_t) web.arg("value").toInt(); return true; }
static inline bool argToUShort(const String &name, uint16_t &out) { if (!web.hasArg("value")) return false; out = (uint16_t) web.arg("value").toInt(); return true; }
static inline bool argToUChar(const String &name, uint8_t &out) { if (!web.hasArg("value")) return false; out = (uint8_t) web.arg("value").toInt(); return true; }

static void httpSet() {
    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (val.length()) { webui_pushLog(String("UI set: ")+key+"="+val); }
    if (key == "gain") { float v; if (argToFloat("value", v) && v>=0.1f && v<=100.0f) { currentGainFactor=v; saveAudioSettings(); } }  // picked up live by the capture task
    else if (key == "rate") { uint32_t v; if (argToUInt("value", v) && v>=8000 && v<=48000) { currentSampleRate=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    else if (key == "buffer") { uint16_t v; if (argToUShort("value", v) && v>=256 && v<=8192) { currentBufferSize=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    // i2sShiftBits removed - fixed at 0 for PDM microphones
    else if (key == "wifi_ps") { String v=web.arg("value"); if (v=="on"||v=="off") { wifiPowerSave=(v=="on"); applyWifiPowerSave(true); saveAudioSettings(); } }
    else if (key == "wifi_tx") { float v; if (argToFloat("value", v) && v>=-1.0f && v<=19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm = snapWifiTxDbm(v); applyWifiTxPower(true); saveAudioSettings(); } }
    else if (key == "auto_recovery") { String v=web.arg("value"); if (v=="on"||v=="off") { autoRecoveryEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "thr_mode") { String v=web.arg("value"); if (v=="auto") { autoThresholdEnabled=true; minAcceptableRate = computeRecommendedMinRate(); saveAudioSettings(); } else if (v=="manual") { autoThresholdEnabled=false; saveAudioSettings(); } }
    else if (key == "min_rate") { uint32_t v; if (argToUInt("value", v) && v>=5 && v<=200) { minAcceptableRate=v; saveAudioSettings(); } }
    else if (key == "check_interval") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=60) { performanceCheckInterval=v; saveAudioSettings(); } }
    else if (key == "sched_reset") { String v=web.arg("value"); if (v=="on"||v=="off") { extern bool scheduledResetEnabled; scheduledResetEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "reset_hours") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=168) { extern uint32_t resetIntervalHours; resetIntervalHours=v; saveAudioSettings(); } }
    else if (key == "cpu_freq") { uint32_t v; if (argToUInt("value", v) && (v==80 || v==160 || v==240)) { cpuFrequencyMhz=(uint8_t)v; setCpuFrequencyMhz(cpuFrequencyMhz); saveAudioSettings(); } }  // WiFi needs >= 80; 120 is not a valid PLL setting
    else if (key == "hp_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { extern bool highpassEnabled; highpassEnabled=(v=="on"); extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); } }
    else if (key == "hp_cutoff") { uint32_t v; if (argToUInt("value", v) && v>=10 && v<=10000) { extern uint16_t highpassCutoffHz; highpassCutoffHz=(uint16_t)v; extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); } }
    else if (key == "agc_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { agcEnabled=(v=="on"); if (!agcEnabled) agcMultiplier=1.0f; saveAudioSettings(); } }
    else if (key == "led_mode") { uint32_t v; if (argToUInt("value", v) && v<=2) { ledMode=(uint8_t)v; saveAudioSettings(); } }
    else if (key == "oh_enable") { String v=web.arg("value"); if (v=="on"||v=="off") { overheatProtectionEnabled = (v=="on"); if (!overheatProtectionEnabled) { overheatLockoutActive = false; } saveAudioSettings(); } }
    else if (key == "oh_limit") { uint32_t v; if (argToUInt("value", v) && v>=OH_MIN && v<=OH_MAX) { uint32_t snapped = OH_MIN + ((v - OH_MIN)/OH_STEP)*OH_STEP; overheatShutdownC = (float)snapped; overheatLockoutActive = false; saveAudioSettings(); } }
    apiSendJSON(F("{\"ok\":true}"));
}

void webui_begin() {
    web.on("/", httpIndex);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/thermal/clear", HTTP_POST, httpThermalClear);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", httpActionServerStart);
    web.on("/api/action/server_stop", httpActionServerStop);
    web.on("/api/action/reset_i2s", httpActionResetI2S);
    web.on("/api/action/reboot", [](){ webui_pushLog(F("UI action: reboot")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(false, 600); });
    web.on("/api/action/factory_reset", [](){ webui_pushLog(F("UI action: factory_reset")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(true, 600); });
    web.on("/api/set", httpSet);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
