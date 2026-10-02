/*
 * lg_lora_hh.h - sealing LoRa frames between a handheld and an AP (D77).
 *
 * D71 left one question open: which key seals a handheld's LoRa frames, since handhelds do not
 * hold the APs' backbone key. The owner's answer (2026-09-29) is a **separate derived key**,
 * LG_SECRET_LORA_HH_KEY = HKDF-SHA256(backbone key, "lg-lora-hh"), held by handhelds and APs
 * alike. It is not the backbone key, so a handheld image that leaks never exposes AP-to-AP
 * traffic, and an attacker who recovers it can talk to an AP over LoRa but cannot read or forge
 * one byte of the ESP-NOW backbone.
 *
 * Wire format, deliberately the same shape as backbone.h's so one reader can recognise both:
 *
 *   outer (12 B, clear, used as BOTH the AEAD nonce and its associated data):
 *     0  u8   version      LGLH_VERSION
 *     1  u8   key epoch    LGLH_EPOCH_HH: "sealed with the handheld LoRa key, not the backbone key"
 *     2  u16  sender       device index (a handheld) or LG_NODE_ID_BASE-style AP id, see below
 *     4  u32  sender boot  committed to NVS before any transmit, so a nonce never repeats
 *     8  u32  frame seq    per boot, strictly increasing
 *   ChaCha20-Poly1305(handheld LoRa key, inner lg frame) || 16-byte tag
 *
 * The **key epoch byte is what separates the two key domains on the air**. An AP feeds every
 * LoRa payload to lgbb_open first and to this second: byte 1 says which key to try, and a frame
 * sealed with one key can never be opened with the other, so neither domain can be made to
 * authenticate the other's traffic even if an attacker replays it onto the wrong path.
 *
 * The sender field carries a *kind* in its top bit, because a handheld and an AP with the same
 * small index must never share a nonce space:
 *   0x0000 | index   an AP, by AP index
 *   0x8000 | index   a handheld, by device index
 *
 * Nonces never repeat (the project rule): the boot counter is committed to NVS before any radio
 * transmit, and the sequence rises within a boot. A retransmission of a part reuses the identical
 * sealed bytes, so the same plaintext, AAD and nonce go out again exactly as the rule requires.
 *
 * Portable C11 over lg_core and lg_crypto headers only: tests/target exercises all of it with no
 * module fitted, and both firmwares compile the same file.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lg_crypto.h"
#include "lg_dedup.h"
#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LGLH_OUTER_LEN   12u
#define LGLH_VERSION     1u
#define LGLH_EPOCH_HH    1u    /* byte 1: sealed with the handheld LoRa key (backbone frames use 0) */

/* The sender field's kind bit. */
#define LGLH_KIND_AP     0x0000u
#define LGLH_KIND_HH     0x8000u
#define LGLH_KIND(s)     ((uint16_t)((s) & 0x8000u))
#define LGLH_INDEX(s)    ((uint16_t)((s) & 0x7FFFu))
#define LGLH_SENDER_AP(i)  ((uint16_t)(LGLH_KIND_AP | (uint16_t)(i)))
#define LGLH_SENDER_HH(d)  ((uint16_t)(LGLH_KIND_HH | (uint16_t)(d)))

/* The smallest and largest a sealed frame can be. */
#define LGLH_SEALED_MIN  (LGLH_OUTER_LEN + 1u + LG_AEAD_TAG_LEN)

/*
 * How many peers a replay window holds. A handheld talks to at most LG_MAX_NODES APs; an AP hears
 * at most LG_MAX_DEVICES handhelds. One table sized for the larger serves both, and it is a fixed
 * array in the caller's context struct: nothing here allocates.
 */
#define LGLH_PEERS_MAX   LG_MAX_DEVICES

typedef struct {
    uint8_t          key[LG_AEAD_KEY_LEN];
    uint16_t         self;                  /* LGLH_SENDER_HH(device) or LGLH_SENDER_AP(index) */
    uint32_t         boot;
    uint32_t         seq;
    lg_dedup_t       replay;
    lg_dedup_entry_t slots[LGLH_PEERS_MAX];
    /* Counters, for the Status screen and the admin page. */
    uint32_t         sealed;
    uint32_t         opened;
    uint32_t         auth_fail;            /* wrong key, tampered, or another grid's module */
    uint32_t         replayed;
    uint32_t         malformed;
    uint32_t         refused;              /* a message type this path does not carry (see below) */
} lglh_t;

/*
 * Sets up one context. `self` is LGLH_SENDER_HH(device index) on a handheld and
 * LGLH_SENDER_AP(index) on an AP; `boot` must already be committed to NVS.
 */
void lglh_init(lglh_t *c, const uint8_t key[LG_AEAD_KEY_LEN], uint16_t self, uint32_t boot);

/*
 * Seals one inner lg frame. Returns the sealed length (LGLH_OUTER_LEN + len + 16) or -1 on a bad
 * argument or a crypto failure. Single-threaded: one task owns the sequence counter.
 */
int lglh_seal(lglh_t *c, uint8_t *out, size_t cap, const uint8_t *inner, size_t len);

typedef enum {
    LGLH_OPEN_OK = 0,
    LGLH_OPEN_NOT_MINE,      /* not this key domain: byte 0 or 1 is not ours. Try the other reader. */
    LGLH_OPEN_MALFORMED,
    LGLH_OPEN_AUTH_FAIL,
    LGLH_OPEN_REPLAY,
} lglh_open_t;

/*
 * Opens a sealed frame. On LGLH_OPEN_OK *sender is the outer sender field (kind and index) and
 * *out holds the inner frame for *out_len bytes.
 *
 * LGLH_OPEN_NOT_MINE is not an error and is not counted: it is how a reader that is fed every
 * payload tells "this one belongs to the other key" from "this one is broken".
 */
lglh_open_t lglh_open(lglh_t *c, const uint8_t *buf, size_t len, uint16_t *sender, uint8_t *out,
                      size_t cap, size_t *out_len);

/*
 * What a handheld may put on LoRa, and what an AP accepts from one (D77, narrowing D71's
 * "deliberately small"): its position, its presence, an SOS or urgent broadcast, an all clear,
 * and short text. Never voice, never registration, never the shared state, never anything an AP
 * would have to trust a handheld about. An AP calls this on every frame it opens from a handheld
 * and drops what is not on the list, so a stolen handheld key cannot inject grid state.
 *
 * Returns false for a frame this path does not carry, and for one too short to read.
 */
bool lglh_type_allowed(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
