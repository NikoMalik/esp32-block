# ESP32 AdBlock

A network-wide ad/tracker blocker on a cheap ESP32. It runs as a **DNS server** for
your home: normal domains resolve as usual, ad/tracker/malware domains get answered
with `0.0.0.0` so they never load. One small box, every device on the WiFi benefits,
no app to install on phones or TVs.

~645,000 domains (ads, trackers, malware, plus Russian lists :) ) live in flash as
40-bit hashes in a bucketed index, memory-mapped and looked up on the device -
**no PSRAM needed**.

- Blocklist memory-mapped from a raw flash partition, ~8 us per lookup (~123k qps)
- Web dashboard: stats, per-device counters, pause, add your own blocked domains
- Phone-based setup, no computer needed by the end user
- Blocklist updates over WiFi; firmware flashed once over USB

> Fork of [M-Abozaid/esp32-c3-adblock](https://github.com/M-Abozaid/esp32-c3-adblock).
> This fork targets the **classic ESP32** (also builds for C3)
---

## What you need

- An **ESP32** board (classic DevKit/WROOM, or ESP32-C3), 4 MB flash, no PSRAM
- A **USB cable** and any USB power source (a phone charger is fine)
- A **2.4 GHz** WiFi network (ESP32 does not see 5 GHz)
- A phone to do the one-time setup

The box only uses USB for power and the initial flashing. It joins your WiFi wirelessly.


on host you need:
```bash
pio,esptool
```

---

## Quick start (end user)

The box comes already flashed. You only connect it to your WiFi:

1. Plug the box into USB power. After ~10 seconds it creates its own WiFi network.
2. On your phone, open WiFi settings and join **`C3-AdBlock-XXXX`**
   (password: whatever came with the box, default `adblocksetup`).
3. A setup page opens automatically. If not, open `http://192.168.4.1` in a browser.
4. Pick your home WiFi from the list (or type its name), enter its password, tap
   **Connect**.
5. The box reboots and joins your network. Switch your phone back to your normal WiFi.

Done - the box is online. Leave it plugged into power.

### Make it block ads on every device

The box is a DNS server; devices use it only if you point their DNS at it. Two ways:

- **Whole home (recommended):** in your **router** settings, under DHCP / LAN, set the
  "DNS server" to the box's IP. Every device then filters automatically.
- **One device:** in that device's WiFi settings, set DNS to the box's IP.

Find the box's IP on its dashboard (the `IP` field), in your router's client list, or
in the serial log. Tip: give the box a **static IP** (DHCP reservation by its MAC) so
the address never changes.

> Router requirements: just a normal home router with a 2.4 GHz network and the ability
> to set a custom DNS (almost all can). No special hardware.

---

## Dashboard

Once the box is online, open **`http://c3adblock.local`** (or its IP) in a browser on
the same network.

- **Stats** - total blocked/allowed, domains in the list, signal, uptime
- **Clients** - per-device counters, ban a device
- **Pause** - stop blocking for N seconds (e.g. to debug a site), then auto-resume
- **Add a blocked domain** - type a domain and it is blocked immediately (stored on
  the box). Use this when something you don't want isn't caught by the lists.
- **Unblock** - remove a domain you added
- **Blocklist update** - upload a new `blocklist.bin`, or set a URL to auto-fetch on a
  schedule

State-changing actions require the dashboard login (`WEB_USER` / `WEB_PASS`).

---

## Build & flash (developers)

Needs [PlatformIO](https://platformio.org/) (`pio`) and `make`.

```fish
# 1. credentials: copy the template and fill it in
cp src/secrets.example.h src/secrets.h
#    WIFI_SSID/WIFI_PASS  - optional fallback (setup is usually done from the phone)
#    WEB_USER/WEB_PASS    - dashboard login
#    AP_PASS              - setup-AP password you hand out (>=8 chars, or "" = open)

# 2. build the blocklist (downloads sources, ~645k domains -> blocklist.bin)
make blocklist

# 3. flash firmware + blocklist over USB (one time)
make flash          # = upload firmware + write blocklist.bin into its raw partition

# 4. watch it boot / find its IP
make monitor        # look for "WiFi up: <ip>", Ctrl+C to exit
```

Other targets: `make build`, `make upload`, `make blobflash`, `make dev`
(upload+monitor), `make erase`, `make compiledb`. Default board is `esp32dev`
(classic ESP32); override with `make upload ENV=c3` for an ESP32-C3, or `PORT=...`.

After the first USB flash, the **blocklist updates over WiFi** from the dashboard
(it is written straight into its flash partition); firmware changes still go over
USB (single-app layout, no firmware OTA).

---

## Blocklist

Built on the host by `tools/build_blocklist.cpp` (compiled by `make blocklist`): it
downloads the default lists - global aggregators **OISD Big** and **Hagezi Pro++**,
the **StevenBlack** base, and a Russian list (**NoADS_RU**) - extracts domains,
hashes them with fxhash to 40 bits, dedups, and writes a v2 bucketed container
(`magic + version + CRC32` header, then a 65536-bucket index + 3-byte suffixes, see
`src/blfmt.h`). Edit `DEFAULT_SOURCES[]` to add/remove lists (any hosts or
AdGuard/ABP `||domain^` source works; cosmetic/regex rules are skipped).

On the device the blob lives in its own raw `blocklist` partition (2.375 MB -> room
for ~800k domains) and is memory-mapped; lookups are a bucket index + a short scan.
A small LittleFS holds runtime state (custom domains, bans, update config).

---

## Why it's fast (architecture)

Flash layout (4 MB, single app slot):

```
nvs        20 KB      WiFi creds (NVS)
app0       1.375 MB   firmware
blocklist  2.375 MB   raw partition: the hash blob, memory-mapped
littlefs   192 KB     runtime state (custom / bans / update config)
```

**Small.** A domain is never stored as text. Each is hashed to a 40-bit value
(fxhash) - that is already just 5 bytes instead of a ~20-char string. Then the hash
is split into a 16-bit prefix and a 24-bit suffix, and the list is grouped into
65536 "buckets" by prefix. The prefix is **not stored**: the bucket a suffix lives
in already encodes it (position = data). Only the 24-bit suffix is written =
**3 bytes/domain**. So ~645k domains take ~2 MB, versus ~2.5 MB of raw text that
also wouldn't fit in RAM to search. Full write-up: `docs/design-blocklist.md`.

**Fast.** A lookup is three cheap steps - no filesystem, no PSRAM, no string compares:

1. `fxhash(domain)` -> a 40-bit value (fast non-crypto hash)
2. the top 16 bits pick one of 65536 buckets via a flat offset table - O(1), no
   binary search
3. scan that bucket's 3-byte suffixes (avg ~10, sorted) for a match

The blob is **memory-mapped** (`esp_partition_mmap`): the CPU reads it straight
through the flash cache like RAM, so steps 2-3 are just a few cache-line reads.
That is the main speed win. The earlier design kept the list as a LittleFS file and
did one random filesystem read per lookup (~1.2 ms); mmap + buckets dropped that to 8 us 150x faster, ~123k lookups/sec.

Modules (each a struct + methods): `blocklist` (hash + mmap lookup), `dns` (server,
forward upstream), `wifi` (connect + captive portal), `web` (dashboard), `clients`
(per-device table). `main.cpp` is just `setup()`/`loop()`.

---

## Notes

- Upstream resolver is Quad9 (`9.9.9.9`); change via the `UPSTREAM_IP` build flag.
- No PSRAM required; works on a bare 4 MB ESP32.
- Security: set real `WEB_PASS` and `AP_PASS` before giving the box to anyone.
