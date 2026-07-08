# Bill of Materials & Build/Flash Instructions

## Why 3 boards, not 2

A 2-node setup can only demonstrate a single ESP-NOW hop (A -> B). It
cannot exercise the thing this project is actually about: **multi-hop
forwarding** -- a node relaying a packet it did not originate, decrementing
TTL, and the duplicate-suppression cache preventing that relay from
re-broadcasting a packet it has already seen. That behavior only shows up
with a genuine 3-node chain:

```
Node A (LEAF)  --ESP-NOW-->  Node B (RELAY)  --ESP-NOW-->  Node C (ROOT/gateway)
```

Node A originates sensor readings and forwards anything it overhears in
range. Node B never originates its own readings -- its only job is to
receive from A, decrement TTL, and re-broadcast toward C. Node C is the
gateway: it consumes payloads and does not forward further.

Physically separate A and C far enough apart that they are *not* in each
other's direct radio range, with B positioned in between -- otherwise A's
broadcast reaches C directly and you have not actually exercised the relay
path. (Indoors, a couple of rooms/walls apart is usually enough to force
the multi-hop path with ESP-NOW's typical range.)

## Bill of materials

| Qty | Part | Approx. cost (each) | Notes |
|-----|------|----------------------|-------|
| 3 | **ESP32-DevKitC-32E** dev board | $6-10 | Exact part: Espressif ESP32-DevKitC-32E (WROOM-32E module variant). Any genuine ESP32-DevKitC-32E works; do not substitute ESP32-S2/S3/C3 boards without re-checking `esp_now.h` API compatibility for that target (ESP-NOW is supported on all of them, but this project pins the `esp32` target in `sdkconfig.defaults`). |
| 3 | USB-A to Micro-USB (or USB-C, depending on DevKitC-32E revision) cable | $3-5 | For flashing + serial monitor. One per board; a 4th spare is cheap insurance. |
| 3 | USB power source (PC USB port, wall adapter, or power bank) | -- | Each board just needs 5V/500mA. No shared power bus required -- boards are physically independent. |
| 0 | Any additional wiring / breadboard / sensors | $0 | **None required.** ESP-NOW rides entirely on the onboard Wi-Fi radio already present on the module. This demo does not wire up a real battery/ADC sensor -- `main.c`'s `sensor_task` fabricates a placeholder millivolt reading. Wiring a real battery-voltage divider into an ADC pin is a natural next step but out of scope here. |

Total: **~$20-30** for all 3 boards + cables.

## Flashing each role

All three boards run the exact same firmware source tree; only the
Kconfig `ESPNOW_MESH_ROLE` choice differs per board (see
`main/Kconfig.projbuild`). Flash each board individually, from a host with
ESP-IDF v5.2+ installed and the board connected via USB:

```sh
idf.py set-target esp32
```

### Node A -- LEAF

```sh
idf.py menuconfig
# espnow-mesh Configuration -> Node role -> Leaf (originates sensor data + forwards)
idf.py -p <PORT_A> build flash monitor
```

### Node B -- RELAY

```sh
idf.py menuconfig
# espnow-mesh Configuration -> Node role -> Relay (forwards only)
idf.py -p <PORT_B> build flash monitor
```

### Node C -- ROOT / gateway

```sh
idf.py menuconfig
# espnow-mesh Configuration -> Node role -> Root / gateway (consumes payloads, does not forward)
idf.py -p <PORT_C> build flash monitor
```

If you'd rather avoid the interactive menu, edit `sdkconfig` directly (or
add a small sdkconfig fragment per role) and set exactly one of:

```
CONFIG_ESPNOW_MESH_ROLE_LEAF=y
CONFIG_ESPNOW_MESH_ROLE_RELAY=y
CONFIG_ESPNOW_MESH_ROLE_ROOT=y
```

then rebuild. `<PORT_X>` is the serial device for that board, e.g.
`/dev/ttyUSB0` on Linux or `COM5` on Windows -- run `idf.py -p PORT monitor`
per board in separate terminals to watch all three logs simultaneously
during a bring-up session.

## Expected behavior once all 3 are powered

1. Node A logs `originating packet src=... seq=... ttl=4` roughly every 10
   seconds.
2. Node B logs `recv src=... seq=... ttl=4` then `forwarding ... ttl 4->3`.
3. Node C logs `recv src=... seq=... ttl=3` then `ROOT accepted payload`.
4. If A and C happen to also be in direct range of each other, C may
   additionally receive A's original broadcast directly (ttl=4) -- the
   duplicate-suppression cache means whichever of the two copies (direct
   vs. via B) arrives *second* is silently dropped as a duplicate, so the
   ROOT's application-level log only fires once per (src_id, seq).
