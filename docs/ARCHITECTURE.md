# Architecture

## Goal

Get battery-powered sensor readings from nodes scattered around a building
to a single gateway ("root") node, when some sensor nodes are out of direct
radio range of the gateway, using ESP-NOW (a connectionless, low-latency
802.11 mechanism that needs no AP association) instead of full Wi-Fi/IP
networking.

## Wire format

Every mesh frame is a fixed 9-byte header followed by 0-200 bytes of
payload:

```
byte 0        magic        (0xE5)                  sanity check / frame filter
bytes 1-4     src_id       (uint32, little endian)  originating node's ID (from its MAC)
bytes 5-6     seq          (uint16, little endian)  per-source sequence number
byte 7        ttl          (uint8)                  remaining hop budget
byte 8        payload_len  (uint8)                  bytes of payload that follow
bytes 9..     payload      (payload_len bytes)
```

Implemented in `include/espnow_mesh/mesh.h` / `src/mesh/mesh.c` as
`mesh_encode_header()` / `mesh_decode_header()`. This layer has zero
ESP-IDF or Wi-Fi dependencies -- it operates purely on `uint8_t` buffers --
so it is fully unit-testable on a host machine (see `tests/host/`).

`src_id` is derived from the lower 32 bits of the node's own Wi-Fi station
MAC address (`main.c: esp_read_mac(..., ESP_MAC_WIFI_STA)`), which is
already guaranteed globally unique per chip, so no separate node-ID
provisioning step is needed.

## Why flooding + dedup + TTL, not a real routing protocol

A proper mesh routing protocol (e.g. something AODV/DSR-like, or 802.15.4's
various mesh layers) builds and maintains routing tables, does route
discovery/repair, and generally assumes a long-lived, moderately dense
network worth the bookkeeping overhead.

For a small sensor mesh (single-digit to low tens of nodes, mostly static
placement, traffic pattern is "everything reports toward one gateway"),
that machinery is overkill:

- **No routing tables to build or repair.** Every node just re-broadcasts
  anything it hasn't seen before, as long as its hop budget (TTL) isn't
  exhausted. The "route" to the root emerges implicitly from whichever
  copy of a packet happens to reach the root first via any path -- there's
  no discovery phase, no convergence delay after a node moves or drops
  out.
- **Trivial to reason about and test.** The entire forwarding decision is
  three questions per received packet: (1) is this actually a mesh frame
  (magic byte)? (2) have I already seen this exact (src_id, seq) before? (3)
  does it still have hops left (ttl > 0)? All three are pure functions
  over fixed-size state, which is exactly why they're unit-tested on the
  host in `tests/host/test_mesh.c` without any ESP32 hardware.
- **Memory-bounded by construction.** The dedup cache
  (`mesh_seen_cache_t`) is a fixed `MESH_SEEN_CACHE_SIZE`-entry ring
  buffer, not a growable table -- essential on a microcontroller with
  limited RAM and no dynamic routing state to bound.

The cost of this simplicity is bounded, not eliminated, redundancy:
flooding means every node in range re-broadcasts every new packet once, so
total airtime scales with (number of nodes) x (number of packets), not
just with the length of the eventual path. For the node counts this
project targets (single digits to a few dozen, not hundreds), that's an
acceptable trade for the removed complexity, and TTL caps how far a
runaway broadcast can travel.

## TTL as a loop-breaker, dedup as the real loop-breaker

Two independent mechanisms both limit rebroadcast, for different reasons:

- **TTL** bounds the maximum path length any single packet can travel
  (`mesh_should_forward()` / `mesh_decrement_ttl()` in the portable core).
  It exists mainly so a topology with more hops than expected fails safe
  (packet quietly stops propagating) rather than needing manual tuning per
  deployment.
- **Duplicate suppression** (`mesh_seen_before()` / `mesh_mark_seen()`) is
  what actually prevents broadcast storms in a mesh with cycles (e.g. A and
  C both in range of B, and also of each other): without it, every node
  that hears a packet from more than one neighbor would re-broadcast it
  once per neighbor, forever, even with TTL > 0, since a fresh
  re-broadcast is itself a "new" transmission other nodes will re-forward
  again. Dedup means each (src_id, seq) pair is forwarded by each node
  *at most once*, no matter how many times it's received from different
  neighbors or how cyclic the topology is.

Combined, a packet originated with `MESH_DEFAULT_TTL = 4` can reach any
node within 4 hops, and every node along the way forwards it exactly once,
regardless of topology.

## What this deliberately does NOT provide

- **No delivery acknowledgment / reliability guarantee above L2.** ESP-NOW
  itself has a single-hop link-layer ACK+retry (the sender knows whether
  its immediate radio transmission succeeded, surfaced here via
  `espnow_send_cb`), but that only covers "did my one radio hop go out
  cleanly" -- it says nothing about whether the packet ultimately reached
  the root through however many further hops. There is no end-to-end ACK
  from the root back to the originating leaf, so a leaf node has no way to
  know its reading actually arrived. For sensor telemetry where an
  occasional dropped reading is fine and the next one is along in a few
  seconds anyway, that's an acceptable trade; it would not be acceptable
  for, say, a command-and-control channel needing delivery guarantees.
- **No route optimization.** The "path" a packet takes is whichever copy
  reaches a given node first from any neighbor -- there's no concept of a
  best/shortest path, and duplicate copies via alternate paths are just
  thrown away, not compared or preferred.
- **No security/encryption of the mesh payload** in this reference
  implementation (ESP-NOW itself supports optional CCMP encryption between
  paired peers, not used here since we rely on the broadcast address whose
  peers use `encrypt = false`). Left for a follow-up if this were pointed
  at a real deployment.
