#include "web.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "secrets.h"
#include "blocklist.h"
#include "clients.h"
#include "dns.h"
#include "wifi.h"

WebServer web(80);

// remote blocklist auto-update
static String updateUrl = "";         // prebuilt blocklist.bin (e.g. GitHub release asset)
static uint32_t updateIntervalH = 24; // hours between auto-fetches
static uint32_t lastCheckMs = 0;
static String updateStatus = "never";

static String macStr(const uint8_t *m) {
    char s[18];
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(s);
}
static String jesc(const String &s) {
    String o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\')
            o += '\\';
        o += ch;
    }
    return o;
}

#include "page.h" // dashboard HTML (PROGMEM)

static void handleStats() {
    uint32_t up = millis() / 1000;
    char ut[24];
    snprintf(ut, sizeof(ut), "%lud %luh %lum", up / 86400, (up % 86400) / 3600, (up % 3600) / 60);
    String j = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"blocked\":" + dns.totalBlocked + ",\"allowed\":" + dns.totalAllowed +
               ",\"domains\":" + bl.numHashes + ",\"rssi\":" + WiFi.RSSI() + ",\"temp\":" + String(temperatureRead(), 1) +
               ",\"heap\":" + ESP.getFreeHeap() + ",\"uptime\":\"" + ut + "\"" +
               ",\"upurl\":\"" + jesc(updateUrl) + "\",\"upiv\":" + updateIntervalH + ",\"upstat\":\"" + jesc(updateStatus) + "\"" +
               ",\"blocking\":" + (dns.blockingOn ? "true" : "false") +
               ",\"resumeIn\":" + (uint32_t)(!dns.blockingOn && dns.resumeAt ? (dns.resumeAt - millis()) / 1000 : 0) +
               ",\"defcreds\":" + ((strcmp(WEB_PASS, "CHANGE_ME_WEB_PASSWORD") == 0 || strcmp(OTA_PASS, "CHANGE_ME_OTA_PASSWORD") == 0) ? "true" : "false") +
               ",\"clients\":[";
    for (int i = 0; i < clients.count; i++) {
        Dev &c = clients.list[i];
        IPAddress ip(c.ip);
        j += (i ? "," : "");
        j += "{\"ip\":\"" + ip.toString() + "\",\"mac\":\"" + macStr(c.mac) + "\",\"blocked\":" + c.blocked + ",\"allowed\":" + c.allowed + ",\"banned\":" + (c.banned ? "true" : "false") + "}";
    }
    j += "],\"custom\":[";
    for (int i = 0; i < bl.numCustom; i++) {
        j += (i ? "," : "");
        j += "\"" + jesc(bl.customDom[i]) + "\"";
    }
    j += "]}";
    web.send(200, "application/json", j);
}

// state-changing GET endpoints need a custom header: a plain <img>/<form> CSRF
// vector can't set it, only the dashboard's own same-origin fetch can
static const char *CSRF_HEADER = "X-Requested-With";
static const char *CSRF_VALUE = "esp-adblock";
static bool requireAuth() {
    if (web.header(CSRF_HEADER) != CSRF_VALUE) {
        web.send(403, "text/plain", "missing CSRF header");
        return false;
    }
    if (web.authenticate(WEB_USER, WEB_PASS))
        return true;
    web.requestAuthentication();
    return false;
}

static void handleBan() {
    if (!requireAuth())
        return;
    IPAddress ip;
    if (ip.fromString(web.arg("ip"))) {
        Dev *c = clients.get((uint32_t)ip);
        if (c) {
            c->banned = !c->banned;
            clients.saveBanned();
        }
    }
    web.send(200, "text/plain", "ok");
}

// ---------- blocklist upload (browser) ----------
static bool upOk = false;
static bool upAuthOk = false;
static void handleUploadDone() {
    if (!upAuthOk) {
        web.requestAuthentication();
        return;
    }
    web.send(upOk ? 200 : 500, "text/plain",
             upOk ? "ok" : "rejected: not a valid v1/v2 blocklist (magic/version/CRC)");
}
static void handleUpload() {
    HTTPUpload &u = web.upload();
    switch (u.status) {
    case UPLOAD_FILE_START:
        upAuthOk = web.header(CSRF_HEADER) == CSRF_VALUE && web.authenticate(WEB_USER, WEB_PASS);
        if (!upAuthOk) {
            Serial.println("[ota] blocklist upload: auth/CSRF check failed");
            break;
        }
        upOk = false;
        bl.beginSwap(); // erase partition, start streamed write
        Serial.printf("[ota] receiving %s\n", u.filename.c_str());
        break;
    case UPLOAD_FILE_WRITE:
        if (upAuthOk)
            bl.writeChunk(u.buf, u.currentSize);
        break;
    case UPLOAD_FILE_END:
        if (!upAuthOk)
            break;
        upOk = bl.commitNew();
        Serial.printf("[ota] %s -> %u domains\n", upOk ? "OK" : "REJECTED", bl.numHashes);
        break;
    case UPLOAD_FILE_ABORTED:
        if (!upAuthOk)
            break;
        bl.reopen();
        Serial.println("[ota] aborted");
        break;
    }
}

// ---------- remote blocklist auto-update ----------
static void loadUpdateCfg() {
    File f = LittleFS.open("/update.cfg", "r");
    if (!f)
        return;
    updateUrl = f.readStringUntil('\n');
    updateUrl.trim();
    String iv = f.readStringUntil('\n');
    iv.trim();
    if (iv.length())
        updateIntervalH = iv.toInt();
    f.close();
    if (updateIntervalH < 1)
        updateIntervalH = 1;
}
static void saveUpdateCfg() {
    File f = LittleFS.open("/update.cfg", "w");
    if (!f)
        return;
    f.println(updateUrl);
    f.println(updateIntervalH);
    f.close();
}
static bool fetchBlocklist(String url) {
    url.trim();
    if (!url.length()) {
        updateStatus = "no url set";
        return false;
    }
    Serial.printf("[remote] GET %s\n", url.c_str());
    WiFiClientSecure cs;
    cs.setInsecure(); // blocklist isn't secret -> skip cert pinning
    WiFiClient cl;
    HTTPClient http;
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub release -> CDN redirect
    bool https = url.startsWith("https");
    if (!(https ? http.begin(cs, url) : http.begin(cl, url))) {
        updateStatus = "begin failed";
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        updateStatus = "HTTP " + String(code);
        Serial.printf("[remote] %s\n", updateStatus.c_str());
        return false;
    }
    bl.beginSwap(); // erase partition, start streamed write
    WiFiClient *stream = http.getStreamPtr();
    int len = http.getSize();
    uint8_t b[1024];
    size_t total = 0;
    uint32_t idle = millis();
    while (http.connected() && (len < 0 || (int)total < len)) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->readBytes(b, avail > sizeof(b) ? sizeof(b) : avail);
            if (n > 0) {
                bl.writeChunk(b, n);
                total += n;
                idle = millis();
            }
        } else {
            if (millis() - idle > 15000)
                break;
            delay(2);
        }
    }
    http.end();
    bool ok = bl.commitNew();
    updateStatus = ok ? ("ok: " + String(bl.numHashes) + " domains") : ("bad data (" + String(total) + "B)");
    Serial.printf("[remote] %s\n", updateStatus.c_str());
    return ok;
}

void web_begin() {
    loadUpdateCfg();
    {
        const char *hdrs[] = {CSRF_HEADER};
        web.collectHeaders(hdrs, 1);
    } // needed for requireAuth()'s CSRF check
    web.on("/", []() { web.send_P(200, "text/html", PAGE); });
    web.on("/stats.json", handleStats);
    web.on("/ban", handleBan);
    web.on("/addblock", []() { if (!requireAuth()) return; bl.addCustom(web.arg("d")); web.send(200, "text/plain", "ok"); });
    web.on("/unblock", []() { if (!requireAuth()) return; bl.removeCustom(web.arg("d")); web.send(200, "text/plain", "ok"); });
    web.on("/pause", []() { // /pause?s=300  (0 or absent = indefinite)
        if (!requireAuth())
            return;
        long s = web.hasArg("s") ? web.arg("s").toInt() : 0;
        dns.blockingOn = false;
        dns.resumeAt = (s > 0) ? millis() + (uint32_t)s * 1000UL : 0;
        web.send(200, "text/plain", "paused");
    });
    web.on("/resume", []() { if (!requireAuth()) return; dns.blockingOn = true; dns.resumeAt = 0; web.send(200, "text/plain", "resumed"); });
    web.on("/forgetwifi", []() { if (!requireAuth()) return; web.send(200, "text/plain", "cleared — rebooting into setup portal");
    wifi.clearCreds(); delay(500); ESP.restart(); });
    web.on("/upload", HTTP_POST, handleUploadDone, handleUpload); // blocklist upload (auth inside handleUpload)
    web.on("/fetchnow", []() { if (!requireAuth()) return; fetchBlocklist(updateUrl); web.send(200, "text/plain", updateStatus); });
    web.on("/setupdate", []() {
        if (!requireAuth())
            return;
        if (web.hasArg("u"))
            updateUrl = web.arg("u");
        if (web.hasArg("h")) {
            updateIntervalH = web.arg("h").toInt();
            if (updateIntervalH < 1)
                updateIntervalH = 1;
        }
        saveUpdateCfg();
        web.send(200, "text/plain", "ok");
    });
    web.begin();
}

void web_tick() {
    web.handleClient();
    if (updateUrl.length()) { // periodic remote blocklist auto-update
        uint32_t now = millis();
        if (lastCheckMs == 0)
            lastCheckMs = now; // skip an immediate fetch on boot
        else if (now - lastCheckMs >= updateIntervalH * 3600000UL) {
            lastCheckMs = now;
            fetchBlocklist(updateUrl);
        }
    }
}
