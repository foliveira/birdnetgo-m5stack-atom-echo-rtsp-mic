#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include "App.h"
#include "JsonOut.h"
#include "PromOut.h"
#include "RestartInfo.h"
#include "WebUI.h"
#include "webui_index.h"   // INDEX_HTML_GZ: web/index.html, gzipped by scripts/embed_webui.py

// Endpoints (POST bodies are form-encoded, replies are JSON unless noted):
//   GET  /               the page (gzip)
//   GET  /api/state      settings ("cfg") and live status ("live"); the page polls it every second
//   POST /api/settings   any subset of the cfg keys; all are validated before any is applied
//   POST /api/action     name=restart_audio|server_on|server_off|clear_thermal|reboot|defaults|forget_wifi
//   POST /api/update     multipart upload of a firmware .bin; the device restarts into it
//   GET  /api/logs?since=N  log lines after line N as text; X-Log-Seq holds the newest line number
//   GET  /metrics        Prometheus text format; counters are totals since boot

static WebServer web(80);

// ---------------------------------------------------------------------------------------------
// Log ring. Lines are numbered from boot so the page fetches only what it has not seen. Fixed
// slots: no heap use while logMux is held and no fragmentation over long uptimes.

static const uint32_t LOG_CAP = 48;
static const size_t LOG_LINE = 192;   // the session summary line is ~170 characters
static char logLines[LOG_CAP][LOG_LINE];
static uint32_t logSeq = 0;           // lines logged since boot; line n is in slot n % LOG_CAP
extern portMUX_TYPE logMux;

void webui_pushLog(const char* line) {
    portENTER_CRITICAL(&logMux);
    strlcpy(logLines[logSeq % LOG_CAP], line, LOG_LINE);
    logSeq++;
    portEXIT_CRITICAL(&logMux);
}

static void httpLogs() {
    // Lines are only added from the loop task, which is running this handler, so the ring cannot
    // change between the length pass and the send pass.
    const uint32_t end = logSeq;
    uint32_t first = web.arg("since").toInt();
    if (first > end) first = 0;                       // the device rebooted since the page's last fetch
    if (end > LOG_CAP && first < end - LOG_CAP) first = end - LOG_CAP;

    size_t total = 0;
    for (uint32_t n = first; n < end; n++) total += strlen(logLines[n % LOG_CAP]) + 1;
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("X-Log-Seq", String(end));
    web.setContentLength(total);
    web.send(200, "text/plain; charset=utf-8", "");
    char chunk[1024];
    size_t used = 0;
    for (uint32_t n = first; n < end; n++) {
        const char* line = logLines[n % LOG_CAP];
        const size_t len = strlen(line);
        if (used + len + 1 > sizeof(chunk)) { web.sendContent(chunk, used); used = 0; }
        memcpy(chunk + used, line, len);
        used += len;
        chunk[used++] = '\n';
    }
    if (used) web.sendContent(chunk, used);
}

// ---------------------------------------------------------------------------------------------
// Replies

static char jsonBuf[2048];

static void sendJson(int code, const JsonOut& j) {
    if (j.overflow()) {   // a bug, not a runtime condition: the state fits with a wide margin
        web.send(500, "application/json", "{\"error\":\"reply too large\"}");
        return;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.setContentLength(j.length());
    web.send(code, "application/json", "");
    web.sendContent(j.c_str(), j.length());
}

static void sendError(int code, const char* msg) {
    JsonOut j(jsonBuf, sizeof(jsonBuf));
    j.open().s("error", msg).close();
    sendJson(code, j);
}

static void sendOk() {
    JsonOut j(jsonBuf, sizeof(jsonBuf));
    j.open().b("ok", true).close();
    sendJson(200, j);
}

static float pct(uint16_t peak) { return peak * 100.0f / 32767.0f; }

static void sendState() {
    JsonOut j(jsonBuf, sizeof(jsonBuf));
    j.open();
    j.s("fw", FW_VERSION_STR);

    j.open("cfg");
    j.u("rate", currentSampleRate).u("buffer", currentBufferSize).f("gain", currentGainFactor, 2);
    j.b("hpf", highpassEnabled).u("hpf_hz", highpassCutoffHz).b("agc", agcEnabled);
    j.f("tx", snapWifiTxDbm(wifiTxPowerDbm), 1).b("ps", wifiPowerSave).u("cpu", cpuFrequencyMhz);
    j.u("led", ledMode).b("oh", overheatProtectionEnabled).u("oh_limit", lroundf(overheatShutdownC));
    j.b("sched", scheduledResetEnabled).u("sched_h", resetIntervalHours).s("name", deviceName);
    j.close();

    const bool streaming = isStreaming;
    const uint32_t rate = appliedSampleRate ? appliedSampleRate : currentSampleRate;
    const uint16_t peak = lastPeakAbs16, hold = peakHoldAbs16;
    j.open("live");
    j.u("uptime", uptimeSeconds()).s("restart_reason", restartReason());
    j.s("ip", WiFi.localIP().toString().c_str()).s("ssid", WiFi.SSID().c_str()).i("rssi", WiFi.RSSI());
    j.s("bssid", WiFi.BSSIDstr().c_str()).u("channel", WiFi.channel());
    j.u("wifi_drops", wifiDrops);
    if (wifiDrops) {
        j.s("wifi_last_drop", WiFi.disconnectReasonName((wifi_err_reason_t)wifiLastDropReason));
        j.u("wifi_last_drop_s", uptimeSeconds() - wifiLastDropAt);
    } else {
        j.null("wifi_last_drop").null("wifi_last_drop_s");
    }
    j.b("ps_suspended", wifiPsSuspended);
    j.u("heap_kb", ESP.getFreeHeap() / 1024).u("heap_min_kb", ESP.getMinFreeHeap() / 1024);
    j.u("cpu_mhz", getCpuFrequencyMhz());
    j.f("temp_c", lastTemperatureValid ? lastTemperatureC : NAN, 1);
    j.f("temp_max_c", maxTemperature > 0.0f ? maxTemperature : NAN, 1);
    j.b("server", rtspServerEnabled).b("streaming", streaming).s("client", currentRtspClient());
    j.u("session_s", streaming ? (millis() - lastRtspPlayMs) / 1000 : 0).s("end_reason", streamEndReason);
    j.u("connects", rtspConnectCount).u("plays", rtspPlayCount);
    j.u("rate", rate).f("frame_ms", currentBufferSize * 1000.0 / rate, 2);
    j.u("sent", audioPacketsSent).u("dropped", framesDroppedQueueFull);
    j.u("overruns", i2sOverruns).u("read_errors", i2sReadErrors);
    j.u("queue", streamQueueDepth()).u("queue_peak", queueHighWater).u("queue_cap", streamQueueFrames());
    j.u("stall_max_ms", sendStallMaxMs).u("stalls", sendStallCount);
    j.f("peak_pct", pct(peak), 1).f("hold_pct", pct(hold > peak ? hold : peak), 1);
    j.u("clips", audioClipCount).f("agc_x", agcMultiplier, 2).b("mic_flat", micFlat);
    j.b("oh_latched", overheatLatched).b("oh_sensor_fault", overheatSensorFault);
    j.s("oh_note", overheatLastReason.c_str());
    j.u("log_seq", logSeq);
    j.close();

    j.close();
    sendJson(200, j);
}

// Prometheus scrape. The buffer is taken from the heap per request (scrapes are seconds apart)
// rather than held permanently, where it would shorten the frame pool.
static void httpMetrics() {
    const size_t cap = 4096;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        web.send(503, "text/plain", "out of memory");
        return;
    }
    PromOut p(buf, cap);
    const char* const info[] = {"version", FW_VERSION_STR, "name", deviceName, nullptr};
    const StreamTotals t = streamTotals();
    const uint16_t peak = lastPeakAbs16, hold = peakHoldAbs16;
    p.info("rtspmic_info", "Firmware version and device name.", info);
    p.gauge("rtspmic_uptime_seconds", "Seconds since boot.", uptimeSeconds());
    p.gauge("rtspmic_wifi_rssi_dbm", "Signal strength of the access point.", WiFi.RSSI());
    p.gauge("rtspmic_wifi_channel", "WiFi channel.", WiFi.channel());
    p.counter("rtspmic_wifi_disconnects_total", "WiFi link losses.", wifiDrops);
    p.gauge("rtspmic_heap_free_bytes", "Free heap.", ESP.getFreeHeap());
    p.gauge("rtspmic_heap_min_free_bytes", "Lowest free heap since boot.", ESP.getMinFreeHeap());
    if (lastTemperatureValid) p.gauge("rtspmic_temperature_celsius", "Chip sensor, uncalibrated.", lastTemperatureC, 1);
    p.gauge("rtspmic_thermal_latched", "1 while thermal protection holds the server off.", overheatLatched);
    p.gauge("rtspmic_rtsp_server_enabled", "1 while the RTSP server accepts clients.", rtspServerEnabled);
    p.gauge("rtspmic_streaming", "1 while a client receives audio.", isStreaming);
    p.counter("rtspmic_rtsp_connections_total", "RTSP connections accepted.", rtspConnectCount);
    p.counter("rtspmic_rtsp_sessions_total", "RTSP sessions started (PLAY).", rtspPlayCount);
    p.gauge("rtspmic_sample_rate_hertz", "Audio sample rate.", appliedSampleRate);
    p.counter("rtspmic_frames_sent_total", "RTP frames sent.", t.sent);
    p.counter("rtspmic_frames_dropped_total", "RTP frames dropped because the network fell behind.", t.dropped);
    p.counter("rtspmic_i2s_overruns_total", "DMA buffers lost because capture fell behind.", t.overruns);
    p.counter("rtspmic_i2s_read_errors_total", "Failed or short I2S reads.", t.readErrors);
    p.counter("rtspmic_send_stalls_total", "Times the socket accepted no data.", t.stalls);
    p.gauge("rtspmic_send_stall_max_seconds", "Longest send stall in the current session.", sendStallMaxMs / 1000.0, 3);
    p.gauge("rtspmic_send_queue_frames", "Frames waiting to be sent.", streamQueueDepth());
    p.gauge("rtspmic_send_queue_capacity_frames", "Frames the send queue holds.", streamQueueFrames());
    p.gauge("rtspmic_audio_peak_ratio", "Loudest peak of the last 3 s after gain, 1 = full scale.",
            (hold > peak ? hold : peak) / 32767.0, 3);
    p.counter("rtspmic_audio_clipped_blocks_total", "Audio blocks that clipped.", audioClipCount);
    p.gauge("rtspmic_agc_multiplier", "Automatic gain control factor (1 when off).", agcMultiplier, 2);
    p.gauge("rtspmic_microphone_flat", "1 while the microphone delivers constant samples.", micFlat);

    if (p.overflow()) {
        web.send(500, "text/plain", "metrics buffer too small");
    } else {
        web.sendHeader("Cache-Control", "no-store");
        web.setContentLength(p.length());
        web.send(200, "text/plain; version=0.0.4; charset=utf-8", "");
        web.sendContent(p.c_str(), p.length());
    }
    free(buf);
}

// Browsers attach Origin to cross-site POSTs. Refusing those keeps other web pages open in the
// same browser from changing settings, uploading firmware or rebooting the device; curl and
// scripts send no Origin.
static bool crossSite() {
    const String origin = web.header("Origin");
    return origin.length() != 0 && origin != "http://" + web.hostHeader();
}

static bool fromThisPage() {
    if (!crossSite()) return true;
    sendError(403, "cross-site request refused");
    return false;
}

// ---------------------------------------------------------------------------------------------
// Settings

struct Settings {
    uint32_t rate;
    uint16_t buffer;
    float gain;
    bool hpf;
    uint16_t hpfHz;
    bool agc;
    float tx;
    bool ps;
    uint8_t cpu;
    uint8_t led;
    bool oh;
    uint8_t ohLimit;
    bool sched;
    uint16_t schedH;
    char name[33];
};

static Settings currentSettings() {
    Settings s;
    s.rate = currentSampleRate;
    s.buffer = currentBufferSize;
    s.gain = currentGainFactor;
    s.hpf = highpassEnabled;
    s.hpfHz = highpassCutoffHz;
    s.agc = agcEnabled;
    s.tx = snapWifiTxDbm(wifiTxPowerDbm);
    s.ps = wifiPowerSave;
    s.cpu = cpuFrequencyMhz;
    s.led = ledMode;
    s.oh = overheatProtectionEnabled;
    s.ohLimit = (uint8_t)lroundf(overheatShutdownC);
    s.sched = scheduledResetEnabled;
    s.schedH = (uint16_t)resetIntervalHours;
    strlcpy(s.name, deviceName, sizeof(s.name));
    return s;
}

static bool parseBool(const String& v, bool& out) {
    if (v == "1" || v == "on" || v == "true") { out = true; return true; }
    if (v == "0" || v == "off" || v == "false") { out = false; return true; }
    return false;
}

// A number within [lo, hi]; integral if asked. Rejects empty and trailing text ("12abc").
static bool parseNum(const String& v, double lo, double hi, bool integral, double& out) {
    if (v.length() == 0) return false;
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    if (*end != '\0' || !isfinite(d) || d < lo || d > hi) return false;
    if (integral && d != floor(d)) return false;
    out = d;
    return true;
}

// Parses one key into s. Returns an error message, or nullptr.
static const char* parseSetting(const String& key, const String& v, Settings& s) {
    double d;
    if (key == "rate") {
        if (!parseNum(v, 16000, 48000, true, d) || !isSupportedSampleRate((uint32_t)d))
            return "must be 16000, 22050, 24000, 32000, 44100 or 48000";
        s.rate = (uint32_t)d;
    } else if (key == "buffer") {
        if (!parseNum(v, 256, 8192, true, d)) return "must be 256 to 8192 samples";
        s.buffer = (uint16_t)d;
    } else if (key == "gain") {
        if (!parseNum(v, 0.1, 100, false, d)) return "must be 0.1 to 100";
        s.gain = (float)d;
    } else if (key == "hpf_hz") {
        if (!parseNum(v, 10, 10000, true, d)) return "must be 10 to 10000 Hz";
        s.hpfHz = (uint16_t)d;
    } else if (key == "tx") {
        if (!parseNum(v, -1, 19.5, false, d)) return "must be -1 to 19.5 dBm";
        s.tx = snapWifiTxDbm((float)d);
    } else if (key == "cpu") {
        if (!parseNum(v, 80, 240, true, d) || (d != 80 && d != 160 && d != 240)) return "must be 80, 160 or 240";
        s.cpu = (uint8_t)d;
    } else if (key == "led") {
        if (!parseNum(v, 0, 2, true, d)) return "must be 0, 1 or 2";
        s.led = (uint8_t)d;
    } else if (key == "oh_limit") {
        if (!parseNum(v, OVERHEAT_MIN_LIMIT_C, OVERHEAT_MAX_LIMIT_C, true, d)) return "must be 30 to 95";
        s.ohLimit = (uint8_t)(OVERHEAT_MIN_LIMIT_C +
                              ((uint32_t)d - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C * OVERHEAT_LIMIT_STEP_C);
    } else if (key == "sched_h") {
        if (!parseNum(v, 1, 168, true, d)) return "must be 1 to 168 hours";
        s.schedH = (uint16_t)d;
    } else if (key == "name") {
        String n = v;
        n.toLowerCase();
        if (!isValidDeviceName(n.c_str())) return "must be 1 to 32 letters, digits or hyphens, with no hyphen at either end";
        strlcpy(s.name, n.c_str(), sizeof(s.name));
    } else {
        bool* flag = key == "hpf" ? &s.hpf : key == "agc" ? &s.agc : key == "ps" ? &s.ps :
                     key == "oh" ? &s.oh : key == "sched" ? &s.sched : nullptr;
        if (!flag) return "unknown setting";
        if (!parseBool(v, *flag)) return "must be 1 or 0";
    }
    return nullptr;
}

static void httpSettings() {
    if (!fromThisPage()) return;
    const Settings was = currentSettings();
    Settings s = was;
    String changes;
    for (int i = 0; i < web.args(); i++) {
        const String key = web.argName(i);
        const char* err = parseSetting(key, web.arg(i), s);
        if (err) {
            sendError(400, (key + ": " + err).c_str());
            return;
        }
        changes += (changes.length() ? ", " : "") + key + "=" + web.arg(i);
    }
    if (changes.length() == 0) { sendState(); return; }
    simplePrintln("Web UI settings: " + changes);

    // Live: the capture task reads these every block.
    currentGainFactor = s.gain;
    ledMode = s.led;
    if (s.agc != was.agc) {
        agcEnabled = s.agc;
        if (!s.agc) agcMultiplier = 1.0f;
    }
    if (s.hpf != was.hpf || s.hpfHz != was.hpfHz) {
        highpassEnabled = s.hpf;
        highpassCutoffHz = s.hpfHz;
        updateHighpassCoeffs();
    }
    if (s.tx != was.tx) {
        wifiTxPowerDbm = s.tx;
        applyWifiTxPower(true);
    }
    if (s.ps != was.ps) {
        wifiPowerSave = s.ps;
        applyWifiPowerSave(true);
    }
    if (s.cpu != was.cpu) {
        cpuFrequencyMhz = s.cpu;
        setCpuFrequencyMhz(s.cpu);
    }
    if (strcmp(s.name, was.name) != 0) setDeviceName(s.name);
    overheatProtectionEnabled = s.oh;
    overheatShutdownC = s.ohLimit;
    scheduledResetEnabled = s.sched;
    resetIntervalHours = s.schedH;
    const bool reconfigure = s.rate != was.rate || s.buffer != was.buffer;
    currentSampleRate = s.rate;
    currentBufferSize = s.buffer;
    saveAudioSettings();

    if (reconfigure) {
        restartI2S();   // ends the session; on failure it puts the previous rate and size back
        if (currentSampleRate != s.rate || currentBufferSize != s.buffer) {
            sendError(500, "the audio pipeline did not accept the new sample rate or frame size; "
                           "the previous values are back (see the log)");
            return;
        }
    }
    sendState();
}

// ---------------------------------------------------------------------------------------------
// Actions

static void httpAction() {
    if (!fromThisPage()) return;
    const String name = web.arg("name");
    if (name == "restart_audio") {
        simplePrintln("Web UI: restart audio");
        restartI2S();
    } else if (name == "server_on") {
        if (!setRtspServer(true)) {
            sendError(409, "thermal protection is latched; use Resume streaming");
            return;
        }
        simplePrintln("Web UI: RTSP server started");
    } else if (name == "server_off") {
        simplePrintln("Web UI: RTSP server stopped");
        setRtspServer(false);
    } else if (name == "clear_thermal") {
        simplePrintln("Web UI: thermal latch cleared");
        clearThermalLatch();
    } else if (name == "reboot") {
        simplePrintln("Web UI: reboot");
        scheduleReboot(RebootKind::Plain, 500, "Web UI reboot");
    } else if (name == "defaults") {
        simplePrintln("Web UI: restore defaults and reboot");
        scheduleReboot(RebootKind::RestoreDefaults, 500, "defaults restored from the Web UI");
    } else if (name == "forget_wifi") {
        simplePrintln("Web UI: forget the WiFi network and restart into the setup portal");
        scheduleReboot(RebootKind::ForgetWifi, 500, "WiFi network forgotten from the Web UI");
    } else {
        sendError(400, "unknown action");
        return;
    }
    sendOk();
}

// ---------------------------------------------------------------------------------------------
// Firmware update. WebServer calls the upload handler for every chunk of the multipart body, then
// the request handler to reply. Update writes to the other OTA slot and switches the boot slot
// only after esp_ota_set_boot_partition() has verified the whole image (checksum and SHA-256),
// so a truncated or foreign file leaves the running firmware in place. The new firmware then
// runs on probation until confirmFirmware() in the sketch accepts it.

static String updateError;
static bool updateStarted = false;

static void httpUpdateUpload() {
    HTTPUpload& up = web.upload();
    if (up.status == UPLOAD_FILE_START) {
        updateError = "";
        updateStarted = false;
        if (crossSite()) {
            updateError = "cross-site request refused";
            return;
        }
        simplePrintln("Firmware update: receiving " + up.filename);
        requestStreamStop("firmware update");   // flash writes stall the capture task; end the session cleanly
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            updateError = Update.errorString();
            return;
        }
        updateStarted = true;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (updateStarted && updateError.length() == 0 && Update.write(up.buf, up.currentSize) != up.currentSize) {
            updateError = Update.errorString();
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (updateStarted && updateError.length() == 0 && !Update.end(true)) updateError = Update.errorString();
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        updateError = "upload interrupted";
    }
}

static void httpUpdateDone() {
    if (!fromThisPage()) return;
    if (!updateStarted && updateError.length() == 0) updateError = "no firmware file in the request";
    if (updateError.length()) {
        Update.abort();   // frees the buffer; a half-written slot is never made bootable
        simplePrintln("Firmware update failed: " + updateError);
        sendError(400, updateError.c_str());
        return;
    }
    simplePrintln("Firmware update written; restarting into it");
    noteFirmwareUpdateInstalled();
    sendOk();
    scheduleReboot(RebootKind::Plain, 1000, "firmware update");
}

// ---------------------------------------------------------------------------------------------

static void httpIndex() {
    web.sendHeader("Content-Encoding", "gzip");
    web.sendHeader("Cache-Control", "no-cache");
    web.send_P(200, "text/html; charset=utf-8", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void webui_begin() {
    static const char* headers[] = {"Origin"};
    web.collectHeaders(headers, 1);
    web.on("/", HTTP_GET, httpIndex);
    web.on("/api/state", HTTP_GET, sendState);
    web.on("/api/settings", HTTP_POST, httpSettings);
    web.on("/api/action", HTTP_POST, httpAction);
    web.on("/api/logs", HTTP_GET, httpLogs);
    web.on("/metrics", HTTP_GET, httpMetrics);
    web.on("/api/update", HTTP_POST, httpUpdateDone, httpUpdateUpload);
    web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
