/*
 * fw_spread.h - a signed firmware update passed from one access point to the next (D101).
 *
 * "As sticky as the grid time" (D48, D97): an update given to one AP reaches the others without
 * anybody carrying a cable round. An AP that is running an update it took as a signed file can serve
 * that file again - the 192 signed bytes it kept, and the image out of its own running slot - and says
 * so to its neighbours. A neighbour running something older asks for it a piece at a time, hands every
 * byte to lg_fwupdate exactly as the admin page and the console do, and restarts into it when the
 * whole of it has checked out. Then it is a holder too, and offers it on.
 *
 * Nothing here decides whether an update is trusted. lg_fwupdate does, from the signature, on the
 * first 192 bytes, before a byte of the image is asked for: a neighbour is a courier, not an authority,
 * and a stolen AP can offer anything it likes to no effect.
 *
 * Three frames, AP to neighbouring AP only, one hop, never on LoRa (lg_core carries them unread):
 *
 *   FW_OFFER, 16 bytes     0 u8 layout (1)   1 u8 role ('N')   2 u8 chip   3 u8 0
 *                          4 u32 serial      8 u32 file length (192 + image)   12 u32 0
 *   FW_GET, 16 bytes       0 u8 layout (1)   1 u8 role   2 u8 chip   3 u8 0
 *                          4 u32 serial      8 u32 offset into the file   12 u16 bytes wanted   14 u16 0
 *   FW_DATA, 12 + n bytes  0 u8 layout (1)   1..3 0   4 u32 serial   8 u32 offset   12 the bytes,
 *                          n <= FW_SPREAD_CHUNK
 *
 * One AP at a time: a holder serves one neighbour until that one stops asking, so the grid's APs take
 * an update, and restart, in turn and never together. All of it runs on the core task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FW_SPREAD_CHUNK 512u

/* A neighbour's FW_OFFER, FW_GET or FW_DATA (lg_node io.on_fw). Core task. */
void fw_spread_on_frame(uint16_t from_node, uint8_t type, const uint8_t *body, size_t len);

/* A backbone link came up: what this AP holds is offered at the next tick. Core task. */
void fw_spread_on_neighbor_up(uint16_t node);
/* D107: an FW_GET from a handheld with a session here, for the handheld update this AP carries. */
void fw_spread_on_client_get(uint32_t device, const uint8_t *body, size_t len);

/* D111: what a neighbour carries for handhelds, as its offers say. */
typedef struct {
    uint16_t node;
    uint8_t  chip;
    uint32_t serial;
} fw_spread_nb_t;
size_t fw_spread_neighbours(fw_spread_nb_t *out, size_t max);
/* Whether a neighbour carries the handheld update for this chip: if none does, this AP's copy is the grid's only one. */
bool fw_spread_carried_elsewhere(uint8_t chip);
/* Stops carrying the handheld update here and tells every AP to do the same (D111). Returns its serial, 0 if none. */
uint32_t fw_spread_withdraw(void);

/* Every pass of the core loop: offers, the next request, retries, and the restart into a finished
 * update. Core task. */
void fw_spread_tick(uint32_t now_ms);

/* Console: what this AP holds to pass on, and what it is fetching. */
void fw_spread_print(void);
