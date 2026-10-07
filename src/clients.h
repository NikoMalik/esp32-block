// per-client table (ip, mac, counters, ban) + banned-IP persistence
#pragma once
#include <Arduino.h>

struct Dev {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t blocked, allowed, lastSeen;
    bool banned;
    String label;
};

struct Clients {
    static const int MAX = 96;
    static const int MAX_BAN = 32;
    Dev list[MAX];
    int count = 0;
    int bannedCount = 0;

    Dev *get(uint32_t ip);     // find or create the entry for ip
    bool isBanned(uint32_t ip);
    void loadBanned();
    void saveBanned();         // rebuild ban list from current clients, persist
};

extern Clients clients;
