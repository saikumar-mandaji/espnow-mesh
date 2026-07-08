/*
 * tests/host/test_mesh.c
 *
 * Host-side unit tests for the portable espnow_mesh core
 * (src/mesh/mesh.c / include/espnow_mesh/mesh.h).
 *
 * Compiled and run entirely on the host with a plain `cc`, no ESP-IDF,
 * no cross toolchain. See tests/host/Makefile.
 */

#include "espnow_mesh/mesh.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                              \
    do {                                               \
        if (cond) {                                    \
            printf("ok  : %s\n", msg);                  \
        } else {                                        \
            printf("FAIL: %s\n", msg);                  \
            g_failures++;                               \
        }                                               \
    } while (0)

/* ------------------------------------------------------------------------
 * Header encode / decode round-trip
 * ------------------------------------------------------------------------ */

static void test_encode_decode_roundtrip(void)
{
    uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };
    mesh_header_t hdr;
    hdr.magic = MESH_MAGIC;
    hdr.src_id = 0x11223344u;
    hdr.seq = 0xABCDu;
    hdr.ttl = 3;
    hdr.payload_len = (uint8_t)sizeof(payload);

    uint8_t wire[MESH_MAX_FRAME];
    size_t n = mesh_encode_header(&hdr, payload, wire, sizeof(wire));

    CHECK(n == MESH_HEADER_SIZE + sizeof(payload), "encode returns header+payload size");

    mesh_header_t decoded;
    const uint8_t *decoded_payload = NULL;
    bool ok = mesh_decode_header(wire, n, &decoded, &decoded_payload);

    CHECK(ok, "decode succeeds on freshly encoded frame");
    CHECK(decoded.magic == MESH_MAGIC, "decoded magic matches");
    CHECK(decoded.src_id == hdr.src_id, "decoded src_id matches");
    CHECK(decoded.seq == hdr.seq, "decoded seq matches");
    CHECK(decoded.ttl == hdr.ttl, "decoded ttl matches");
    CHECK(decoded.payload_len == hdr.payload_len, "decoded payload_len matches");
    CHECK(decoded_payload != NULL && memcmp(decoded_payload, payload, sizeof(payload)) == 0,
          "decoded payload bytes match original");
}

static void test_decode_rejects_bad_magic(void)
{
    uint8_t wire[MESH_HEADER_SIZE] = { 0 };
    wire[0] = 0x00; /* not MESH_MAGIC */
    mesh_header_t decoded;
    const uint8_t *payload = NULL;
    bool ok = mesh_decode_header(wire, sizeof(wire), &decoded, &payload);
    CHECK(!ok, "decode rejects frame with wrong magic byte");
}

static void test_decode_rejects_short_buffer(void)
{
    uint8_t wire[MESH_HEADER_SIZE - 1];
    memset(wire, 0, sizeof(wire));
    mesh_header_t decoded;
    const uint8_t *payload = NULL;
    bool ok = mesh_decode_header(wire, sizeof(wire), &decoded, &payload);
    CHECK(!ok, "decode rejects buffer shorter than header size");
}

static void test_decode_rejects_truncated_payload(void)
{
    uint8_t payload[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    mesh_header_t hdr = { MESH_MAGIC, 42, 7, 5, (uint8_t)sizeof(payload) };
    uint8_t wire[MESH_MAX_FRAME];
    size_t n = mesh_encode_header(&hdr, payload, wire, sizeof(wire));
    CHECK(n > 0, "setup: encode for truncation test succeeds");

    mesh_header_t decoded;
    const uint8_t *decoded_payload = NULL;
    /* Truncate: only give it the header + 3 payload bytes although the
     * header claims 10 payload bytes follow. */
    bool ok = mesh_decode_header(wire, MESH_HEADER_SIZE + 3, &decoded, &decoded_payload);
    CHECK(!ok, "decode rejects frame truncated shorter than declared payload_len");
}

/* ------------------------------------------------------------------------
 * Duplicate suppression
 * ------------------------------------------------------------------------ */

static void test_duplicate_detection(void)
{
    mesh_seen_cache_t cache;
    mesh_seen_cache_init(&cache);

    CHECK(!mesh_seen_before(&cache, 100, 1), "fresh cache: (100,1) not seen yet");

    mesh_mark_seen(&cache, 100, 1);
    CHECK(mesh_seen_before(&cache, 100, 1), "after marking, (100,1) reported as seen (duplicate)");
}

static void test_different_source_not_duplicate(void)
{
    mesh_seen_cache_t cache;
    mesh_seen_cache_init(&cache);

    mesh_mark_seen(&cache, 100, 1);
    CHECK(!mesh_seen_before(&cache, 200, 1),
          "different source id with same seq is NOT treated as duplicate");
}

static void test_different_seq_not_duplicate(void)
{
    mesh_seen_cache_t cache;
    mesh_seen_cache_init(&cache);

    mesh_mark_seen(&cache, 100, 1);
    CHECK(!mesh_seen_before(&cache, 100, 2),
          "same source id with different seq is NOT treated as duplicate");
}

/* ------------------------------------------------------------------------
 * TTL / forwarding decision
 * ------------------------------------------------------------------------ */

static void test_ttl_zero_should_not_forward(void)
{
    CHECK(!mesh_should_forward(0), "ttl == 0 must NOT be forwarded");
    CHECK(mesh_should_forward(1), "ttl == 1 (last hop) should still be forwarded");
    CHECK(mesh_should_forward(MESH_DEFAULT_TTL), "fresh default ttl should be forwarded");
}

static void test_ttl_decrement(void)
{
    CHECK(mesh_decrement_ttl(3) == 2, "decrementing ttl 3 gives 2");
    CHECK(mesh_decrement_ttl(1) == 0, "decrementing ttl 1 gives 0 (next hop must drop)");
    CHECK(mesh_decrement_ttl(0) == 0, "decrementing ttl 0 stays clamped at 0");
}

/* ------------------------------------------------------------------------
 * Cache eviction behavior once full (bounded memory)
 * ------------------------------------------------------------------------ */

static void test_cache_eviction_when_full(void)
{
    mesh_seen_cache_t cache;
    mesh_seen_cache_init(&cache);

    /* Fill the cache completely with distinct (src_id, seq) pairs, using
     * src_id as a monotonically increasing counter so each entry is
     * unique. */
    for (uint32_t i = 0; i < MESH_SEEN_CACHE_SIZE; i++) {
        mesh_mark_seen(&cache, i, 0);
    }
    CHECK(cache.count == MESH_SEEN_CACHE_SIZE, "cache count saturates at MESH_SEEN_CACHE_SIZE");

    for (uint32_t i = 0; i < MESH_SEEN_CACHE_SIZE; i++) {
        if (!mesh_seen_before(&cache, i, 0)) {
            CHECK(0, "every one of the first N entries should still be present before eviction");
            break;
        }
    }

    /* Insert one more distinct entry -- this must evict the OLDEST entry
     * (src_id == 0, since it was inserted first / FIFO ring buffer), not
     * grow the cache beyond its fixed bound. */
    mesh_mark_seen(&cache, MESH_SEEN_CACHE_SIZE, 0);

    CHECK(cache.count == MESH_SEEN_CACHE_SIZE,
          "cache count stays bounded at MESH_SEEN_CACHE_SIZE after overflow insert (no unbounded growth)");
    CHECK(!mesh_seen_before(&cache, 0, 0),
          "oldest entry (src_id=0) was evicted to make room for the new one");
    CHECK(mesh_seen_before(&cache, MESH_SEEN_CACHE_SIZE, 0),
          "newly inserted entry after overflow is present");
    CHECK(mesh_seen_before(&cache, 1, 0),
          "second-oldest entry (src_id=1) survives the single eviction");
}

static void test_mark_seen_duplicate_is_noop(void)
{
    mesh_seen_cache_t cache;
    mesh_seen_cache_init(&cache);

    mesh_mark_seen(&cache, 5, 5);
    size_t count_after_first = cache.count;
    mesh_mark_seen(&cache, 5, 5); /* mark the same pair again */
    CHECK(cache.count == count_after_first,
          "re-marking an already-seen pair does not consume another cache slot");
}

int main(void)
{
    test_encode_decode_roundtrip();
    test_decode_rejects_bad_magic();
    test_decode_rejects_short_buffer();
    test_decode_rejects_truncated_payload();

    test_duplicate_detection();
    test_different_source_not_duplicate();
    test_different_seq_not_duplicate();

    test_ttl_zero_should_not_forward();
    test_ttl_decrement();

    test_cache_eviction_when_full();
    test_mark_seen_duplicate_is_noop();

    if (g_failures == 0) {
        printf("ALL HOST TESTS PASSED\n");
        return 0;
    } else {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
}
