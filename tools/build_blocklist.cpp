// host tool: fetch blocklists, parse, fxhash to 40-bit, write sorted blob for the device
// output format must match the firmware reader (same hash, 5-byte little-endian, sorted)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/fxhash.h"
#include "../src/blfmt.h"

#define HASH_BYTES 5   // must match firmware HASH_BYTES
static const uint64_t MASK = (1ULL << (HASH_BYTES * 8)) - 1;

// default lists: global ads/trackers/malware aggregators (OISD + Hagezi) + base + Russian
static const char *DEFAULT_SOURCES[] = {
    "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts",
    "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro.plus-onlydomains.txt",
    "https://raw.githubusercontent.com/sjhgvr/oisd/main/domainswild2_big.txt",
    "https://raw.githubusercontent.com/Zalexanninev15/NoADS_RU/main/hosts/blocker.txt",
    "https://raw.githubusercontent.com/Zalexanninev15/NoADS_RU/main/hosts/blockerFL.txt",
};
static const int DEFAULT_COUNT = (int)(sizeof(DEFAULT_SOURCES) / sizeof(DEFAULT_SOURCES[0]));

static int allow_missing = 0;

// read a local file or, for http(s), download via curl into out
static int read_source(const char *src, std::string &out) {
    out.clear();
    FILE *f;
    int is_url = (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0);
    if (is_url) {
        fprintf(stderr, "  downloading %s ...\n", src);
        std::string cmd = "curl -fsSL '";
        cmd += src;
        cmd += "'";
        f = popen(cmd.c_str(), "r");
        if (!f) return -1;
    } else {
        f = fopen(src, "rb");
        if (!f) return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    int rc = is_url ? pclose(f) : fclose(f);
    return rc == 0 ? 0 : -1;
}

// canonical domain form so host and firmware hash the same string
static std::string norm(const char *s, size_t n) {
    std::string d;
    d.reserve(n);
    for (size_t i = 0; i < n; i++) d += (char)tolower((unsigned char)s[i]);
    size_t a = 0, b = d.size();
    while (a < b && (d[a] == '*' || d[a] == '.')) a++;
    while (b > a && d[b - 1] == '.') b--;
    d = d.substr(a, b - a);
    if (d.size() > 4 && d.compare(0, 4, "www.") == 0) d = d.substr(4);
    return d;
}

static int is_domain_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-';
}

static int adg_match(const char *line, int *is_allow, int *has_mod, std::string &domain) {
    const char *p = line;
    *is_allow = 0;
    *has_mod = 0;
    if (p[0] == '@' && p[1] == '@') { *is_allow = 1; p += 2; }
    if (!(p[0] == '|' && p[1] == '|')) return 0;
    p += 2;
    const char *dstart = p;
    while (*p && is_domain_char(*p)) p++;
    if (p == dstart) return 0;
    const char *dend = p;
    if (*p == '^') p++;
    if (*p == '$') *has_mod = 1;
    else if (*p != 0) return 0;
    domain.assign(dstart, dend - dstart);
    return 1;
}

static void add_block(std::vector<std::string> &domains, const std::string &d) {
    if (d.find('.') != std::string::npos && d.find(' ') == std::string::npos) domains.push_back(d);
}

int main(int argc, char **argv) {
    std::vector<const char *> args;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--allow-missing") == 0) allow_missing = 1;
        else args.push_back(argv[i]);
    }
    const char *out = args.size() > 0 ? args[0] : "blocklist.bin";

    std::vector<const char *> sources;
    if (args.size() > 1) for (size_t i = 1; i < args.size(); i++) sources.push_back(args[i]);
    else for (int i = 0; i < DEFAULT_COUNT; i++) sources.push_back(DEFAULT_SOURCES[i]);

    std::vector<std::string> domains, allow;
    long skipped = 0;

    for (size_t si = 0; si < sources.size(); si++) {
        std::string data;
        if (read_source(sources[si], data) != 0) {
            fprintf(stderr, "  !! FAILED to read %s\n", sources[si]);
            if (!allow_missing) return 1;
            continue;
        }
        size_t pos = 0, len = data.size();
        while (pos < len) {
            size_t eol = data.find('\n', pos);
            if (eol == std::string::npos) eol = len;
            std::string raw = data.substr(pos, eol - pos);
            pos = eol + 1;
            while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();

            size_t ls = 0;
            while (ls < raw.size() && (raw[ls] == ' ' || raw[ls] == '\t')) ls++;
            int abp = (raw.compare(ls, 2, "||") == 0 || raw.compare(ls, 2, "@@") == 0);

            std::string line;
            if (abp) {
                line = raw.substr(ls);
            } else {
                size_t h = raw.find('#');
                line = raw.substr(0, h == std::string::npos ? raw.size() : h);
                size_t a = 0, b = line.size();
                while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
                while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
                line = line.substr(a, b - a);
            }
            if (line.empty() || line[0] == '!' || line[0] == '/' || line[0] == '[') continue;

            int is_allow, has_mod;
            std::string dom;
            if (adg_match(line.c_str(), &is_allow, &has_mod, dom)) {
                if (has_mod) { skipped++; continue; }
                std::string nd = norm(dom.c_str(), dom.size());
                if (is_allow) allow.push_back(nd);
                else add_block(domains, nd);
                continue;
            }
            if (line.compare(0, 2, "||") == 0 || line.compare(0, 2, "@@") == 0 || line[0] == '|' ||
                line.find('^') != std::string::npos || line.find('$') != std::string::npos ||
                line.find('*') != std::string::npos) {
                skipped++;
                continue;
            }
            std::vector<std::string> parts;
            size_t tp = 0;
            while (tp < line.size()) {
                while (tp < line.size() && (line[tp] == ' ' || line[tp] == '\t')) tp++;
                size_t ts = tp;
                while (tp < line.size() && line[tp] != ' ' && line[tp] != '\t') tp++;
                if (tp > ts) parts.push_back(line.substr(ts, tp - ts));
            }
            const char *d = 0;
            std::string hold;
            if (parts.size() >= 2 && (parts[0] == "0.0.0.0" || parts[0] == "127.0.0.1" || parts[0] == "::1" || parts[0] == "::")) {
                hold = parts[1]; d = hold.c_str();
            } else if (parts.size() == 1) {
                hold = parts[0]; d = hold.c_str();
            }
            if (d) add_block(domains, norm(d, strlen(d)));
        }
    }

    std::vector<uint64_t> allow_h;
    allow_h.reserve(allow.size());
    for (size_t i = 0; i < allow.size(); i++) allow_h.push_back(fxhash64((const uint8_t *)allow[i].data(), allow[i].size()) & MASK);
    std::sort(allow_h.begin(), allow_h.end());
    allow_h.erase(std::unique(allow_h.begin(), allow_h.end()), allow_h.end());

    std::sort(domains.begin(), domains.end());
    domains.erase(std::unique(domains.begin(), domains.end()), domains.end());

    long removed = 0;
    std::vector<uint64_t> hashes;
    hashes.reserve(domains.size());
    for (size_t i = 0; i < domains.size(); i++) {
        uint64_t h = fxhash64((const uint8_t *)domains[i].data(), domains[i].size()) & MASK;
        if (std::binary_search(allow_h.begin(), allow_h.end(), h)) { removed++; continue; }
        hashes.push_back(h);
    }
    long distinct_domains = (long)hashes.size();

    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());

    // v2 bucketed layout: hashes are sorted, so they're already grouped by the
    // 16-bit prefix (bucket) and sorted by the 24-bit suffix within each bucket
    uint32_t cnt = (uint32_t)hashes.size();
    std::vector<uint32_t> off(BL_BUCKETS + 1, 0);
    for (size_t i = 0; i < hashes.size(); i++) off[(hashes[i] >> BL_SUFFIX_BYTES * 8) + 1]++;
    for (int b = 1; b <= BL_BUCKETS; b++) off[b] += off[b - 1]; // prefix sum -> start offsets

    std::vector<uint8_t> payload((size_t)(BL_BUCKETS + 1) * 4 + (size_t)cnt * BL_SUFFIX_BYTES);
    size_t p = 0;
    for (int b = 0; b <= BL_BUCKETS; b++) {
        for (int k = 0; k < 4; k++) payload[p++] = (uint8_t)(off[b] >> (8 * k));
    }
    for (size_t i = 0; i < hashes.size(); i++) {
        uint32_t s = (uint32_t)(hashes[i] & BL_SUFFIX_MASK);
        for (int k = 0; k < BL_SUFFIX_BYTES; k++) payload[p++] = (uint8_t)(s >> (8 * k));
    }
    uint32_t crc = bl_crc32(payload.data(), payload.size());
    uint8_t hdr[BL_HEADER_SIZE] = {BL_MAGIC0, BL_MAGIC1, BL_MAGIC2, BL_MAGIC3, BL_VERSION, BL_SUFFIX_BYTES, 0, 0};
    for (int k = 0; k < 4; k++) hdr[8 + k] = (uint8_t)(cnt >> (8 * k));
    for (int k = 0; k < 4; k++) hdr[12 + k] = (uint8_t)(crc >> (8 * k));

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", out); return 1; }
    fwrite(hdr, 1, BL_HEADER_SIZE, f);
    fwrite(payload.data(), 1, payload.size(), f);
    fclose(f);

    long n = (long)hashes.size();
    long size = BL_HEADER_SIZE + (long)payload.size();
    if (allow_h.size()) fprintf(stderr, "allowlisted      : %ld removed (%zu @@ rules)\n", removed, allow_h.size());
    if (skipped) fprintf(stderr, "skipped rules    : %ld (adblock syntax a DNS hash list cannot express)\n", skipped);
    printf("source domains   : %ld\n", distinct_domains);
    printf("hash entries     : %ld  (fxhash, v2 bucketed: 16-bit prefix + 3-byte suffix)\n", n);
    printf("collisions       : %ld  (domains sharing a hash -> over-block)\n", distinct_domains - n);
    printf("bucket table     : %d bytes (%d buckets)\n", (BL_BUCKETS + 1) * 4, BL_BUCKETS);
    printf("flash blob       : %ld bytes  (%.2f MB)  -> %s\n", size, size / 1024.0 / 1024.0, out);
    printf("avg per bucket   : %.1f\n", (double)n / BL_BUCKETS);
    return 0;
}
