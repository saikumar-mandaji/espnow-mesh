/*
 * espnow_mesh/mesh.c
 *
 * Portable core implementation. Pure C99, no ESP-IDF / Wi-Fi headers.
 * See include/espnow_mesh/mesh.h for the format and API documentation.
 */

#include "espnow_mesh/mesh.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Encode / decode
 * ------------------------------------------------------------------------ */

size_t mesh_encode_header(const mesh_header_t *hdr,
                           const uint8_t *payload,
                           uint8_t *out_buf,
                           size_t out_buf_len)
{
    if (hdr == NULL || out_buf == NULL) {
        return 0;
    }
    if (hdr->payload_len > MESH_MAX_PAYLOAD) {
        return 0;
    }
    if (hdr->payload_len > 0 && payload == NULL) {
        return 0;
    }

    size_t total = (size_t)MESH_HEADER_SIZE + (size_t)hdr->payload_len;
    if (out_buf_len < total) {
        return 0;
    }

    size_t off = 0;

    out_buf[off++] = MESH_MAGIC;

    /* src_id, little endian */
    out_buf[off++] = (uint8_t)(hdr->src_id & 0xFFu);
    out_buf[off++] = (uint8_t)((hdr->src_id >> 8) & 0xFFu);
    out_buf[off++] = (uint8_t)((hdr->src_id >> 16) & 0xFFu);
    out_buf[off++] = (uint8_t)((hdr->src_id >> 24) & 0xFFu);

    /* seq, little endian */
    out_buf[off++] = (uint8_t)(hdr->seq & 0xFFu);
    out_buf[off++] = (uint8_t)((hdr->seq >> 8) & 0xFFu);

    out_buf[off++] = hdr->ttl;
    out_buf[off++] = hdr->payload_len;

    if (hdr->payload_len > 0) {
        memcpy(&out_buf[off], payload, hdr->payload_len);
    }
    off += hdr->payload_len;

    return off;
}

bool mesh_decode_header(const uint8_t *buf,
                         size_t buf_len,
                         mesh_header_t *out_hdr,
                         const uint8_t **out_payload)
{
    if (buf == NULL || out_hdr == NULL || out_payload == NULL) {
        return false;
    }
    if (buf_len < MESH_HEADER_SIZE) {
        return false;
    }
    if (buf[0] != MESH_MAGIC) {
        return false;
    }

    mesh_header_t hdr;
    size_t off = 1;

    hdr.magic = buf[0];

    hdr.src_id = (uint32_t)buf[off] |
                 ((uint32_t)buf[off + 1] << 8) |
                 ((uint32_t)buf[off + 2] << 16) |
                 ((uint32_t)buf[off + 3] << 24);
    off += 4;

    hdr.seq = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
    off += 2;

    hdr.ttl = buf[off++];
    hdr.payload_len = buf[off++];

    if (hdr.payload_len > MESH_MAX_PAYLOAD) {
        return false;
    }
    if ((size_t)(buf_len - off) < (size_t)hdr.payload_len) {
        return false;
    }

    *out_hdr = hdr;
    *out_payload = &buf[off];
    return true;
}

/* ------------------------------------------------------------------------
 * Seen cache (duplicate suppression)
 * ------------------------------------------------------------------------ */

void mesh_seen_cache_init(mesh_seen_cache_t *cache)
{
    if (cache == NULL) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
    cache->next_slot = 0;
    cache->count = 0;
}

bool mesh_seen_before(const mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq)
{
    if (cache == NULL) {
        return false;
    }
    for (size_t i = 0; i < MESH_SEEN_CACHE_SIZE; i++) {
        const mesh_seen_entry_t *e = &cache->entries[i];
        if (e->valid && e->src_id == src_id && e->seq == seq) {
            return true;
        }
    }
    return false;
}

void mesh_mark_seen(mesh_seen_cache_t *cache, uint32_t src_id, uint16_t seq)
{
    if (cache == NULL) {
        return;
    }
    if (mesh_seen_before(cache, src_id, seq)) {
        return; /* already recorded, don't burn another slot */
    }

    /* Ring-buffer FIFO eviction: next_slot always points at the slot to
     * write next, whether it's empty or holds the oldest entry. */
    mesh_seen_entry_t *slot = &cache->entries[cache->next_slot];
    bool was_valid = slot->valid;

    slot->src_id = src_id;
    slot->seq = seq;
    slot->valid = true;

    cache->next_slot = (cache->next_slot + 1) % MESH_SEEN_CACHE_SIZE;

    if (!was_valid && cache->count < MESH_SEEN_CACHE_SIZE) {
        cache->count++;
    }
}

/* ------------------------------------------------------------------------
 * TTL / forwarding decision
 * ------------------------------------------------------------------------ */

bool mesh_should_forward(uint8_t ttl)
{
    return ttl > 0;
}

uint8_t mesh_decrement_ttl(uint8_t ttl)
{
    if (ttl == 0) {
        return 0;
    }
    return (uint8_t)(ttl - 1);
}
