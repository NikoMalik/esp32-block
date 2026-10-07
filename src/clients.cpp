#include "clients.h"
#include <LittleFS.h>
#include "lwip/etharp.h"
#include "lwip/netif.h"

Clients clients;

static uint32_t bannedIP[Clients::MAX_BAN];

bool Clients::isBanned(uint32_t ip) {
    for (int i = 0; i < bannedCount; i++)
        if (bannedIP[i] == ip)
            return true;
    return false;
}

void Clients::loadBanned() {
    bannedCount = 0;
    File f = LittleFS.open("/banned.txt", "r");
    if (!f)
        return;
    while (f.available() && bannedCount < MAX_BAN) {
        String l = f.readStringUntil('\n');
        l.trim();
        IPAddress ip;
        if (l.length() && ip.fromString(l))
            bannedIP[bannedCount++] = (uint32_t)ip;
    }
    f.close();
}

void Clients::saveBanned() {
    bannedCount = 0;
    for (int i = 0; i < count && bannedCount < MAX_BAN; i++)
        if (list[i].banned)
            bannedIP[bannedCount++] = list[i].ip;
    File f = LittleFS.open("/banned.txt", "w");
    if (!f)
        return;
    for (int i = 0; i < bannedCount; i++) {
        IPAddress ip(bannedIP[i]);
        f.println(ip.toString());
    }
    f.close();
}

// resolve ip to mac via the lwIP ARP table
static void getMac(uint32_t ip, uint8_t *mac) {
    memset(mac, 0, 6);
    ip4_addr_t ipa;
    ipa.addr = ip;
    struct eth_addr *eth = nullptr;
    const ip4_addr_t *ipret = nullptr;
    for (struct netif *nif = netif_list; nif; nif = nif->next)
        if (etharp_find_addr(nif, &ipa, &eth, &ipret) >= 0 && eth) {
            memcpy(mac, eth->addr, 6);
            return;
        }
}

Dev *Clients::get(uint32_t ip) {
    for (int i = 0; i < count; i++)
        if (list[i].ip == ip) {
            list[i].lastSeen = millis();
            return &list[i];
        }
    if (count < MAX) {
        Dev *c = &list[count++];
        c->ip = ip;
        c->blocked = c->allowed = 0;
        c->lastSeen = millis();
        c->banned = isBanned(ip);
        c->label = "";
        getMac(ip, c->mac);
        return c;
    }
    return nullptr;
}
