#pragma once
// Copy this file to secrets.h and fill in your WiFi credentials.
// secrets.h is gitignored so your credentials never get committed.
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Login for the dashboard's control actions (ban, add/remove domain, pause,
// blocklist upload, forget-wifi). Pick a real value before putting the box on a
// network you don't fully trust, ideally not the same as your WiFi password.
static const char *WEB_USER = "admin";
static const char *WEB_PASS = "CHANGE_ME_WEB_PASSWORD";

// Password for the setup access point (shown during WiFi provisioning). >=8 chars
// enables WPA2 so only someone you gave the password to can open the setup page.
// Empty string leaves the setup AP open (anyone nearby can reach it).
static const char *AP_PASS = "";
