/*
 * main/main.c
 *
 * ESP-IDF application entry point for espnow-mesh.
 *
 * This file is NOT host-testable: it depends on ESP-IDF, FreeRTOS, the
 * esp_wifi and esp_now components, and real Wi-Fi radio hardware. It is
 * exercised only by `idf.py build` (compile-only verification in CI; see
 * .github/workflows/ci.yml) and, ultimately, real ESP32-DevKitC-32E boards.
 * See docs/VERIFICATION.md for the exact split between what is and is not
 * verified in this repository.
 *
 * Responsibilities of this file:
 *   - Bring up Wi-Fi in station mode (required by ESP-NOW even though we
 *     never associate with an AP) and initialize the ESP-NOW component.
 *   - Register esp_now_send_cb / esp_now_recv_cb.
 *   - On receive: decode the mesh header (portable core), check the
 *     duplicate-suppression cache (portable core), check the TTL forwarding
 *     decision (portable core), and if the packet should be forwarded,
 *     decrement its TTL and re-broadcast it. If this node is the root, hand
 *     the payload off to application-level handling (here: just logged).
 *   - Periodically originate a sensor-reading packet (leaf/relay roles).
 *
 * Node role selection: each node needs to know whether it's a LEAF (only
 * originates + forwards), a RELAY (forwards only, does not originate
 * sensor data, though it could), or the ROOT/gateway (does not forward
 * onward -- it's the final destination and simply consumes payloads,
 * e.g. logging or handing them to a backhaul link). This is controlled by
 * a Kconfig choice (see main/Kconfig.projbuild), settable via
 * `idf.py menuconfig` or a sdkconfig fragment, so the exact same source
 * tree builds all three roles without source edits:
 *
 *   idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults menuconfig
 *   -> "espnow-mesh Configuration" -> "Node role" -> Leaf / Relay / Root
 *
 * or non-interactively, e.g.:
 *   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" \
 *          build -- -DCONFIG_ESPNOW_MESH_ROLE_ROOT=y
 */

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "espnow_mesh/mesh.h"

static const char *TAG = "espnow_mesh";

/* Broadcast MAC: ESP-NOW frames sent to this address are received by every
 * node in radio range that also has ESP-NOW initialized, regardless of
 * peer-list pairing -- this is what makes flood-style forwarding possible
 * without pre-establishing a peer for every node in the mesh. */
static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* This node's stable ID, derived from its own base MAC address (lower 32
 * bits are plenty of entropy to avoid collisions across a handful of
 * sensor nodes). */
static uint32_t s_local_node_id;

/* Sequence counter for packets *originated* by this node (as opposed to
 * ones we're merely forwarding on behalf of another node). */
static uint16_t s_local_seq;

/* Portable core's duplicate-suppression cache. One instance per node. */
static mesh_seen_cache_t s_seen_cache;

#if CONFIG_ESPNOW_MESH_ROLE_ROOT
#define NODE_ROLE_STR "ROOT"
#elif CONFIG_ESPNOW_MESH_ROLE_RELAY
#define NODE_ROLE_STR "RELAY"
#elif CONFIG_ESPNOW_MESH_ROLE_LEAF
#define NODE_ROLE_STR "LEAF"
#else
#define NODE_ROLE_STR "LEAF"
#endif

/* True for ROOT nodes: they consume payloads but never re-forward, since
 * they're the final destination (gateway) of the mesh. */
static inline bool node_is_root(void)
{
#if CONFIG_ESPNOW_MESH_ROLE_ROOT
    return true;
#else
    return false;
#endif
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL) {
        return;
    }
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "send to " MACSTR " failed", MAC2STR(mac_addr));
    }
}

/* Broadcast a wire-format mesh frame to all peers in range. */
static void mesh_broadcast(const uint8_t *wire, size_t len)
{
    esp_err_t err = esp_now_send(s_broadcast_mac, wire, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
}

/* Originate a brand-new mesh packet carrying `payload` and broadcast it. */
static void mesh_originate(const uint8_t *payload, uint8_t payload_len)
{
    mesh_header_t hdr = {
        .magic = MESH_MAGIC,
        .src_id = s_local_node_id,
        .seq = s_local_seq++,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = payload_len,
    };

    /* Remember our own (src_id, seq) so that if this broadcast loops back
     * to us via a relay, we recognize it as already-seen and don't
     * re-forward our own packet back out. */
    mesh_mark_seen(&s_seen_cache, hdr.src_id, hdr.seq);

    uint8_t wire[MESH_MAX_FRAME];
    size_t n = mesh_encode_header(&hdr, payload, wire, sizeof(wire));
    if (n == 0) {
        ESP_LOGE(TAG, "mesh_encode_header failed (payload too large?)");
        return;
    }

    ESP_LOGI(TAG, "originating packet src=%08" PRIx32 " seq=%u ttl=%u",
             hdr.src_id, (unsigned)hdr.seq, (unsigned)hdr.ttl);
    mesh_broadcast(wire, n);
}

/* ESP-NOW receive callback: this is where the portable core's forwarding
 * logic is actually wired in. */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (info == NULL || data == NULL || data_len <= 0) {
        return;
    }

    mesh_header_t hdr;
    const uint8_t *payload = NULL;

    if (!mesh_decode_header(data, (size_t)data_len, &hdr, &payload)) {
        /* Not a valid mesh frame (bad magic, truncated, etc) -- ignore.
         * ESP-NOW has no notion of "ports", so any stray broadcast frame
         * from an unrelated ESP-NOW application in radio range will end
         * up here too; the magic byte is our cheap filter. */
        return;
    }

    /* Never re-process/re-forward a packet we've already seen -- this is
     * what turns naive flooding into bounded flooding and prevents
     * broadcast storms / infinite loops in a mesh with cycles. */
    if (mesh_seen_before(&s_seen_cache, hdr.src_id, hdr.seq)) {
        ESP_LOGD(TAG, "dup dropped src=%08" PRIx32 " seq=%u", hdr.src_id, (unsigned)hdr.seq);
        return;
    }
    mesh_mark_seen(&s_seen_cache, hdr.src_id, hdr.seq);

    ESP_LOGI(TAG, "recv src=%08" PRIx32 " seq=%u ttl=%u len=%u",
             hdr.src_id, (unsigned)hdr.seq, (unsigned)hdr.ttl, (unsigned)hdr.payload_len);

    /* Application-level handling of the payload happens on every node
     * that sees the packet (useful for passive sniffing/logging), but only
     * the ROOT is expected to be the "real" consumer / gateway hand-off
     * point in a production deployment. */
    if (node_is_root()) {
        ESP_LOGI(TAG, "ROOT accepted payload (%u bytes) from node %08" PRIx32,
                 (unsigned)hdr.payload_len, hdr.src_id);
        /* TODO(production): hand payload off to a backhaul link (Wi-Fi
         * STA/MQTT, UART, etc). Out of scope for this mesh-forwarding
         * demo. */
        return;
    }

    /* Root nodes are the end of the line and never forward onward. Leaf
     * and relay nodes both forward, decrementing TTL, as long as the
     * hop budget allows it. */
    if (!mesh_should_forward(hdr.ttl)) {
        ESP_LOGD(TAG, "ttl exhausted, dropping src=%08" PRIx32 " seq=%u",
                 hdr.src_id, (unsigned)hdr.seq);
        return;
    }

    mesh_header_t fwd_hdr = hdr;
    fwd_hdr.ttl = mesh_decrement_ttl(hdr.ttl);

    uint8_t wire[MESH_MAX_FRAME];
    size_t n = mesh_encode_header(&fwd_hdr, payload, wire, sizeof(wire));
    if (n == 0) {
        ESP_LOGE(TAG, "mesh_encode_header failed while forwarding");
        return;
    }

    ESP_LOGI(TAG, "forwarding src=%08" PRIx32 " seq=%u ttl %u->%u",
             hdr.src_id, (unsigned)hdr.seq, (unsigned)hdr.ttl, (unsigned)fwd_hdr.ttl);
    mesh_broadcast(wire, n);
}

static void espnow_mesh_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = 0; /* use current Wi-Fi channel */
    peer.ifidx = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* ESP-NOW is connectionless: we deliberately never call
     * esp_wifi_connect(). Station mode is required only so the radio is
     * up and has a MAC/channel; no AP association happens. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
}

static void sensor_task(void *arg)
{
    (void)arg;

    /* Root nodes are pure gateways in this demo: they don't originate
     * sensor readings of their own. */
    if (node_is_root()) {
        vTaskDelete(NULL);
        return;
    }

    uint8_t fake_reading[4];
    for (;;) {
        /* Placeholder "sensor" payload -- a real node would read a battery
         * ADC channel / temperature sensor / etc here. */
        uint32_t millivolts = 3300 + (esp_random() % 400);
        memcpy(fake_reading, &millivolts, sizeof(millivolts));

        mesh_originate(fake_reading, sizeof(fake_reading));

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    s_local_node_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                       ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    s_local_seq = 0;

    mesh_seen_cache_init(&s_seen_cache);
    espnow_mesh_init();

    ESP_LOGI(TAG, "espnow-mesh starting, role=%s node_id=%08" PRIx32, NODE_ROLE_STR, s_local_node_id);

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
