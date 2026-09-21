/*
 * lora_seal.h - sealing for the handheld half of the LoRa radio (D71, D76).
 *
 * AP-to-AP LoRa frames are sealed by backbone.c with the APs' backbone key and are not touched
 * here. Frames between a handheld and an AP are sealed with LG_SECRET_LORA_KEY instead, which both
 * hold and which is derived one way from the backbone key, so a handheld that is lost or taken
 * apart gives up this grid's handheld LoRa traffic and nothing else (D76). The two conversations
 * therefore use different keys, and the part header's peer kind is what picks one (lora_wire.h).
 *
 * The wire format is byte for byte the backbone's, so one reader and one reassembler serve both:
 *
 *   outer (12 B, clear, and both the AEAD nonce and its associated data):
 *     u8 version(1), u8 key epoch(0), u16 sender, u32 sender boot, u32 frame sequence
 *   ChaCha20-Poly1305(LoRa key, inner lg frame) || 16-byte tag
 *
 * Nonces never repeat, which under one shared key has to hold across every device that holds it:
 *
 *   - `sender` separates them. A handheld sends its device number, which is unique in a grid and
 *     is 1..LORA_PEER_INDEX_MAX; an AP sends LORA_SEAL_AP_SENDER | its index. No two devices ever
 *     put the same value there, so no two ever build the same nonce.
 *   - `boot` is the counter each device commits to NVS before it transmits anything.
 *   - `seq` only ever grows within a boot, from one counter in this struct, and every frame this
 *     device seals takes the next value. A part that is retransmitted after a failure carries the
 *     identical sealed bytes, which is a repeated ciphertext and not a repeated nonce.
 *
 * Opening records the sender's (boot, seq) in a sliding window, so a frame replayed on the air is
 * dropped rather than delivered twice.
 *
 * Portable C11 over lg_core and lg_crypto headers: tests/target compiles it directly.
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

#define LORA_SEAL_OUTER_LEN  12u
#define LORA_SEAL_VERSION    1u
/*
 * Set in the sender field of a frame an AP seals for a handheld, so an AP's nonces and a
 * handheld's can never collide under the one key. It is above LG_MAX_DEVICES and above anything a
 * device number can be, and it is not on the air anywhere else.
 */
#define LORA_SEAL_AP_SENDER  0x8000u

/* Room for one window per device in a grid: an AP hears every handheld, a handheld every AP. */
#define LORA_SEAL_PEERS      LG_MAX_DEVICES

typedef struct {
    uint8_t          key[LG_AEAD_KEY_LEN];
    uint16_t         self;       /* device number, or LORA_SEAL_AP_SENDER | AP index */
    uint32_t         boot;
    uint32_t         seq;        /* only ever increases within this boot */
    lg_dedup_entry_t slots[LORA_SEAL_PEERS];
    lg_dedup_t       replay;
    uint32_t         auth_fail;
    uint32_t         replayed;
} lora_seal_t;

typedef enum {
    LORA_SEAL_OK        = 0,
    LORA_SEAL_MALFORMED = -1,
    LORA_SEAL_AUTH_FAIL = -2,
    LORA_SEAL_REPLAY    = -3,
} lora_seal_result_t;

/* self is this device's sender field; boot its NVS boot counter, already committed. */
void lora_seal_init(lora_seal_t *s, const uint8_t key[LG_AEAD_KEY_LEN], uint16_t self, uint32_t boot);

/* Seals one inner lg frame. Returns LORA_SEAL_OUTER_LEN + len + LG_AEAD_TAG_LEN, or a negative
 * value when the arguments or the room do not allow it. Nothing is written on failure. */
int lora_seal_frame(lora_seal_t *s, uint8_t *out, size_t cap, const uint8_t *inner, size_t len);

/*
 * Opens one sealed payload and records it in the replay window. On LORA_SEAL_OK the inner frame is
 * in out for *out_len bytes, and *src and *boot name the sender. A frame this device sealed itself
 * is refused as malformed: hearing our own is a wiring fault, never a message.
 */
lora_seal_result_t lora_seal_open(lora_seal_t *s, const uint8_t *buf, size_t len, uint16_t *src,
                                  uint32_t *boot, uint8_t *out, size_t cap, size_t *out_len);

/* Whether a sender field names an AP rather than a handheld. */
static inline bool lora_seal_is_ap(uint16_t sender)
{
    return (sender & LORA_SEAL_AP_SENDER) != 0u;
}

#ifdef __cplusplus
}
#endif
