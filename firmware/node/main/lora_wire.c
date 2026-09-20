/* lora_wire.c - the radio-independent half of the LoRa backbone (D71). See lora_wire.h. */
#include "lora_wire.h"

#include <string.h>

/* ---- base64 ---- */

static const char B64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

size_t lora_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    if (in == NULL || out == NULL || len == 0) {
        return 0;
    }
    size_t need = (len + 2u) / 3u * 4u;
    if (need > cap) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        size_t have = len - i;
        v |= have > 1 ? (uint32_t)in[i + 1] << 8 : 0u;
        v |= have > 2 ? (uint32_t)in[i + 2] : 0u;
        out[n++] = B64[(v >> 18) & 0x3Fu];
        out[n++] = B64[(v >> 12) & 0x3Fu];
        out[n++] = have > 1 ? B64[(v >> 6) & 0x3Fu] : '=';
        out[n++] = have > 2 ? B64[v & 0x3Fu] : '=';
    }
    return n;
}

size_t lora_b64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    if (in == NULL || out == NULL || len == 0 || len % 4u != 0u) {
        return 0;
    }
    size_t pad = 0;
    if (in[len - 1] == '=') {
        pad = in[len - 2] == '=' ? 2u : 1u;
    }
    size_t need = len / 4u * 3u - pad;
    if (need == 0 || need > cap) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64_value(in[i]);
        int b = b64_value(in[i + 1]);
        int c = in[i + 2] == '=' ? 0 : b64_value(in[i + 2]);
        int d = in[i + 3] == '=' ? 0 : b64_value(in[i + 3]);
        bool tail = i + 4u == len;
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return 0;
        }
        if (!tail && (in[i + 2] == '=' || in[i + 3] == '=')) {
            return 0;   /* padding anywhere but at the end is not base64 */
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        if (n < need) {
            out[n++] = (uint8_t)(v >> 16);
        }
        if (n < need) {
            out[n++] = (uint8_t)(v >> 8);
        }
        if (n < need) {
            out[n++] = (uint8_t)v;
        }
    }
    return n;
}

/* ---- parts ---- */

size_t lora_part_count(size_t payload_len)
{
    if (payload_len == 0 || payload_len > LORA_PAYLOAD_MAX) {
        return 0;
    }
    return (payload_len + LORA_SLICE_MAX - 1u) / LORA_SLICE_MAX;
}

size_t lora_part_build(uint8_t peer, uint8_t id, uint8_t part, uint8_t of, const uint8_t *slice,
                       size_t slice_len, uint8_t *out, size_t cap)
{
    if (out == NULL || slice == NULL || of == 0u || of > LORA_PARTS_MAX || part >= of ||
        slice_len == 0 || slice_len > LORA_SLICE_MAX || cap < LORA_PART_HDR + slice_len) {
        return 0;
    }
    out[0] = LORA_MAGIC;
    out[1] = peer;
    out[2] = id;
    out[3] = part;
    out[4] = of;
    memcpy(out + LORA_PART_HDR, slice, slice_len);
    return LORA_PART_HDR + slice_len;
}

bool lora_part_parse(const uint8_t *in, size_t len, lora_part_t *out)
{
    if (in == NULL || out == NULL || len <= LORA_PART_HDR || len > LORA_PART_RAW_MAX || in[0] != LORA_MAGIC) {
        return false;
    }
    uint8_t of = in[4];
    uint8_t part = in[3];
    if (of == 0u || of > LORA_PARTS_MAX || part >= of) {
        return false;
    }
    out->peer = in[1];
    out->from = LORA_PEER_INDEX(in[1]);
    out->id = in[2];
    out->part = part;
    out->of = of;
    out->slice = in + LORA_PART_HDR;
    out->slice_len = len - LORA_PART_HDR;
    return true;
}

/* ---- reassembly ---- */

void lora_asm_init(lora_asm_t *a)
{
    memset(a, 0, sizeof(*a));
}

void lora_asm_set_slot(lora_asm_t *a, size_t i, uint8_t *buffer, size_t cap)
{
    if (i < LORA_ASM_SLOTS) {
        a->slots[i].in_use = false;
        a->slots[i].ready = false;
        a->slots[i].data = buffer;
        a->slots[i].cap = buffer != NULL ? cap : 0u;
    }
}

size_t lora_asm_capacity(const lora_asm_t *a)
{
    size_t best = 0;
    for (size_t i = 0; i < LORA_ASM_SLOTS; i++) {
        if (a->slots[i].cap > best) {
            best = a->slots[i].cap;
        }
    }
    return best;
}

void lora_asm_done(lora_asm_t *a, lora_asm_slot_t *slot)
{
    (void)a;
    if (slot != NULL) {
        slot->in_use = false;
        slot->ready = false;
    }
}

uint32_t lora_asm_expire(lora_asm_t *a, uint32_t now_ms)
{
    uint32_t freed = 0;
    for (size_t i = 0; i < LORA_ASM_SLOTS; i++) {
        lora_asm_slot_t *s = &a->slots[i];
        if (!s->in_use || s->ready) {
            continue;   /* a slot handed to the caller is the caller's until it gives it back */
        }
        if (now_ms - s->last_ms > LORA_ASM_TIMEOUT_MS || now_ms - s->started_ms > LORA_ASM_MAX_MS) {
            s->in_use = false;
            a->timeouts++;
            freed++;
        }
    }
    return freed;
}

static bool have_bit(const lora_asm_slot_t *s, uint8_t part)
{
    return (s->have[part / 64u] & (1ULL << (part % 64u))) != 0ULL;
}

static void set_bit(lora_asm_slot_t *s, uint8_t part)
{
    s->have[part / 64u] |= 1ULL << (part % 64u);
}

static bool have_all(const lora_asm_slot_t *s)
{
    for (uint8_t i = 0; i < s->of; i++) {
        if (!have_bit(s, i)) {
            return false;
        }
    }
    return true;
}

lora_asm_result_t lora_asm_feed(lora_asm_t *a, uint32_t now_ms, const uint8_t *part, size_t len,
                                lora_asm_slot_t **out)
{
    *out = NULL;
    lora_part_t p;
    if (!lora_part_parse(part, len, &p)) {
        a->dropped++;
        return LORA_ASM_BAD;
    }
    /* Every part but the last is a full slice, so a part's place in the payload is its number. */
    bool last = p.part + 1u == p.of;
    if (!last && p.slice_len != LORA_SLICE_MAX) {
        a->dropped++;
        return LORA_ASM_BAD;
    }
    /*
     * The largest this set could become. A payload beyond what this AP has room for is refused at
     * its first part, with a reason, rather than after a minute of airtime.
     */
    size_t most = (size_t)p.of * LORA_SLICE_MAX;
    if (most > lora_asm_capacity(a)) {
        a->dropped++;
        return LORA_ASM_OVERSIZE;
    }

    (void)lora_asm_expire(a, now_ms);

    lora_asm_slot_t *s = NULL;
    for (size_t i = 0; i < LORA_ASM_SLOTS; i++) {
        lora_asm_slot_t *c = &a->slots[i];
        if (c->in_use && !c->ready && c->peer == p.peer && c->id == p.id) {
            s = c;
            break;
        }
    }
    if (s == NULL) {
        /* The smallest free slot that holds it, so a two-part frame cannot take the large slot
         * away from a transfer that needs it. */
        for (size_t i = 0; i < LORA_ASM_SLOTS; i++) {
            lora_asm_slot_t *c = &a->slots[i];
            if (!c->in_use && c->cap >= most && (s == NULL || c->cap < s->cap)) {
                s = c;
            }
        }
        if (s == NULL) {
            a->dropped++;
            return LORA_ASM_FULL;
        }
        s->in_use = true;
        s->ready = false;
        s->peer = p.peer;
        s->id = p.id;
        s->of = p.of;
        memset(s->have, 0, sizeof(s->have));
        s->last_len = 0;
        s->total = 0;
        s->started_ms = now_ms;
    }
    if (s->of != p.of) {
        a->dropped++;
        return LORA_ASM_BAD;   /* the same message number with a different part count: not ours */
    }
    if (have_bit(s, p.part)) {
        a->dropped++;
        return LORA_ASM_DUPLICATE;
    }
    memcpy(s->data + (size_t)p.part * LORA_SLICE_MAX, p.slice, p.slice_len);
    set_bit(s, p.part);
    s->last_ms = now_ms;
    if (last) {
        s->last_len = (uint16_t)p.slice_len;
    }
    if (!have_all(s)) {
        return LORA_ASM_NEED_MORE;
    }
    s->total = (uint16_t)((size_t)(s->of - 1u) * LORA_SLICE_MAX + s->last_len);
    s->ready = true;
    *out = s;
    return LORA_ASM_COMPLETE;
}

/* ---- the send policy ---- */

lora_policy_t lora_policy_for_frame(const uint8_t *frame, size_t len)
{
    lg_env_t e;
    if (frame == NULL || len > LORA_PAYLOAD_MAX || lg_env_decode(frame, len, &e) != LG_OK) {
        return LORA_SEND_NEVER;
    }
    if (e.type == LG_T_VOICE) {
        return LORA_SEND_NEVER;   /* one 100 ms frame would be seconds of airtime (D61, D71) */
    }
    if ((e.flags & (LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR)) != 0u) {
        return LORA_SEND_ALWAYS;   /* urgent broadcasts, SOS and all clear go on both radios */
    }
    return LORA_SEND_IF_WIFI_DOWN;
}

bool lora_is_alert_frame(const uint8_t *frame, size_t len)
{
    return lora_policy_for_frame(frame, len) == LORA_SEND_ALWAYS;
}

/* ---- the bounded send queue ---- */

void lora_txq_init(lora_txq_t *q)
{
    memset(q, 0, sizeof(*q));
}

void lora_txq_set_slot(lora_txq_t *q, size_t i, uint8_t *buffer, size_t cap)
{
    if (i < LORA_TXQ_SLOTS) {
        q->slots[i].in_use = false;
        q->slots[i].data = buffer;
        q->slots[i].cap = buffer != NULL ? cap : 0u;
    }
}

size_t lora_txq_capacity(const lora_txq_t *q)
{
    size_t best = 0;
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        if (q->slots[i].cap > best) {
            best = q->slots[i].cap;
        }
    }
    return best;
}

static void txq_recount(lora_txq_t *q)
{
    uint16_t n = 0;
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        n = (uint16_t)(n + (q->slots[i].in_use ? 1u : 0u));
    }
    q->depth = n;
    if (n > q->high) {
        q->high = n;
    }
}

void lora_txq_release(lora_txq_t *q, lora_txq_slot_t *slot);

lora_txq_slot_t *lora_txq_reserve(lora_txq_t *q, uint16_t dest, bool alert, uint8_t id, size_t cap,
                                  uint32_t due_ms)
{
    if (cap == 0 || cap > LORA_PAYLOAD_MAX) {
        return NULL;
    }
    lora_txq_slot_t *slot = NULL;
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {   /* the smallest free slot that holds it */
        lora_txq_slot_t *c = &q->slots[i];
        if (!c->in_use && c->cap >= cap && (slot == NULL || c->cap < slot->cap)) {
            slot = c;
        }
    }
    if (slot == NULL) {
        /* Full: the oldest entry that is neither an alert nor already on the air makes room. */
        for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
            lora_txq_slot_t *c = &q->slots[i];
            if (c->in_use && !c->alert && !c->sending && c->cap >= cap &&
                (slot == NULL || c->order < slot->order)) {
                slot = c;
            }
        }
        if (slot == NULL) {
            q->dropped++;   /* nothing here may be dropped for this one: refuse the newcomer */
            return NULL;
        }
        q->dropped++;
    }
    slot->in_use = true;
    slot->alert = alert;
    slot->sending = false;
    slot->dest = dest;
    slot->len = 0;
    slot->of = 0;
    slot->id = id;
    slot->part = 0;
    slot->order = q->next_order++;
    slot->due_ms = due_ms;
    txq_recount(q);
    return slot;
}

bool lora_txq_commit(lora_txq_t *q, lora_txq_slot_t *slot, size_t len)
{
    size_t of = lora_part_count(len);
    if (slot == NULL) {
        return false;
    }
    if (of == 0u || len > slot->cap) {
        lora_txq_release(q, slot);
        return false;
    }
    slot->len = (uint16_t)len;
    slot->of = (uint8_t)of;
    return true;
}

bool lora_txq_push(lora_txq_t *q, uint16_t dest, bool alert, uint8_t id, const uint8_t *data, size_t len,
                   uint32_t due_ms)
{
    if (data == NULL || len == 0) {
        return false;
    }
    lora_txq_slot_t *slot = lora_txq_reserve(q, dest, alert, id, len, due_ms);
    if (slot == NULL) {
        return false;
    }
    memcpy(slot->data, data, len);
    return lora_txq_commit(q, slot, len);
}

lora_txq_slot_t *lora_txq_peek(lora_txq_t *q, uint32_t now_ms)
{
    lora_txq_slot_t *best = NULL;
    lora_txq_slot_t *sending = NULL;
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        lora_txq_slot_t *c = &q->slots[i];
        if (!c->in_use || c->of == 0u || (int32_t)(now_ms - c->due_ms) < 0) {
            continue;
        }
        if (c->sending) {
            sending = c;
        }
        if (best == NULL) {
            best = c;
        } else if (c->alert != best->alert) {
            best = c->alert ? c : best;
        } else if (c->order < best->order) {
            best = c;
        }
    }
    /*
     * An alert takes the radio from a transfer already in progress; anything else waits for that
     * transfer to finish, so parts never interleave without a reason.
     */
    if (sending != NULL && (best == NULL || !best->alert)) {
        return sending;
    }
    return best;
}

bool lora_txq_alert_due(const lora_txq_t *q, uint32_t now_ms)
{
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        const lora_txq_slot_t *c = &q->slots[i];
        if (c->in_use && c->of != 0u && c->alert && (int32_t)(now_ms - c->due_ms) >= 0) {
            return true;
        }
    }
    return false;
}

void lora_txq_release(lora_txq_t *q, lora_txq_slot_t *slot)
{
    if (slot != NULL) {
        slot->in_use = false;
        slot->sending = false;
        txq_recount(q);
    }
}

/* ---- the module's AT replies ---- */

static bool digits(const char *p, const char *end)
{
    if (p >= end) {
        return false;
    }
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static long signed_number(const char *p, const char *end, bool *ok)
{
    bool neg = p < end && *p == '-';
    if (neg) {
        p++;
    }
    if (!digits(p, end)) {
        *ok = false;
        return 0;
    }
    long v = 0;
    for (; p < end; p++) {
        v = v * 10 + (*p - '0');
    }
    *ok = true;
    return neg ? -v : v;
}

bool lora_at_parse(char *line, lora_at_t *out)
{
    memset(out, 0, sizeof(*out));
    if (line == NULL || line[0] != '+') {
        return false;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (strcmp(line, "+OK") == 0) {
        out->kind = LORA_AT_OK;
        return true;
    }
    if (strcmp(line, "+READY") == 0 || strcmp(line, "+RESET") == 0) {
        out->kind = LORA_AT_READY;
        return true;
    }
    if (strncmp(line, "+ERR=", 5) == 0) {
        bool ok = false;
        long v = signed_number(line + 5, line + len, &ok);
        if (!ok) {
            return false;
        }
        out->kind = LORA_AT_ERR;
        out->err = (int)v;
        return true;
    }
    if (strncmp(line, "+RCV=", 5) == 0) {
        char *p = line + 5;
        char *c1 = strchr(p, ',');
        if (c1 == NULL) {
            return false;
        }
        char *c2 = strchr(c1 + 1, ',');
        if (c2 == NULL) {
            return false;
        }
        bool ok = false;
        long addr = signed_number(p, c1, &ok);
        if (!ok || addr < 0 || addr > 0xFFFF) {
            return false;
        }
        long dlen = signed_number(c1 + 1, c2, &ok);
        if (!ok || dlen < 0) {
            return false;
        }
        char *data = c2 + 1;
        size_t left = (size_t)(line + len - data);
        if ((size_t)dlen > left) {
            return false;   /* truncated line */
        }
        char *after = data + dlen;
        /* The declared length, not a comma count, ends the data: a payload with a comma in it
         * would still be read correctly. Ours never has one; it is base64. */
        if (after >= line + len || *after != ',') {
            return false;
        }
        char *c4 = strchr(after + 1, ',');
        if (c4 == NULL) {
            return false;
        }
        long rssi = signed_number(after + 1, c4, &ok);
        if (!ok) {
            return false;
        }
        long snr = signed_number(c4 + 1, line + len, &ok);
        if (!ok) {
            return false;
        }
        *after = '\0';
        out->kind = LORA_AT_RCV;
        out->addr = (uint16_t)addr;
        out->len = (uint16_t)dlen;
        out->data = data;
        out->rssi = (int)rssi;
        out->snr = (int)snr;
        return true;
    }
    char *eq = strchr(line, '=');
    if (eq == NULL || eq == line + 1) {
        return false;
    }
    *eq = '\0';
    out->kind = LORA_AT_VALUE;
    out->name = line + 1;
    out->value = eq + 1;
    return true;
}

/* ---- airtime ---- */

uint32_t lora_airtime_ms(size_t payload_chars)
{
    /*
     * SF 9, BW 125 kHz, CR 4/5, preamble 12, explicit header, CRC on, low-data-rate optimise off.
     * Symbol time is exactly 4096 us at these settings.
     *   n_payload = 8 + max(ceil((8*PL - 4*SF + 28 + 16) / (4*SF)) * 5, 0)
     *   preamble  = 12 + 4.25 symbols
     */
    const uint32_t sym_us = 4096u;
    long num = (long)(8u * payload_chars) - 4 * 9 + 28 + 16;
    long den = 4 * 9;
    long steps = num > 0 ? (num + den - 1) / den : 0;
    uint32_t n_payload = 8u + (uint32_t)(steps > 0 ? steps * 5 : 0);
    /* Preamble in quarter symbols: 12 * 4 + 17 = 65. */
    uint32_t us = (65u * sym_us) / 4u + n_payload * sym_us;
    return (us + 999u) / 1000u;
}
