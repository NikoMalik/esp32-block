// AdBlock — DNS sinkhole + web dashboard for the ESP32 (no PSRAM)
// Blocklist = sorted 40-bit fxhash values in flash, binary-searched
// Modules: blocklist, clients, dns, wifi, web
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "secrets.h"
#include "blocklist.h"
#include "clients.h"
#include "dns.h"
#include "wifi.h"
#include "web.h"

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[adblock] booting");
    if (!LittleFS.begin(true))
        Serial.println("LittleFS FAILED");

    bl.begin();
    Serial.printf("blocklist: %u domains\n", bl.numHashes);
    // bl.bench(); // no bench in prod,enable it for dev
    // ps.. idk how to pass arguments to pass bench
    clients.loadBanned();
    Serial.printf("custom: %d, banned: %d\n", bl.numCustom, clients.bannedCount);

    // hold BOOT at power-on to wipe saved WiFi and force the setup portal
#if CONFIG_IDF_TARGET_ESP32C3
    const int BOOT_PIN = 9; // C3 BOOT button
#else
    const int BOOT_PIN = 0; // classic ESP32 BOOT button (GPIO9 is a flash pin there)
#endif
    pinMode(BOOT_PIN, INPUT_PULLUP);
    if (digitalRead(BOOT_PIN) == LOW) {
        delay(60);
        if (digitalRead(BOOT_PIN) == LOW) {
            wifi.clearCreds();
            Serial.println("[setup] BOOT held -> cleared saved WiFi");
        }
    }

    if (!wifi.connect())
        wifi.runPortal(); // blocks + reboots on save, returns only when connected
    Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("c3adblock")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("dashboard: http://c3adblock.local");
    }
    if (strcmp(WEB_PASS, "CHANGE_ME_WEB_PASSWORD") == 0 || strcmp(OTA_PASS, "CHANGE_ME_OTA_PASSWORD") == 0)
        Serial.println("[WARN] secrets.h still has placeholder WEB_PASS/OTA_PASS — set real values");

    dns.begin();
    web_begin();
    Serial.println("DNS :53 + dashboard :80 up");
}

void loop() {
    if (Serial.available() && Serial.read() == 'b')
        bl.bench(); // type 'b' in the serial monitor to benchmark lookups
    web_tick();
    bool busy = dns.handle();
    if (!dns.blockingOn && dns.resumeAt && (int32_t)(millis() - dns.resumeAt) >= 0) {
        dns.blockingOn = true;
        dns.resumeAt = 0;
    }
    if (!busy)
        delay(1); // sleep only when idle: full speed under load, cool when quiet
}
