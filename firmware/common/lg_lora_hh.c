/* lg_lora_hh.c - sealing LoRa frames between a handheld and an AP (D77). See lg_lora_hh.h. */
#include "lg_lora_hh.h"

#include <string.h>

#include "lg_envelope.h"

void lglh_init(lglh_t *c, const uint8_t key[LG_AEAD_KEY_LEN], uint16_t self, uint32_t boot)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    if (key != NULL) {
        memcpy(c->key, key, sizeof(c->key));
    }
    c->self = self;
    c->boot = boot;
    lg_dedup_init(&c->replay, c->slots, LGLH_PEERS_MAX);
}

int lglh_seal(lglh_t *c, uint8_t *out, size_t cap, const uint8_t *inner, size_t len)
{
    if (c == NULL || out == NULL || inner == NULL || len == 0u || len > LG_FRAME_MAX ||
        cap < LGLH_OUTER_LEN + len + LG_AEAD_TAG_LEN) {
        return -1;
    }
    out[0] = LGLH_VERSION;
    out[1] = LGLH_EPOCH_HH;
    lg_wr16(out + 2, c->self);
    lg_wr32(out + 4, c->boot);
    lg_wr32(out + 8, ++c->seq);
    /* The outer header is both the nonce and the associated data, exactly as on the backbone. */
    int n = lg_aead_seal(c->key, out, out, LGLH_OUTER_LEN, inner, len, out + LGLH_OUTER_LEN);
    if (n < 0) {
        return -1;
    }
    c->sealed++;
    return (int)(LGLH_OUTER_LEN + (size_t)n);
}

lglh_open_t lglh_open(lglh_t *c, const uint8_t *buf, size_t len, uint16_t *sender, uint8_t *out,
                      size_t cap, size_t *out_len)
{
    if (c == NULL || buf == NULL || out == NULL || len < LGLH_SEALED_MIN) {
        if (c != NULL) {
            c->malformed++;
        }
        return LGLH_OPEN_MALFORMED;
    }
    /*
     * Which key domain is this? Answered before a single byte is authenticated, and without
     * counting anything: an AP feeds every LoRa payload to both readers, and a backbone frame
     * arriving here is normal traffic, not a fault.
     */
    if (buf[0] != LGLH_VERSION || buf[1] != LGLH_EPOCH_HH) {
        return LGLH_OPEN_NOT_MINE;
    }
    uint16_t from = lg_rd16(buf + 2);
    uint32_t from_boot = lg_rd32(buf + 4);
    uint32_t seq = lg_rd32(buf + 8);
    if (from == c->self) {
        c->malformed++;   /* our own frame back: another module on this address, or a loop */
        return LGLH_OPEN_MALFORMED;
    }
    uint16_t index = LGLH_INDEX(from);
    if (LGLH_KIND(from) == LGLH_KIND_HH ? index >= LG_MAX_DEVICES : index >= LG_MAX_NODES) {
        c->malformed++;
        return LGLH_OPEN_MALFORMED;
    }
    size_t ct_len = len - LGLH_OUTER_LEN;
    if (ct_len - LG_AEAD_TAG_LEN > cap) {
        c->malformed++;
        return LGLH_OPEN_MALFORMED;
    }
    int n = lg_aead_open(c->key, buf, buf, LGLH_OUTER_LEN, buf + LGLH_OUTER_LEN, ct_len, out);
    if (n < 0) {
        c->auth_fail++;
        return LGLH_OPEN_AUTH_FAIL;
    }
    /*
     * The replay window is keyed on the whole sender field, kind bit included, so handheld 3 and
     * AP 3 keep separate sequence spaces and neither can retire the other's frames.
     */
    if (lg_dedup_mark(&c->replay, (uint32_t)from, from_boot, seq) != LG_DEDUP_NEW) {
        c->replayed++;
        return LGLH_OPEN_REPLAY;
    }
    if (sender != NULL) {
        *sender = from;
    }
    if (out_len != NULL) {
        *out_len = (size_t)n;
    }
    c->opened++;
    return LGLH_OPEN_OK;
}

bool lglh_type_allowed(const uint8_t *frame, size_t len)
{
    lg_env_t e;
    if (frame == NULL || lg_env_decode(frame, len, &e) != LG_OK) {
        return false;
    }
    switch (e.type) {
    case LG_T_POSITION:         /* where the sender is: the emergency case this radio exists for */
    case LG_T_PRESENCE_UPDATE:  /* that they are alive and which AP they were last on */
    case LG_T_TEXT:             /* short text, including an SOS or urgent broadcast and an all clear */
    case LG_T_MSG_ACK:          /* so a sender learns their message landed */
    /*
     * The announcement pair (D80). A handheld sends PING on a timer so an AP knows it is reachable
     * this way *before* it has something urgent to say, and the AP's PONG is how the handheld learns
     * the path works in both directions. Both are the smallest frames in the protocol.
     *
     * Admitting them asserts nothing about the grid: an AP answering a PING records only that
     * handheld's own battery and replies. That is the test every type on this list has to pass --
     * the refusals below are the ones where an AP would have to *trust a handheld*, and one stolen
     * handheld holds the key that seals all of this.
     */
    case LG_T_PING:
    case LG_T_PONG:
        return true;
    default:
        /*
         * Everything else is refused, and the list above is the whole allowance. Voice (LG_T_VOICE)
         * could never fit the airtime (D71). Registration, the shared state (LG_T_GRID_STATE),
         * groups and the time are things an AP would have to *trust a handheld about*: accepting
         * them over a path sealed by a key that every handheld holds would let one stolen handheld
         * rewrite the grid. They travel over Wi-Fi, where a handheld has a session, or not at all.
         */
        return false;
    }
}
