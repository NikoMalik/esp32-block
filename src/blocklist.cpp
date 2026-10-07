#include "blocklist.h"

Blocklist bl;

static inline uint64_t hash40(const char *s, size_t n) {
    return fxhash64((const uint8_t *)s, n) & Blocklist::HASH_MASK;
}

// find the blocklist partition, read+check the header, memory-map the used bytes
bool Blocklist::mapBlob() {
    if (base) {
        spi_flash_munmap(mmapHandle);
        base = nullptr;
    }
    bucketOff = nullptr;
    suffixes = nullptr;
    numHashes = 0;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BLOCKLIST_PART);
    if (!part) {
        Serial.println("[bl] no 'blocklist' partition");
        return false;
    }
    uint8_t hdr[BL_HEADER_SIZE];
    if (esp_partition_read(part, 0, hdr, BL_HEADER_SIZE) != ESP_OK)
        return false;
    if (!(hdr[0] == BL_MAGIC0 && hdr[1] == BL_MAGIC1 && hdr[2] == BL_MAGIC2 && hdr[3] == BL_MAGIC3 &&
          hdr[4] == BL_VERSION && hdr[5] == BL_SUFFIX_BYTES)) {
        Serial.println("[bl] empty or bad blocklist blob");
        return false;
    }
    uint32_t cnt = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    size_t need = BL_HEADER_SIZE + (size_t)(BL_BUCKETS + 1) * 4 + (size_t)cnt * BL_SUFFIX_BYTES;
    if (need > part->size) {
        Serial.println("[bl] blob bigger than partition");
        return false;
    }
    const void *ptr = nullptr;
    if (esp_partition_mmap(part, 0, need, SPI_FLASH_MMAP_DATA, &ptr, &mmapHandle) != ESP_OK) {
        Serial.println("[bl] mmap failed");
        return false;
    }
    base = (const uint8_t *)ptr;
    bucketOff = (const uint32_t *)(base + BL_HEADER_SIZE);
    suffixes = base + BL_HEADER_SIZE + (size_t)(BL_BUCKETS + 1) * 4;
    numHashes = cnt;
    return true;
}

// one bucket (avg ~7 entries), suffixes sorted -> linear scan with early-out
static bool inBlob(uint64_t h) {
    if (!bl.suffixes)
        return false;
    uint32_t b = (uint32_t)(h >> 24);
    uint32_t suf = (uint32_t)(h & BL_SUFFIX_MASK);
    uint32_t start = bl.bucketOff[b], end = bl.bucketOff[b + 1];
    const uint8_t *p = bl.suffixes + (size_t)start * BL_SUFFIX_BYTES;
    for (uint32_t i = start; i < end; i++, p += BL_SUFFIX_BYTES) {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        if (v == suf)
            return true;
        if (v > suf)
            break;
    }
    return false;
}

static bool inCustom(uint64_t h) {
    for (int i = 0; i < bl.numCustom; i++)
        if (bl.customHash[i] == h)
            return true;
    return false;
}

bool Blocklist::contains(const char *domain) {
    const char *p = domain;
    size_t len = strlen(domain);
    while (*p) {
        uint64_t h = hash40(p, len);
        if (inCustom(h) || inBlob(h))
            return true;
        const char *dot = (const char *)memchr(p, '.', len);
        if (!dot)
            break;
        const char *next = dot + 1;
        size_t nlen = len - (size_t)(next - p);
        if (!memchr(next, '.', nlen))
            break;
        p = next;
        len = nlen;
    }
    return false;
}

void Blocklist::reopen() {
    mapBlob();
}

// unmap, find + erase the raw partition, reset the streamed-write state
void Blocklist::beginSwap() {
    if (base) {
        spi_flash_munmap(mmapHandle);
        base = nullptr;
    }
    bucketOff = nullptr;
    suffixes = nullptr;
    numHashes = 0;
    writePos = 0;
    totalWritten = 0;
    wbufLen = 0;
    crcRun = BL_CRC_INIT;
    wpart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BLOCKLIST_PART);
    if (!wpart) {
        Serial.println("[bl] no partition to write");
        return;
    }
    if (esp_partition_erase_range(wpart, 0, wpart->size) != ESP_OK) {
        Serial.println("[bl] erase failed");
        wpart = nullptr;
    }
}

bool Blocklist::writeChunk(const uint8_t *d, size_t n) {
    if (!wpart)
        return false;
    // capture header bytes and CRC the payload (everything past the 16-byte header)
    size_t o = totalWritten;
    for (size_t i = 0; i < n && o + i < BL_HEADER_SIZE; i++)
        whdr[o + i] = d[i];
    if (o + n > BL_HEADER_SIZE) {
        size_t skip = o < BL_HEADER_SIZE ? BL_HEADER_SIZE - o : 0;
        crcRun = bl_crc32_update(crcRun, d + skip, n - skip);
    }
    totalWritten += n;
    // buffer into sector-sized flash writes
    while (n) {
        size_t take = sizeof(wbuf) - wbufLen;
        if (take > n)
            take = n;
        memcpy(wbuf + wbufLen, d, take);
        wbufLen += take;
        d += take;
        n -= take;
        if (wbufLen == sizeof(wbuf)) {
            if (esp_partition_write(wpart, writePos, wbuf, sizeof(wbuf)) != ESP_OK)
                return false;
            writePos += sizeof(wbuf);
            wbufLen = 0;
        }
    }
    return true;
}

// flush the tail, check header + CRC + size, then re-map if good
bool Blocklist::commitNew() {
    if (!wpart)
        return false;
    if (wbufLen) {
        size_t pad = (4 - (wbufLen & 3)) & 3; // flash writes are 4-byte aligned
        while (pad--)
            wbuf[wbufLen++] = 0xFF;
        if (esp_partition_write(wpart, writePos, wbuf, wbufLen) != ESP_OK)
            return false;
        writePos += wbufLen;
        wbufLen = 0;
    }
    wpart = nullptr;
    bool ok = totalWritten >= BL_HEADER_SIZE &&
              whdr[0] == BL_MAGIC0 && whdr[1] == BL_MAGIC1 && whdr[2] == BL_MAGIC2 && whdr[3] == BL_MAGIC3 &&
              whdr[4] == BL_VERSION && whdr[5] == BL_SUFFIX_BYTES;
    if (ok) {
        uint32_t cnt = (uint32_t)whdr[8] | ((uint32_t)whdr[9] << 8) | ((uint32_t)whdr[10] << 16) | ((uint32_t)whdr[11] << 24);
        uint32_t want = (uint32_t)whdr[12] | ((uint32_t)whdr[13] << 8) | ((uint32_t)whdr[14] << 16) | ((uint32_t)whdr[15] << 24);
        size_t expect = BL_HEADER_SIZE + (size_t)(BL_BUCKETS + 1) * 4 + (size_t)cnt * BL_SUFFIX_BYTES;
        ok = (totalWritten == expect) && (~crcRun == want);
    }
    mapBlob(); // maps if the written blob is valid, else leaves numHashes=0
    return ok;
}

void Blocklist::loadCustom() {
    numCustom = 0;
    File f = LittleFS.open("/custom.txt", "r");
    if (!f)
        return;
    while (f.available() && numCustom < MAX_CUSTOM) {
        String l = f.readStringUntil('\n');
        l.trim();
        l.toLowerCase();
        if (l.length() && l.indexOf('.') > 0) {
            customDom[numCustom] = l;
            customHash[numCustom] = hash40(l.c_str(), l.length());
            numCustom++;
        }
    }
    f.close();
}

void Blocklist::saveCustom() {
    File f = LittleFS.open("/custom.txt", "w");
    if (!f)
        return;
    for (int i = 0; i < numCustom; i++)
        f.println(customDom[i]);
    f.close();
}

bool Blocklist::addCustom(String d) {
    d.trim();
    d.toLowerCase();
    if (d.startsWith("www."))
        d = d.substring(4);
    if (!d.length() || d.indexOf('.') < 0 || numCustom >= MAX_CUSTOM)
        return false;
    for (int i = 0; i < numCustom; i++)
        if (customDom[i] == d)
            return false;
    customDom[numCustom] = d;
    customHash[numCustom] = hash40(d.c_str(), d.length());
    numCustom++;
    saveCustom();
    return true;
}

void Blocklist::removeCustom(String d) {
    d.toLowerCase();
    for (int i = 0; i < numCustom; i++)
        if (customDom[i] == d) {
            for (int j = i; j < numCustom - 1; j++) {
                customDom[j] = customDom[j + 1];
                customHash[j] = customHash[j + 1];
            }
            numCustom--;
            saveCustom();
            return;
        }
}

void Blocklist::begin() {
    mapBlob();
    loadCustom();
}

// time inBlob over N present hashes (reconstructed from the blob) and N absent ones
void Blocklist::bench() {
    if (numHashes == 0) {
        Serial.println("[bench] empty blocklist");
        return;
    }
    const int N = 1000;
    uint64_t *hits = (uint64_t *)malloc(N * sizeof(uint64_t));
    uint64_t *misses = (uint64_t *)malloc(N * sizeof(uint64_t));
    if (!hits || !misses) {
        free(hits);
        free(misses);
        Serial.println("[bench] oom");
        return;
    }
    int got = 0;
    int step = BL_BUCKETS / N;
    if (step < 1)
        step = 1;
    for (int b = 0; b < BL_BUCKETS && got < N; b += step) {
        if (bucketOff[b] < bucketOff[b + 1]) {
            const uint8_t *p = suffixes + (size_t)bucketOff[b] * BL_SUFFIX_BYTES;
            uint32_t suf = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            hits[got] = ((uint64_t)b << 24) | suf;
            misses[got] = hits[got] ^ 0x7;
            got++;
        }
    }

    uint32_t c = 0;
    uint32_t t0 = micros();
    for (int i = 0; i < got; i++)
        if (inBlob(hits[i]))
            c++;
    uint32_t dh = micros() - t0;

    uint32_t m = 0;
    uint32_t t1 = micros();
    for (int i = 0; i < got; i++)
        if (inBlob(misses[i]))
            m++;
    uint32_t dm = micros() - t1;

    Serial.printf("[bench] %u domains, N=%d (mmap buckets)\n", numHashes, got);
    Serial.printf("[bench] hit : %u/%d found, %.2f us/lookup, %lu qps\n",
                  c, got, (float)dh / got, (unsigned long)(1000000ULL * got / (dh ? dh : 1)));
    Serial.printf("[bench] miss: %u/%d found, %.2f us/lookup, %lu qps\n",
                  m, got, (float)dm / got, (unsigned long)(1000000ULL * got / (dm ? dm : 1)));
    free(hits);
    free(misses);
}
