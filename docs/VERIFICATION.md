# Verification status

Honest accounting of what has actually been checked in this repository, as
opposed to what is merely expected to work.

## What IS verified

- **Mesh header encode/decode round-trip** (`tests/host/test_mesh.c`):
  encoding a header + payload and decoding it back reproduces every field
  exactly (magic, src_id, seq, ttl, payload_len) and the payload bytes.
- **Malformed-frame rejection**: decode correctly rejects a frame with the
  wrong magic byte, a buffer shorter than the fixed header size, and a
  frame whose declared `payload_len` doesn't fit in the bytes actually
  available (truncated frame).
- **Duplicate detection**: marking `(src_id, seq)` as seen causes a
  subsequent `mesh_seen_before()` check for the exact same pair to report
  true (duplicate); a different `src_id` with the same `seq`, or the same
  `src_id` with a different `seq`, is correctly NOT flagged as a duplicate.
- **TTL forwarding decision**: `ttl == 0` correctly yields
  "do not forward"; `ttl >= 1` yields "forward"; decrementing ttl behaves
  correctly at the boundary (1 -> 0, and 0 stays clamped at 0 rather than
  wrapping to 255).
- **Bounded-memory cache eviction**: filling the seen-cache to its fixed
  `MESH_SEEN_CACHE_SIZE` capacity and inserting one more entry evicts
  exactly the oldest entry (FIFO/ring-buffer order), keeps `cache->count`
  clamped at the fixed capacity, and preserves every other still-live
  entry -- i.e. the cache provably never grows unbounded, and old entries
  age out.
- All of the above run as `tests/host/test_mesh.c`, compiled with a plain
  host `cc -std=gnu99 -O0 -g -Wall -Wextra -Wpedantic` against
  `src/mesh/mesh.c` directly (see `tests/host/Makefile`) -- no ESP-IDF, no
  cross-compiler, no simulator involved. This is exercised automatically
  by CI's `host-tests` job on every push (`.github/workflows/ci.yml`).

### A note on how these tests were run in this dev environment

This project was authored in a Windows development environment with
**no local C compiler and no WSL Linux distribution installed** (`cc`,
`gcc`, and `wsl.exe -e bash` were all unavailable at the time of writing).
The host tests were written to the same rigor as the rest of this
portfolio's projects and are believed correct by inspection and by design
(they were the actual design spec driving `mesh.c`'s implementation), but
they could not be executed locally before the first push. The
authoritative first real execution of `make -C tests/host test` is CI's
`host-tests` job on `ubuntu-latest` (which does have `gcc`), and that run
is what should be checked/linked as the actual evidence these tests pass
-- see the repository's Actions tab. If that run is not green, treat the
"what IS verified" section above as aspirational until it is.

## What is NOT verified

- **The ESP-IDF application itself is not host-testable.** `main/main.c`
  depends on `esp_wifi.h`, `esp_now.h`, `nvs_flash.h`, and FreeRTOS -- none
  of which exist off-target. CI's `firmware-build` job only proves the
  firmware *compiles* against a real ESP-IDF toolchain (`idf.py set-target
  esp32 && idf.py build`); it does not run the resulting binary anywhere.
- **No real ESP32 hardware was available in this development environment**
  to flash and run any of the three node roles (LEAF/RELAY/ROOT).
  Consequently, none of the following have been observed on real
  hardware:
  - That `esp_now_send`/`esp_now_register_recv_cb` actually deliver frames
    between real boards at all.
  - That the 3-node LEAF -> RELAY -> ROOT topology described in
    `docs/hardware/BOM.md` actually requires and exercises the relay hop
    (as opposed to A and C being in unintentional direct range of each
    other).
  - Real-world ESP-NOW range, latency, and packet-loss characteristics.
  - Behavior under real RF interference, multiple concurrent originators,
    or a node power-cycling mid-mesh.
  - Actual power/battery behavior of the placeholder `sensor_task` duty
    cycle (`vTaskDelay(10000ms)` with Wi-Fi modem sleep disabled per
    `sdkconfig.defaults` -- this is intentionally NOT power-optimized in
    this reference build).
- **No fuzz/property-based testing** of `mesh_decode_header()` against
  arbitrary/adversarial byte sequences beyond the specific malformed-frame
  cases enumerated above -- a reasonable follow-up given this function
  parses attacker-reachable input off the air.

## Known limitations

- Node role (LEAF/RELAY/ROOT) is a compile-time Kconfig choice, not
  runtime-configurable or auto-elected -- swapping a board's role requires
  reflashing.
- `sensor_task`'s reading is a fabricated placeholder value, not a real
  ADC-sampled battery voltage (see `docs/hardware/BOM.md` -- no sensor
  wiring is part of this BOM).
- No end-to-end delivery acknowledgment from ROOT back to the originating
  LEAF (see `docs/ARCHITECTURE.md` for the reasoning) -- a leaf cannot
  detect that its own reading was dropped somewhere in the mesh.
- `src_id` collisions are only as unlikely as two nodes sharing the lower
  32 bits of their Wi-Fi MAC, which is astronomically unlikely at the
  node counts this project targets but is not a cryptographic guarantee.
- The seen-cache is sized (`MESH_SEEN_CACHE_SIZE = 32`) for a small mesh
  with a slow-ish reporting interval; a much larger/busier mesh could see
  legitimate old-but-still-in-flight duplicates evicted before a genuine
  duplicate arrives, causing (harmless, just wasteful) re-forwarding of a
  packet that should have been deduped. Tune the constant if scaling this
  up.
