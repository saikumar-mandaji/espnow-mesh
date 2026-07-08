/*
 * espnow_mesh/mesh.h
 *
 * Portable, pure C99 core of the ESP-NOW mesh protocol used by espnow-mesh.
 *
 * This header (and its matching .c file in src/mesh/mesh.c) MUST NOT include
 * any ESP-IDF, FreeRTOS, or Wi-Fi/ESP-NOW headers. It is compiled both:
 *   1. As part of the ESP-IDF firmware application (main/main.c wires it
 *      into the esp_now_register_recv_cb / esp_now_send calls).
 *   2. Standalone on the host, by tests/host/test_mesh.c, using a plain
 *      `cc` toolchain with no cross-compiler and no ESP-IDF installed.
 *
 * Packet format (wire header, MESH_HEADER_SIZE bytes, all fields little
 * endian on the wire regardless of host byte order -- see mesh_encode_header
 * / mesh_decode_header):
 *
 *   byte 0        : magic       (MESH_MAGIC) - cheap sanity check that this
 *                                is actually a mesh packet and not noise
 *                                picked up by the ESP-NOW recv callback.
 *   bytes 1-4     : src_id      (uint32_t) - originating node's stable ID
 *                                (derived from its MAC address).
 *   bytes 5-6     : seq         (uint16_t) - sequence number, incremented
 *                                by the originating node for every new
 *                                packet it sends. Wraps around; combined
 *                                with src_id this forms the dedup key.
 *   byte 7        : ttl         (uint8_t)  - hop budget. Decremented by
 *                                every relay before re-broadcast. A packet
 *                                is NOT forwarded once ttl reaches 0.
 *   byte 8        : payload_len (uint8_t)  - number of payload bytes that
 *                                follow the header in the wire buffer.
 *
 * Total header size: 9 bytes. Payload follows immediately after, up to
 * MESH_MAX_PAYLOAD bytes, giving a worst-case wire frame well under the
 * ESP-NOW 250-byte payload limit.
 */

#ifndef ESPNOW_MESH_MESH_H
#define ESPNOW_MESH_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Wire format constants
 * ------------------------------------------------------------------------ */

/* Arbitrary constant identifying a valid mesh frame. */
#define MESH_MAGIC 0xE5u

/* Fixed on-wire header size in bytes: magic(1) + src_id(4) + seq(2) +
 * ttl(1) + payload_len(1). */
#define MESH_HEADER_SIZE 9u

/* Maximum payload bytes a single mesh frame may carry. Chosen to keep
 * MESH_HEADER_SIZE + MESH_MAX_PAYLOAD comfortably under ESP-NOW's 250-byte
 * hard limit on payload size. */
#define MESH_MAX_PAYLOAD 200u

/* Maximum total wire frame size (header + payload). */
#define MESH_MAX_FRAME (MESH_HEADER_SIZE + MESH_MAX_PAYLOAD)

/* Recommended default hop budget for freshly originated packets. Three
 * physical nodes (leaf -> relay -> root) need a TTL of at least 2 to reach
 * the root; we default to 4 to leave headroom for future larger meshes
 * without a firmware change. */
#define MESH_DEFAULT_TTL 4u

/* Number of (src_id, seq) entries the duplicate-suppression cache holds.
 * This is deliberately small and fixed: this is an embedded system running
 * on an ESP32 with limited RAM, and the cache must never grow unbounded.
 * Once full, the oldest entry is evicted (ring buffer / FIFO policy). */
#define MESH_SEEN_CACHE_SIZE 32u

/* ------------------------------------------------------------------------
 * Header struct (host-side representation, NOT the wire layout)
 * ------------------------------------------------------------------------ */

typedef struct mesh_header {
    uint8_t  magic;
    uint32_t src_id;
    uint16_t seq;
    uint8_t  ttl;
    uint8_t  payload_len;
} mesh_header_t;

/*
 * Encode `hdr` followed by `payload` (payload_len bytes, taken from
 * hdr->payload_len) into `out_buf` in wire format.
 *
 * Returns the total number of bytes written (MESH_HEADER_SIZE +
 * hdr->payload_len) on success, or 0 on failure (NULL args, payload_len
 * too large for MESH_MAX_PAYLOAD, or out_buf_len too small to hold the
 * resulting frame).
 */
size_t mesh_encode_header(const mesh_header_t *hdr,
                           const uint8_t *payload,
                           uint8_t *out_buf,
                           size_t out_buf_len);

/*
 * Decode a wire-format frame in `buf` (buf_len bytes) into `out_hdr`, and
 * set *out_payload to point at the payload bytes within `buf` (no copy).
 *
 * Returns true on success. Returns false if buf_len is too small to hold
 * a header, if the magic byte doesn't match MESH_MAGIC, or if the frame's
 * declared payload_len doesn't fit within the remaining bytes of buf.
 */
bool mesh_decode_header(const uint8_t *buf,
                         size_t buf_len,
                         mesh_header_t *out_hdr,
                         const uint8_t **out_payload);

/* ------------------------------------------------------------------------
 * Duplicate suppression / seen-cache
 * ------------------------------------------------------------------------ */

typedef struct mesh_seen_entry {
    uint32_t src_id;
    uint16_t seq;
    bool     valid;
} mesh_seen_entry_t;

typedef struct mesh_seen_cache {
    mesh_seen_entry_t entries[MESH_SEEN_CACHE_SIZE];
    size_t            next_slot; /* ring-buffer write cursor */
    size_t            count;     /* number of valid entries, <= MESH_SEEN_CACHE_SIZE */
} mesh_seen_cache_t;

/* Initialize (or reset) a seen-cache to empty. */
void mesh_seen_cache_init(mesh_seen_cache_t *cache);

/*
 * Returns true if (src_id, seq) is already present in the cache, i.e. this
 * exact packet has already been observed (originated locally, received, or
 * forwarded) and should NOT be forwarded again.
 */
bool mesh_seen_before(const mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq);

/*
 * Record (src_id, seq) as seen. If the cache is full, the oldest entry
 * (ring-buffer FIFO order) is evicted to make room. If the pair is already
 * present, this is a no-op (avoids duplicate entries burning cache slots).
 */
void mesh_mark_seen(mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq);

/* ------------------------------------------------------------------------
 * TTL / forwarding decision
 * ------------------------------------------------------------------------ */

/*
 * Given the ttl value carried by a just-received packet, returns true if
 * the node is allowed to forward it onward (ttl > 0). A packet arriving
 * with ttl == 0 has exhausted its hop budget and must be dropped, not
 * forwarded.
 */
bool mesh_should_forward(uint8_t ttl);

/*
 * Returns the ttl value to stamp on a forwarded copy of a packet: one less
 * than the received ttl. Caller must have already checked
 * mesh_should_forward() returned true before calling this (i.e. ttl > 0).
 */
uint8_t mesh_decrement_ttl(uint8_t ttl);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_MESH_MESH_H */
