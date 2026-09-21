/* lora_seal.c - the handheld half of the LoRa radio's sealing (D71, D76). See lora_seal.h. */
#include "lora_seal.h"

#include <string.h>

void lora_seal_init(lora_seal_t *s, const uint8_t key[LG_AEAD_KEY_LEN], uint16_t self, uint32_t boot)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->key, key, LG_AEAD_KEY_LEN);
    s->self = self;
    s->boot = boot;
    lg_dedup_init(&s->replay, s->slots, LORA_SEAL_PEERS);
}

int lora_seal_frame(lora_seal_t *s, uint8_t *out, size_t cap, const uint8_t *inner, size_t len)
{
    if (s == NULL || out == NULL || inner == NULL || len == 0 || len > LG_FRAME_MAX ||
        cap < LORA_SEAL_OUTER_LEN + len + LG_AEAD_TAG_LEN) {
        return -1;
    }
    /*
     * The outer twelve bytes are the nonce and the associated data at once, and the sequence in
     * them comes from this one counter: every frame this device seals takes the next value, so no
     * nonce is ever built twice (see lora_seal.h).
     */
    uint8_t outer[LORA_SEAL_OUTER_LEN];
    outer[0] = LORA_SEAL_VERSION;
    outer[1] = 0;
    lg_wr16(outer + 2, s->self);
    lg_wr32(outer + 4, s->boot);
    lg_wr32(outer + 8, s->seq + 1u);
    int n = lg_aead_seal(s->key, outer, outer, LORA_SEAL_OUTER_LEN, inner, len, out + LORA_SEAL_OUTER_LEN);
    if (n < 0) {
        return -1;   /* the counter has not moved: nothing was put on the air */
    }
    s->seq++;
    memcpy(out, outer, LORA_SEAL_OUTER_LEN);
    return (int)(LORA_SEAL_OUTER_LEN + (size_t)n);
}

lora_seal_result_t lora_seal_open(lora_seal_t *s, const uint8_t *buf, size_t len, uint16_t *src,
                                  uint32_t *boot, uint8_t *out, size_t cap, size_t *out_len)
{
    if (s == NULL || buf == NULL || out == NULL || src == NULL || boot == NULL || out_len == NULL ||
        len <= LORA_SEAL_OUTER_LEN + LG_AEAD_TAG_LEN || buf[0] != LORA_SEAL_VERSION) {
        return LORA_SEAL_MALFORMED;
    }
    uint16_t from = lg_rd16(buf + 2);
    uint32_t from_boot = lg_rd32(buf + 4);
    uint32_t seq = lg_rd32(buf + 8);
    if (from == s->self) {
        return LORA_SEAL_MALFORMED;   /* our own frame coming back: a wiring fault, not a message */
    }
    size_t ct_len = len - LORA_SEAL_OUTER_LEN;
    if (ct_len - LG_AEAD_TAG_LEN > cap) {
        return LORA_SEAL_MALFORMED;
    }
    int n = lg_aead_open(s->key, buf, buf, LORA_SEAL_OUTER_LEN, buf + LORA_SEAL_OUTER_LEN, ct_len, out);
    if (n < 0) {
        s->auth_fail++;
        return LORA_SEAL_AUTH_FAIL;
    }
    /* The replay window is keyed on the sender field, which an AP and a handheld can never share. */
    if (lg_dedup_mark(&s->replay, from, from_boot, seq) != LG_DEDUP_NEW) {
        s->replayed++;
        return LORA_SEAL_REPLAY;
    }
    *src = from;
    *boot = from_boot;
    *out_len = (size_t)n;
    return LORA_SEAL_OK;
}
