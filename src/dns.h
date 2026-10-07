// DNS sinkhole: answer blocked names with 0.0.0.0, forward the rest upstream
#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

struct Dns {
    WiFiUDP server, upstream;
    uint8_t buf[1536];                   // packet scratch, EDNS replies exceed 512
    uint32_t totalBlocked = 0, totalAllowed = 0;
    bool blockingOn = true;              // pause toggle (Pi-hole style)
    uint32_t resumeAt = 0;               // millis to auto-resume, 0 = indefinite

    void begin();                        // bind sockets
    bool handle();                       // drain a burst of queries, true if any
    int buildBlocked(int qend, uint16_t qtype);
    int forwardUpstream(int qlen, int qend);
};

extern Dns dns;
