#include "wifi.h"
#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h"

extern WebServer web;

Wifi wifi;

static String esc(const String &s) {
    String o;
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    return o;
}

// native <select> (datalist silently fails in some in-app/captive browsers) plus
// a manual field for hidden networks
static void portalRoot() {
    const char *inp = "width:100%;box-sizing:border-box;padding:11px;margin:6px 0;border-radius:6px;border:1px solid #30363d;background:#161b22;color:#c9d1d9";
    String html =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>AdBlock setup</title>"
        "<body style='font:16px system-ui,sans-serif;max-width:420px;margin:30px auto;padding:0 16px;background:#0d1117;color:#c9d1d9'>"
        "<h2>&#128737; AdBlock — WiFi setup</h2>"
        "<p style='color:#8b949e'>Pick your home network, enter its password, and tap Connect. The box reboots and joins it.</p>"
        "<form method=POST action=/wifisave>"
        "<select name=s style='" + String(inp) + "'>" + wifi.portalOpts + "</select>"
        "<input name=s2 placeholder='or type the network name' style='" + String(inp) + "'>"
        "<input name=p type=password placeholder='WiFi password' style='" + String(inp) + "'>"
        "<button style='width:100%;padding:12px;margin-top:8px;border-radius:6px;border:0;background:#3fb950;color:#000;font-weight:600;cursor:pointer'>Connect</button>"
        "</form>"
        "<p style='color:#8b949e;font-size:12px'>Not in the list? Type the name (for hidden networks).</p>"
        "</body>";
    web.send(200, "text/html", html);
}

static void wifiSave() {
    String ss = web.arg("s2"); // manual field wins over the dropdown
    ss.trim();
    if (!ss.length())
        ss = web.arg("s");
    ss.trim();
    String pw = web.arg("p");
    if (!ss.length()) {
        web.send(400, "text/plain", "no network selected");
        return;
    }
    wifi.prefs.begin("wifi", false);
    wifi.prefs.putString("ssid", ss);
    wifi.prefs.putString("pass", pw);
    wifi.prefs.end();
    web.send(200, "text/html", "<!doctype html><meta charset=utf-8><body style='font:16px system-ui;text-align:center;margin-top:60px;background:#0d1117;color:#c9d1d9'>"
                               "&#9989; Done. Rebooting and joining <b>" +
                                   ss + "</b>&hellip;<br><br>"
                                        "Switch your phone back to your normal WiFi.</body>");
    delay(900);
    ESP.restart();
}

bool Wifi::hasCreds() {
    prefs.begin("wifi", true);
    bool nvs = prefs.getString("ssid", "").length() > 0;
    prefs.end();
    return nvs || (WIFI_SSID && *WIFI_SSID && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0);
}

bool Wifi::connect() {
    prefs.begin("wifi", true);
    String ss = prefs.getString("ssid", "");
    String pw = prefs.getString("pass", "");
    prefs.end();
    const char *ssid = ss.length() ? ss.c_str() : WIFI_SSID;
    const char *pass = ss.length() ? pw.c_str() : WIFI_PASS;
    if (!ssid || !*ssid || strcmp(ssid, "YOUR_WIFI_SSID") == 0)
        return false;
    Serial.printf("WiFi: connecting to \"%s\"%s\n", ssid, ss.length() ? " (provisioned)" : " (secrets.h)");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

void Wifi::clearCreds() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
}

// blocks until creds are saved (then reboots); a configured device that just
// failed to join reboots after 3 min of nobody using the portal
void Wifi::runPortal() {
    WiFi.mode(WIFI_STA);   // scan needs station mode
    WiFi.disconnect(false); // drop the failed/pending association, else scan returns -2
    delay(200);
    int n = 0;
    for (int a = 0; a < 3 && n <= 0; a++) {
        n = WiFi.scanNetworks();
        Serial.printf("[setup] scan %d/3: %d networks\n", a + 1, n);
        if (n <= 0) {
            WiFi.scanDelete();
            delay(600);
        }
    }
    portalOpts = "<option value=''>— select network —</option>";
    for (int i = 0; i < n && i < 15; i++) {
        String ss = esc(WiFi.SSID(i));
        portalOpts += "<option value='" + ss + "'>" + ss + "</option>";
    }
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char ap[24];
    snprintf(ap, sizeof(ap), "C3-AdBlock-%02X%02X", mac[4], mac[5]);
    WiFi.mode(WIFI_AP);
    bool locked = strlen(AP_PASS) >= 8;
    if (locked)
        WiFi.softAP(ap, AP_PASS); // WPA2: only someone with the password can open setup
    else
        WiFi.softAP(ap);
    IPAddress apIP = WiFi.softAPIP();
    portalDns.start(53, "*", apIP);
    web.on("/", portalRoot);
    web.on("/wifisave", HTTP_POST, wifiSave);
    // OS captive-detection probes (Android /generate_204, Apple /hotspot-detect.html,
    // Windows /ncsi.txt) hit onNotFound -> redirect them so the phone auto-pops the form
    web.onNotFound([]() {
        web.sendHeader("Location", "http://192.168.4.1/", true);
        web.send(302, "text/plain", "");
    });
    web.begin();
    Serial.printf("\n[setup] No WiFi. Join network \"%s\" (%s), a setup page pops up (or http://%s)\n",
                  ap, locked ? "password-protected" : "open", apIP.toString().c_str());
    const bool configured = hasCreds();
    uint32_t t0 = millis();
    while (true) {
        portalDns.processNextRequest();
        web.handleClient();
        delay(2);
        if (WiFi.softAPgetStationNum() > 0)
            t0 = millis();
        if (configured && millis() - t0 > 180000UL) {
            Serial.println("[setup] retrying WiFi");
            ESP.restart();
        }
    }
}
