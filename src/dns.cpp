#include "dns.h"
#include "blocklist.h"
#include "clients.h"

// ASCII lowercase table, filled once at startup; a table lookup avoids the
// locale-aware tolower() call per char in the query hot path
static uint8_t LOWER[256];
static const bool LOWER_INIT = [] {
    for (int i = 0; i < 256; i++)
        LOWER[i] = (i >= 'A' && i <= 'Z') ? (uint8_t)(i + 32) : (uint8_t)i;
    return true;
}();

#ifndef UPSTREAM_IP
#define UPSTREAM_IP 9, 9, 9, 9 // Quad9
#endif
#ifndef UPSTREAM_PORT
#define UPSTREAM_PORT 53
#endif
static const IPAddress UPSTREAM(UPSTREAM_IP);
static const uint16_t DNS_PORT = 53;

Dns dns;

// pull the queried name (lowercased, www. stripped) out of the packet
static size_t parseQuery(const uint8_t *pkt, int len, char *out, uint16_t *qtype, int *qend) {
    if (len < 13)
        return 0;
    int i = 12;
    size_t o = 0;
    while (i < len) {
        uint8_t l = pkt[i++];
        if (l == 0)
            break;
        if (l & 0xC0)
            return 0;
        if (o + l + 1 >= 250 || i + l > len)
            return 0;
        if (o)
            out[o++] = '.';
        for (uint8_t k = 0; k < l; k++)
            out[o++] = LOWER[pkt[i++]];
    }
    out[o] = 0;
    if (i + 4 > len)
        return 0;
    *qtype = (pkt[i] << 8) | pkt[i + 1];
    *qend = i + 4;
    if (o > 4 && strncmp(out, "www.", 4) == 0) {
        memmove(out, out + 4, o - 3);
        o -= 4;
    }
    return o;
}

// turn the query in buf into an A=0.0.0.0 answer
int Dns::buildBlocked(int qend, uint16_t qtype) {
    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = 0;
    buf[7] = (qtype == 1) ? 1 : 0;
    buf[8] = 0;
    buf[9] = 0;
    buf[10] = 0;
    buf[11] = 0;
    if (qtype != 1)
        return qend;
    const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 1, 0x2C, 0, 4, 0, 0, 0, 0};
    memcpy(buf + qend, ans, sizeof(ans));
    return qend + sizeof(ans);
}

// forward buf upstream and return only the reply that matches THIS query
// (fresh random txid + question match, so a late reply can't shift to another client)
int Dns::forwardUpstream(int qlen, int qend) {
    upstream.flush();
    while (upstream.parsePacket() > 0)
        upstream.flush();
    const uint8_t cid0 = buf[0], cid1 = buf[1];
    const uint16_t wid = (uint16_t)esp_random();
    uint8_t q[260];
    int ql = qend - 12;
    const bool haveQ = ql > 0 && ql <= (int)sizeof(q) && qend <= qlen;
    if (haveQ)
        memcpy(q, buf + 12, ql);
    buf[0] = wid >> 8;
    buf[1] = wid & 0xFF;
    upstream.beginPacket(UPSTREAM, UPSTREAM_PORT);
    upstream.write(buf, qlen);
    upstream.endPacket();
    const uint32_t t0 = millis();
    while (millis() - t0 < 1000) {
        int sz = upstream.parsePacket();
        if (sz <= 0) {
            delay(1);
            continue;
        }
        const bool fromUp = upstream.remoteIP() == UPSTREAM && upstream.remotePort() == UPSTREAM_PORT;
        int n = upstream.read(buf, sizeof(buf));
        upstream.flush();
        if (!fromUp || n < 12 || sz > (int)sizeof(buf))
            continue;
        if (buf[0] != (wid >> 8) || buf[1] != (wid & 0xFF))
            continue;
        if (haveQ && (n < 12 + ql || memcmp(buf + 12, q, ql) != 0))
            continue;
        buf[0] = cid0;
        buf[1] = cid1;
        return n;
    }
    return 0;
}

void Dns::begin() {
    server.begin(DNS_PORT);
    upstream.begin(0);
}

// drain a burst per call (capped so web/OTA still get a turn)
bool Dns::handle() {
    bool did = false;
    for (int budget = 0; budget < 16; budget++) {
        int sz = server.parsePacket();
        if (sz <= 0)
            break;
        did = true;
        IPAddress cip = server.remoteIP();
        uint16_t cport = server.remotePort();
        int qlen = server.read(buf, sizeof(buf));
        if (qlen < 13)
            continue;
        char domain[256];
        uint16_t qtype = 0;
        int qend = qlen;
        size_t dl = parseQuery(buf, qlen, domain, &qtype, &qend);
        Dev *c = clients.get((uint32_t)cip);
        bool ban = c && c->banned;
        bool blocked = ban || (blockingOn && dl && bl.numHashes && bl.contains(domain));
        int rlen;
        if (blocked) {
            rlen = buildBlocked(qend, qtype);
            totalBlocked++;
            if (c)
                c->blocked++;
        } else {
            rlen = forwardUpstream(qlen, qend);
            totalAllowed++;
            if (c)
                c->allowed++;
        }
        if (rlen > 0) {
            server.beginPacket(cip, cport);
            server.write(buf, rlen);
            server.endPacket();
        }
    }
    return did;
}
