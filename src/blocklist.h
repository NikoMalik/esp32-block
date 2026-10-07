// blocklist: v2 bucketed blob in a raw flash partition, memory-mapped and looked
// up by 16-bit prefix (bucket) + 24-bit suffix; plus a small runtime custom list
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "fxhash.h"
#include "blfmt.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"

static const char *const BLOCKLIST_PART = "blocklist"; // raw partition label
static const char *const BLOCKLIST_NEW = "/blocklist.new";

struct Blocklist {
    static const uint64_t HASH_MASK = (1ULL << 40) - 1;
    static const int MAX_CUSTOM = 200;

    const uint8_t *base = nullptr; // mmap'd partition
    spi_flash_mmap_handle_t mmapHandle = 0;
    const uint32_t *bucketOff = nullptr; // BL_BUCKETS+1 start offsets
    const uint8_t *suffixes = nullptr;   // numHashes * 3 bytes
    uint32_t numHashes = 0;

    String customDom[MAX_CUSTOM]; // runtime blocks, listed in dashboard
    uint64_t customHash[MAX_CUSTOM];
    int numCustom = 0;

    // streaming write of a new blob straight into the raw partition
    const esp_partition_t *wpart = nullptr;
    uint32_t writePos = 0;     // flushed-to-flash byte offset (multiple of 4096)
    size_t totalWritten = 0;   // total bytes received
    uint32_t crcRun = 0;       // running CRC over payload (bytes >= header)
    uint8_t whdr[BL_HEADER_SIZE];
    uint8_t wbuf[4096];        // flash write buffer (sector aligned)
    uint16_t wbufLen = 0;

    void begin();                      // map blob, load custom
    bool contains(const char *domain); // domain or any parent blocked
    bool mapBlob();                    // find partition, mmap, parse header
    void reopen();                     // re-map after an update
    void beginSwap();                  // erase partition, start a streamed write
    bool writeChunk(const uint8_t *d, size_t n); // append received bytes
    bool commitNew();                  // flush, verify CRC, re-map
    bool addCustom(String d);
    void removeCustom(String d);
    void loadCustom();
    void saveCustom();
    void bench();
};

extern Blocklist bl;
