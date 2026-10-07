#include "WifiSetup.h"

#include "RestartInfo.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

static bool wifiStarted() {
    return WiFi.getMode() != WIFI_OFF || WiFi.mode(WIFI_STA);
}

bool wifiHasStoredCredentials() {
    wifi_config_t conf;
    return wifiStarted() && esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
}

void wifiForgetCredentials() {
    if (wifiStarted()) WiFi.disconnect(false, true);   // eraseap: writes an empty config to NVS
}

bool wifiJoinStored(const char* hostname, uint32_t timeoutMs) {
    WiFi.setHostname(hostname);   // DHCP name; must precede the STA start
    WiFi.mode(WIFI_STA);
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == 0) return false;
    // The IDF default (and WiFiManager's) is a fast scan that joins the first matching access
    // point found. Scan every channel and take the strongest instead; written to NVS only once.
    if (conf.sta.scan_method != WIFI_ALL_CHANNEL_SCAN || conf.sta.sort_method != WIFI_CONNECT_AP_BY_SIGNAL) {
        conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        esp_wifi_set_config(WIFI_IF_STA, &conf);
    }
    Serial.printf("[WiFi] joining \"%s\"\n", (const char*)conf.sta.ssid);
    WiFi.begin();
    const uint32_t start = millis();
    uint32_t lastKick = start;
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= timeoutMs) return false;
        // The core retries most failures itself but gives up on some (an AUTH_FAIL while the
        // router is still booting, for one); nudge it now and then.
        if (millis() - lastKick >= 15000) {
            lastKick = millis();
            WiFi.reconnect();
        }
        delay(100);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Setup portal

struct ScannedNet {
    char ssid[33];
    int8_t rssi;
    bool open;
};
static ScannedNet nets[20];
static int netCount = 0;

static void scanNetworks() {
    netCount = 0;
    const int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || ssid.length() > 32) continue;   // hidden network
        int found = -1;
        for (int k = 0; k < netCount; k++) {
            if (strcmp(nets[k].ssid, ssid.c_str()) == 0) { found = k; break; }
        }
        const int8_t rssi = (int8_t)WiFi.RSSI(i);
        if (found >= 0) {   // same network from another access point: keep the strongest
            if (rssi > nets[found].rssi) nets[found].rssi = rssi;
            continue;
        }
        if (netCount == (int)(sizeof(nets) / sizeof(nets[0]))) continue;
        ScannedNet& e = nets[netCount++];
        strlcpy(e.ssid, ssid.c_str(), sizeof(e.ssid));
        e.rssi = rssi;
        e.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    for (int i = 1; i < netCount; i++) {   // strongest first
        for (int k = i; k > 0 && nets[k].rssi > nets[k - 1].rssi; k--) {
            ScannedNet t = nets[k]; nets[k] = nets[k - 1]; nets[k - 1] = t;
        }
    }
}

static void appendEscaped(String& out, const char* s) {
    for (; *s; s++) {
        switch (*s) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += *s;
        }
    }
}

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>WiFi setup</title><style>"
    "body{font:16px/1.4 system-ui,sans-serif;margin:0;padding:16px;background:#f4f6f8;color:#1c2430}"
    "main{max-width:440px;margin:auto}h1{font-size:20px}.muted{color:#5f6b7a;font-size:14px}.err{color:#cf222e}"
    ".n{display:flex;justify-content:space-between;width:100%;padding:10px 12px;margin:4px 0;border:1px solid #ccd3db;"
    "border-radius:6px;background:#fff;font:inherit;text-align:left;cursor:pointer}"
    "label{display:block;margin:14px 0 4px;font-weight:600}"
    "input{width:100%;box-sizing:border-box;padding:10px;font:inherit;border:1px solid #ccd3db;border-radius:6px}"
    "button[type=submit]{margin-top:16px;width:100%;padding:12px;font:inherit;font-weight:600;color:#fff;"
    "background:#1f6feb;border:0;border-radius:6px}</style></head><body><main>";

[[noreturn]] void wifiRunSetupPortal(const char* apName, const char* hostname) {
    // Without stored credentials there is nothing to retry, so the portal waits indefinitely.
    // With them (the join failed, e.g. the router was down), restart after 3 idle minutes.
    const uint32_t idleTimeoutMs = wifiHasStoredCredentials() ? 180000 : 0;

    // Scan before the access point starts: in AP+STA mode a scan hops channels under the phone.
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    scanNetworks();
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName);
    const IPAddress ip = WiFi.softAPIP();
    const String home = "http://" + ip.toString() + "/";
    Serial.printf("[Setup] join the open network \"%s\" and open %s (%d networks found)\n",
                  apName, home.c_str(), netCount);

    DNSServer dns;
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", ip);   // every name resolves to the portal: phones then show the page
    WebServer server(80);

    uint32_t lastActivity = millis();
    uint32_t lastRetry = millis();
    String error;
    char pendSsid[33] = "", pendPass[65] = "";
    uint32_t pendingAt = 0;

    auto page = [&]() {
        String h;
        h.reserve(3072);
        h += PAGE_HEAD;
        h += "<h1>Atom Echo RTSP Mic: WiFi setup</h1>";
        if (error.length()) { h += "<p class=err>"; appendEscaped(h, error.c_str()); h += "</p>"; }
        h += "<p class=muted>Choose your 2.4 GHz network, or type its name if it is hidden.</p>";
        for (int i = 0; i < netCount; i++) {
            h += "<button type=button class=n data-s=\"";
            appendEscaped(h, nets[i].ssid);
            h += "\"><span>";
            appendEscaped(h, nets[i].ssid);
            h += "</span><span class=muted>";
            h += String(nets[i].rssi);
            h += nets[i].open ? " dBm, open" : " dBm";
            h += "</span></button>";
        }
        if (netCount == 0) h += "<p class=muted>No networks found.</p>";
        h += "<p><a href=/rescan>Scan again</a></p>"
             "<form method=post action=/save><label for=s>Network name (SSID)</label>"
             "<input id=s name=ssid maxlength=32 required autocapitalize=none autocorrect=off>"
             "<label for=p>Password</label><input id=p name=pass type=password maxlength=64>"
             "<label style='font-weight:400'><input type=checkbox style='width:auto' "
             "onclick=\"p.type=this.checked?'text':'password'\"> Show password</label>"
             "<button type=submit>Connect</button></form><p class=muted>Device MAC address: ";
        h += WiFi.macAddress();
        h += "</p></main><script>document.querySelectorAll('.n').forEach(function(b){b.onclick="
             "function(){s.value=b.dataset.s;p.focus()}})</script></body></html>";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/html; charset=utf-8", h);
    };

    server.on("/", HTTP_GET, [&]() { lastActivity = millis(); page(); });
    server.on("/rescan", HTTP_GET, [&]() {
        lastActivity = millis();
        scanNetworks();
        server.sendHeader("Location", home, true);
        server.send(302, "text/plain", "");
    });
    server.on("/save", HTTP_POST, [&]() {
        lastActivity = millis();
        const String ssid = server.arg("ssid");
        const String pass = server.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32) {
            error = "Enter a network name of 1 to 32 characters.";
        } else if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 64)) {
            error = "WiFi passwords are 8 to 63 characters long.";
        } else {
            strlcpy(pendSsid, ssid.c_str(), sizeof(pendSsid));
            strlcpy(pendPass, pass.c_str(), sizeof(pendPass));
            pendingAt = millis();
            String h;
            h += PAGE_HEAD;
            h += "<h1>Connecting to ";
            appendEscaped(h, pendSsid);
            h += "</h1><p>If it works the device restarts and joins that network. Then open <b>http://";
            appendEscaped(h, hostname);
            h += ".local/</b> from a computer on it, or find the device's address in your router.</p>"
                 "<p class=muted>If it does not work, the LED stays cyan: reconnect to <b>";
            appendEscaped(h, apName);
            h += "</b> and try again.</p></main></body></html>";
            server.send(200, "text/html; charset=utf-8", h);
            return;
        }
        page();
    });
    server.onNotFound([&]() {   // captive-portal probes (generate_204, hotspot-detect, ...) land here
        lastActivity = millis();
        server.sendHeader("Location", home, true);
        server.send(302, "text/plain", "");
    });
    server.begin();

    for (;;) {
        dns.processNextRequest();
        server.handleClient();
        if (WiFi.softAPgetStationNum() > 0) lastActivity = millis();   // someone is on the setup network

        // Join outside the request handler so the reply above reaches the phone first.
        if (pendingAt && millis() - pendingAt > 500) {
            pendingAt = 0;
            Serial.printf("[Setup] joining \"%s\"\n", pendSsid);
            WiFi.begin(pendSsid, pendPass);   // stores the credentials in NVS
            const uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
                dns.processNextRequest();
                server.handleClient();
                delay(10);
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[Setup] joined, address %s; restarting\n", WiFi.localIP().toString().c_str());
                delay(500);
                restartWithNote("joined a network from the setup portal");
            }
            Serial.println("[Setup] join failed");
            error = String("Could not join \"") + pendSsid + "\". Check the password and that it is a 2.4 GHz network.";
            WiFi.disconnect();   // stop the core's retries; the access point stays up
            lastActivity = millis();
        }

        // With a stored network, keep trying it while nobody uses the portal: after a power cut
        // the router often boots slower than the device, and this joins as soon as it is up
        // instead of waiting for the idle restart.
        if (idleTimeoutMs && !pendingAt && WiFi.softAPgetStationNum() == 0 && millis() - lastRetry > 30000) {
            Serial.println("[Setup] retrying the stored network");
            WiFi.begin();   // stored credentials
            const uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000 && WiFi.softAPgetStationNum() == 0) {
                dns.processNextRequest();
                server.handleClient();
                delay(10);
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("[Setup] stored network reachable again; restarting");
                restartWithNote("setup portal: stored network is back");
            }
            WiFi.disconnect();   // stop the retries so the access point keeps a steady channel
            lastRetry = millis();
        }

        if (idleTimeoutMs && millis() - lastActivity > idleTimeoutMs) {
            Serial.println("[Setup] idle, restarting to retry the stored network");
            restartWithNote("setup portal idle for 3 minutes");
        }
        delay(5);
    }
}
