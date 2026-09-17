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

typedef void    (*lgbb_frame_cb_t)(uint16_t from_node, const uint8_t *frame, size_t len);
typedef void    (*lgbb_link_cb_t)(uint16_t node, bool up);
typedef uint8_t (*lgbb_clients_cb_t)(void);

esp_err_t lgbb_init(uint16_t self, uint32_t boot, const uint8_t key[32],
                  lgbb_frame_cb_t on_frame, lgbb_link_cb_t on_link, lgbb_clients_cb_t clients);

/* Call often from the core task: drains received frames, sends HELLO, expires links, services TX. */
void lgbb_poll(uint32_t now_ms);

void    lgbb_send_unicast(uint16_t node, const uint8_t *frame, size_t len);
void    lgbb_flood(uint16_t except_node, const uint8_t *frame, size_t len);
bool    lgbb_is_neighbor(uint16_t node);
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
