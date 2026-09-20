/*
 * backbone.h - node-to-node link over ESP-NOW on the SoftAP interface.
 *
 * Wire format of every ESP-NOW frame:
 *   outer (12 B, clear, AEAD associated data and nonce):
 *     u8 version(1), u8 key epoch(0), u16 sender node, u32 sender boot, u32 frame seq
 *   ChaCha20-Poly1305(backbone key, inner lg frame) || 16-byte tag
 *
 * Neighbors are learned from NODE_HELLO broadcasts every 2 s. A link is usable
 * only after two-way confirmation: the neighbor's HELLO lists this node with
 * its current boot counter, which a replayed HELLO cannot contain.
 * Exactly one ESP-NOW frame is in flight at a time (workaround for the
 * ESP_ERR_ESPNOW_NO_MEM wedge in esp-idf issue 18682).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The clear outer header every sealed backbone frame carries, on any transport. */
#define LGBB_OUTER_LEN  12u

typedef void    (*lgbb_frame_cb_t)(uint16_t from_node, const uint8_t *frame, size_t len);
typedef void    (*lgbb_link_cb_t)(uint16_t node, bool up);
typedef uint8_t (*lgbb_clients_cb_t)(void);

/*
 * Neighbours this AP has over some other transport than ESP-NOW (D71: LoRa), so that the HELLO
 * this AP sends lists them too. Without that a LoRa-only peer could never see itself listed and
 * the link could never be confirmed in either direction. Fills up to max entries and returns how
 * many. Called on the core task while a HELLO is built.
 */
typedef size_t  (*lgbb_extra_peers_cb_t)(uint16_t *nodes, uint32_t *boots, size_t max);
void lgbb_set_extra_peers(lgbb_extra_peers_cb_t cb);

esp_err_t lgbb_init(uint16_t self, uint32_t boot, const uint8_t key[32],
                  lgbb_frame_cb_t on_frame, lgbb_link_cb_t on_link, lgbb_clients_cb_t clients);

/* Call often from the core task: drains received frames, sends HELLO, expires links, services TX. */
void lgbb_poll(uint32_t now_ms);

void    lgbb_send_unicast(uint16_t node, const uint8_t *frame, size_t len);
void    lgbb_flood(uint16_t except_node, const uint8_t *frame, size_t len);
bool    lgbb_is_neighbor(uint16_t node);

/*
 * Whether ESP-NOW is actually carrying traffic to this neighbour right now: the link is confirmed
 * and either its HELLO or a MAC-level ACK of a unicast to it arrived within the link timeout.
 * The second backbone (D71) uses this for "the Wi-Fi link is down or unacknowledged": there is no
 * end-to-end acknowledgement to consult, so a link that has gone quiet counts as unacknowledged.
 */
bool    lgbb_link_acked(uint16_t node, uint32_t now_ms);

/*
 * Sealing and opening for a second transport (D71). Both use the same key, the same outer header
 * and, crucially, the same frame sequence counter as ESP-NOW, so a nonce built from
 * (this AP, boot, sequence) is never used twice whichever radio carried it. Core task only.
 */
/* Returns the sealed length (LGBB_OUTER_LEN + len + 16) or a negative value. */
int     lgbb_seal(uint8_t *out, size_t cap, const uint8_t *inner, size_t len);

typedef enum {
    LGBB_OPEN_OK        = 0,
    LGBB_OPEN_MALFORMED = -1,
    LGBB_OPEN_AUTH_FAIL = -2,
    LGBB_OPEN_REPLAY    = -3,   /* already seen, on either transport: one replay window covers both */
} lgbb_open_t;

/*
 * Opens a sealed frame that arrived on another transport and marks it in the shared replay
 * window. On LGBB_OPEN_OK the inner lg frame is in out, its length in out_len, and the sender's
 * AP index and boot counter in src and boot.
 */
lgbb_open_t lgbb_open(const uint8_t *buf, size_t len, uint16_t *src, uint32_t *boot, uint8_t *out,
                      size_t cap, size_t *out_len);

/*
 * Chaos hook (console only, never the BLE link or the admin page): stop using ESP-NOW for
 * `seconds`, so that anything still crossing between APs went by LoRa. It restores itself when the
 * time is up even if whatever turned it off never came back.
 */
void lgbb_disable(uint32_t seconds);
void lgbb_enable(void);
bool lgbb_is_off(void);

/* Builds this AP's HELLO frame (the same one ESP-NOW broadcasts) for another transport to carry.
 * Returns the frame length or 0. Core task only. */
size_t  lgbb_build_hello(uint8_t *out, size_t cap);
uint8_t lgbb_link_count(void);
uint32_t lgbb_tx_frame_count(void);   /* frames handed to the radio since boot */
void    lgbb_print(void);

typedef struct {
    uint16_t node;
    bool     up;       /* two-way confirmed */
    int      rssi;
    uint32_t age_ms;   /* since the last HELLO */
} lgbb_link_info_t;

/* Copies up to max link entries; returns the number copied. Call from the core task. */
size_t lgbb_links(lgbb_link_info_t *out, size_t max, uint32_t now_ms);

/* D70: per-neighbour frame and byte counts for the BLE admin link's TRAFFIC reply. Counts and
 * sizes only; no frame content and no hardware address (D21). Kept across link loss. */
typedef struct {
    uint16_t node;
    bool     up;
    int8_t   rssi;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t tx_fail;
    uint32_t heard_age_ms;   /* since the last frame or HELLO from it; 0xFFFFFFFF never heard */
} lgbb_link_traffic_t;

/* Copies up to max entries, one per neighbour this AP has ever heard. Core task. */
size_t lgbb_link_traffic(lgbb_link_traffic_t *out, size_t max, uint32_t now_ms);

/* D70: radio-wide counts. */
typedef struct {
    uint32_t tx_frames, tx_fail, tx_dropped, tx_nomem;
    uint32_t rx_frames, rx_auth_fail, rx_replay, rx_queue_full;
    uint8_t  tx_queued;
} lgbb_radio_traffic_t;

void lgbb_radio_traffic(lgbb_radio_traffic_t *out);
