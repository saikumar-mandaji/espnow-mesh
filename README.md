# espnow-mesh

A multi-hop [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now)
mesh network for battery-powered sensor nodes, built on
[ESP-IDF](https://github.com/espressif/esp-idf) (not Arduino). Sensor
nodes forward each other's packets toward a designated root/gateway node
using hop-count-limited flooding and (source, sequence)-based duplicate
suppression -- no Wi-Fi association, no routing tables, no dynamic route
discovery.

```
Node A (LEAF)  --ESP-NOW-->  Node B (RELAY)  --ESP-NOW-->  Node C (ROOT/gateway)
```

See `docs/ARCHITECTURE.md` for the design rationale and `docs/hardware/BOM.md`
for exactly what hardware this needs (3x ESP32-DevKitC-32E, no extra
wiring) and how to build/flash each role.

## Quick start

### Host tests (portable mesh core -- no ESP32 needed)

```sh
cd tests/host
make test
```

Expects a plain host C compiler (`cc`, gnu99). Exercises header
encode/decode, duplicate suppression, TTL forwarding decisions, and
bounded-memory cache eviction -- see `docs/VERIFICATION.md` for exactly
what is and isn't covered.

### Firmware (requires ESP-IDF v5.2+ and real ESP32-DevKitC-32E hardware)

```sh
idf.py set-target esp32
idf.py menuconfig   # espnow-mesh Configuration -> Node role: Leaf / Relay / Root
idf.py -p <PORT> build flash monitor
```

Flash the LEAF role onto one board, RELAY onto a second, and ROOT onto a
third -- see `docs/hardware/BOM.md` for the full walkthrough and expected
log output.

## Layout

```
include/espnow_mesh/mesh.h   portable mesh header format + dedup + TTL API (pure C99)
src/mesh/mesh.c              portable mesh core implementation (pure C99)
src/mesh/CMakeLists.txt      ESP-IDF component wrapper for the core above
tests/host/test_mesh.c       host unit tests for the portable core
tests/host/Makefile          `make test` -- plain host cc, no ESP-IDF
main/main.c                  ESP-IDF app: ESP-NOW send/recv callbacks, role logic
main/Kconfig.projbuild        per-node role selection (LEAF / RELAY / ROOT)
CMakeLists.txt               top-level ESP-IDF project file
sdkconfig.defaults           default build config (target esp32, role LEAF)
docs/ARCHITECTURE.md         mesh header format + design tradeoffs
docs/VERIFICATION.md         what is / isn't verified, and how
docs/hardware/BOM.md         exact parts list + per-role build/flash steps
```

## API summary (portable core, `include/espnow_mesh/mesh.h`)

```c
size_t mesh_encode_header(const mesh_header_t *hdr, const uint8_t *payload,
                           uint8_t *out_buf, size_t out_buf_len);
bool   mesh_decode_header(const uint8_t *buf, size_t buf_len,
                           mesh_header_t *out_hdr, const uint8_t **out_payload);

void mesh_seen_cache_init(mesh_seen_cache_t *cache);
bool mesh_seen_before(const mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq);
void mesh_mark_seen(mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq);

bool    mesh_should_forward(uint8_t ttl);
uint8_t mesh_decrement_ttl(uint8_t ttl);
```

## Known limitations

- No end-to-end delivery acknowledgment (root back to originating leaf) --
  ESP-NOW's own ACK only covers a single radio hop, see
  `docs/ARCHITECTURE.md`.
- Node role is a compile-time choice (Kconfig), not runtime-switchable.
- No real ESP32 hardware was available while building this repo -- the
  firmware compiles in CI (`idf.py build`) but has not been run on actual
  boards. Full disclosure in `docs/VERIFICATION.md`.
- Sensor reading is a placeholder value, not a real battery ADC sample.

## License

[MIT](LICENSE)
