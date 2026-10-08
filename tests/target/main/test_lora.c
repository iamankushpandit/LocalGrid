/*
 * test_lora.c - the LoRa backbone (D71) without a radio.
 *
 * Everything here runs on a board with no module fitted, which is the point: the part framing,
 * reassembly of short frames and of a long payload, base64 at its size limits, the queue rules
 * that must never drop an alert, never make one wait and never carry live voice, and the AT lines
 * the RYLR998 actually prints. What a real module does with those lines is a bench question; what
 * this firmware does with them is settled here.
 */
#include <string.h>

#include "lg_body.h"
#include "lg_envelope.h"
#include "lg_lora_hh.h"
#include "lg_test.h"
#include "lg_lora_wire.h"

#define PEER_ONE  (LORA_PEER_AP | 1u)
#define PEER_TWO  (LORA_PEER_AP | 2u)

/* ---- base64 ---- */

static void test_base64(void)
{
    char out[LORA_PART_B64_MAX + 8];
    uint8_t back[LORA_PART_RAW_MAX + 8];

    /* RFC 4648 vectors, so a wrong table shows up as a wrong string, not as a round trip. */
    CHECK_EQ(lora_b64_encode((const uint8_t *)"f", 1, out, sizeof(out)), 4);
    CHECK(memcmp(out, "Zg==", 4) == 0);
    CHECK_EQ(lora_b64_encode((const uint8_t *)"foobar", 6, out, sizeof(out)), 8);
    CHECK(memcmp(out, "Zm9vYmFy", 8) == 0);
    CHECK_EQ(lora_b64_decode("Zm9vYmFy", 8, back, sizeof(back)), 6);
    CHECK(memcmp(back, "foobar", 6) == 0);

    /* Every length up to a full part round-trips, and a full part is exactly the characters
     * one AT+SEND carries. */
    static uint8_t in[LORA_PART_RAW_MAX];
    for (size_t i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)(i * 7u + 3u);
    }
    for (size_t len = 1; len <= LORA_PART_RAW_MAX; len++) {
        size_t n = lora_b64_encode(in, len, out, sizeof(out));
        CHECK_EQ(n, (len + 2u) / 3u * 4u);
        CHECK(n <= LORA_PART_B64_MAX);
        size_t m = lora_b64_decode(out, n, back, sizeof(back));
        CHECK_EQ(m, len);
        CHECK(memcmp(back, in, len) == 0);
    }
    CHECK_EQ(lora_b64_encode(in, LORA_PART_RAW_MAX, out, sizeof(out)), LORA_PART_B64_MAX);

    /* No room, nothing written. */
    CHECK_EQ(lora_b64_encode(in, 10, out, 8), 0);
    CHECK_EQ(lora_b64_decode("Zm9vYmFy", 8, back, 3), 0);
    /* Not base64. */
    CHECK_EQ(lora_b64_decode("Zm9v", 3, back, sizeof(back)), 0);      /* length not a multiple of 4 */
    CHECK_EQ(lora_b64_decode("Zm9*", 4, back, sizeof(back)), 0);      /* a character outside the set */
    CHECK_EQ(lora_b64_decode("Z=9vYmFy", 8, back, sizeof(back)), 0);  /* padding before the end */
    CHECK_EQ(lora_b64_decode("", 0, back, sizeof(back)), 0);
}

/* ---- parts ---- */

static size_t build(uint8_t *out, size_t cap, uint8_t peer, uint8_t id, uint8_t part, uint8_t of,
                    const uint8_t *payload, size_t payload_len)
{
    size_t off = (size_t)part * LORA_SLICE_MAX;
    size_t slice = payload_len - off > LORA_SLICE_MAX ? LORA_SLICE_MAX : payload_len - off;
    return lora_part_build(peer, id, part, of, payload + off, slice, out, cap);
}

static void test_parts(void)
{
    static uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i ^ 0x5Au);
    }
    CHECK_EQ(lora_part_count(1), 1);
    CHECK_EQ(lora_part_count(LORA_SLICE_MAX), 1);
    CHECK_EQ(lora_part_count(LORA_SLICE_MAX + 1u), 2);
    CHECK_EQ(lora_part_count(300), (300u + LORA_SLICE_MAX - 1u) / LORA_SLICE_MAX);
    /* The owner's 6 KB ceiling, rounded up to whole parts, and one byte past it. */
    CHECK_EQ(LORA_PAYLOAD_MAX, LORA_PARTS_MAX * LORA_SLICE_MAX);
    CHECK(LORA_PAYLOAD_MAX >= 6144);
    CHECK_EQ(lora_part_count(LORA_PAYLOAD_MAX), LORA_PARTS_MAX);
    CHECK(lora_part_count(LORA_PAYLOAD_MAX) <= LORA_PARTS_MAX);
    CHECK_EQ(lora_part_count(LORA_PAYLOAD_MAX + 1u), 0);   /* refused, never cut short */
    CHECK_EQ(lora_part_count(0), 0);

    uint8_t part[LORA_PART_RAW_MAX];
    size_t n = build(part, sizeof(part), PEER_TWO, 7, 1, 3, payload, sizeof(payload));
    CHECK_EQ(n, LORA_PART_HDR + LORA_SLICE_MAX);
    CHECK_EQ(part[0], LORA_MAGIC);
    lora_part_t p;
    CHECK(lora_part_parse(part, n, &p));
    CHECK_EQ(p.peer, PEER_TWO);
    CHECK_EQ(p.from, 2);
    CHECK_EQ(LORA_PEER_KIND(p.peer), LORA_PEER_AP);
    CHECK_EQ(p.id, 7);
    CHECK_EQ(p.part, 1);
    CHECK_EQ(p.of, 3);
    CHECK_EQ(p.slice_len, LORA_SLICE_MAX);
    CHECK(memcmp(p.slice, payload + LORA_SLICE_MAX, LORA_SLICE_MAX) == 0);

    /* A handheld's part parses as a handheld's, so the format is ready for one (docs/lora.md). */
    n = build(part, sizeof(part), (uint8_t)(LORA_PEER_HANDHELD | 3u), 1, 0, 1, payload, 40);
    CHECK(lora_part_parse(part, n, &p));
    CHECK_EQ(LORA_PEER_KIND(p.peer), LORA_PEER_HANDHELD);
    CHECK_EQ(p.from, 3);

    /* Refused arguments leave nothing behind. */
    CHECK_EQ(lora_part_build(PEER_ONE, 0, 2, 2, payload, 4, part, sizeof(part)), 0);    /* part >= of */
    CHECK_EQ(lora_part_build(PEER_ONE, 0, 0, 0, payload, 4, part, sizeof(part)), 0);
    CHECK_EQ(lora_part_build(PEER_ONE, 0, 0, (uint8_t)(LORA_PARTS_MAX + 1u), payload, 4, part, sizeof(part)), 0);
    CHECK_EQ(lora_part_build(PEER_ONE, 0, 0, 1, payload, LORA_SLICE_MAX + 1u, part, sizeof(part)), 0);
    CHECK_EQ(lora_part_build(PEER_ONE, 0, 0, 1, payload, 8, part, 8), 0);               /* no room */

    /* Rubbish is not a part. */
    n = build(part, sizeof(part), PEER_ONE, 0, 0, 1, payload, 20);
    CHECK(lora_part_parse(part, n, &p));
    part[0] = 'X';
    CHECK(!lora_part_parse(part, n, &p));
    part[0] = LORA_MAGIC;
    part[4] = LORA_PARTS_MAX + 1u;
    CHECK(!lora_part_parse(part, n, &p));
    part[4] = 1u;
    CHECK(!lora_part_parse(part, LORA_PART_HDR, &p));   /* a header with no slice */
}

/* ---- reassembly ---- */

/* Two small slots and one large, exactly as an AP with a module gives it. */
static uint8_t s_small[2][LORA_SMALL_MAX];
static uint8_t s_large[LORA_PAYLOAD_MAX];

static void asm_setup(lora_asm_t *a, bool with_large)
{
    lora_asm_init(a);
    lora_asm_set_slot(a, 0, s_small[0], sizeof(s_small[0]));
    lora_asm_set_slot(a, 1, s_small[1], sizeof(s_small[1]));
    lora_asm_set_slot(a, 2, with_large ? s_large : NULL, sizeof(s_large));
}

static void test_reassembly(void)
{
    static lora_asm_t a;
    static uint8_t payload[300];
    uint8_t part[LORA_PART_RAW_MAX];
    lora_asm_slot_t *slot = NULL;

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 3u + 1u);
    }
    asm_setup(&a, true);

    /* One part is a whole payload. */
    size_t n = build(part, sizeof(part), PEER_ONE, 1, 0, 1, payload, 40);
    CHECK_EQ(lora_asm_feed(&a, 1000, part, n, &slot), LORA_ASM_COMPLETE);
    CHECK(slot != NULL && slot->total == 40);
    CHECK(memcmp(slot->data, payload, 40) == 0);
    lora_asm_done(&a, slot);

    /* Three parts, out of order, with one arriving twice. */
    const size_t len3 = 3u * LORA_SLICE_MAX - 10u;   /* whatever the slice size, three parts */
    n = build(part, sizeof(part), PEER_ONE, 2, 2, 3, payload, len3);
    CHECK_EQ(lora_asm_feed(&a, 1000, part, n, &slot), LORA_ASM_NEED_MORE);
    n = build(part, sizeof(part), PEER_ONE, 2, 0, 3, payload, len3);
    CHECK_EQ(lora_asm_feed(&a, 1100, part, n, &slot), LORA_ASM_NEED_MORE);
    CHECK_EQ(lora_asm_feed(&a, 1150, part, n, &slot), LORA_ASM_DUPLICATE);
    n = build(part, sizeof(part), PEER_ONE, 2, 1, 3, payload, len3);
    CHECK_EQ(lora_asm_feed(&a, 1200, part, n, &slot), LORA_ASM_COMPLETE);
    CHECK(slot != NULL && slot->total == len3);
    CHECK(memcmp(slot->data, payload, len3) == 0);
    lora_asm_done(&a, slot);

    /* A missing part is given up after the idle timeout and never half delivered. */
    asm_setup(&a, true);
    n = build(part, sizeof(part), PEER_ONE, 3, 0, 3, payload, len3);
    CHECK_EQ(lora_asm_feed(&a, 10000, part, n, &slot), LORA_ASM_NEED_MORE);
    CHECK_EQ(lora_asm_expire(&a, 10000 + LORA_ASM_TIMEOUT_MS), 0);
    CHECK_EQ(lora_asm_expire(&a, 10001 + LORA_ASM_TIMEOUT_MS), 1);
    CHECK_EQ(a.timeouts, 1);

    /* A long payload: every part restarts the ten seconds, so every part at a second each finishes. */
    static uint8_t big[LORA_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 5u + 9u);
    }
    asm_setup(&a, true);
    size_t of = lora_part_count(sizeof(big));
    CHECK_EQ(of, LORA_PARTS_MAX);
    lora_asm_result_t r = LORA_ASM_NEED_MORE;
    for (size_t i = 0; i < of; i++) {
        n = build(part, sizeof(part), PEER_ONE, 11, (uint8_t)i, (uint8_t)of, big, sizeof(big));
        r = lora_asm_feed(&a, 20000 + (uint32_t)i * 1000u, part, n, &slot);   /* a second apart */
        CHECK_EQ(r, i + 1u == of ? LORA_ASM_COMPLETE : LORA_ASM_NEED_MORE);
    }
    CHECK(slot != NULL && slot->total == sizeof(big));
    CHECK(memcmp(slot->data, big, sizeof(big)) == 0);
    CHECK_EQ(a.timeouts, 0);
    lora_asm_done(&a, slot);

    /* Without the large slot, the same payload is refused at its first part, not truncated. */
    asm_setup(&a, false);
    n = build(part, sizeof(part), PEER_ONE, 12, 0, (uint8_t)of, big, sizeof(big));
    CHECK_EQ(lora_asm_feed(&a, 40000, part, n, &slot), LORA_ASM_OVERSIZE);
    CHECK(slot == NULL);
    CHECK_EQ(lora_asm_capacity(&a), LORA_SMALL_MAX);

    /* Two senders at once, and a third that finds no slot free. */
    asm_setup(&a, true);
    for (uint8_t peer = 1; peer <= LORA_ASM_SLOTS; peer++) {
        n = build(part, sizeof(part), (uint8_t)(LORA_PEER_AP | peer), 9, 0, 2, payload, sizeof(payload));
        CHECK_EQ(lora_asm_feed(&a, 2000, part, n, &slot), LORA_ASM_NEED_MORE);
    }
    n = build(part, sizeof(part), (uint8_t)(LORA_PEER_AP | (LORA_ASM_SLOTS + 1u)), 9, 0, 2, payload, sizeof(payload));
    CHECK_EQ(lora_asm_feed(&a, 2000, part, n, &slot), LORA_ASM_FULL);
    /* Each set still completes on its own. */
    n = build(part, sizeof(part), PEER_TWO, 9, 1, 2, payload, sizeof(payload));
    CHECK_EQ(lora_asm_feed(&a, 2100, part, n, &slot), LORA_ASM_COMPLETE);
    CHECK(slot != NULL && slot->total == 2u * LORA_SLICE_MAX);
    lora_asm_done(&a, slot);

    /* A part that is not full but claims not to be the last is a lie. */
    asm_setup(&a, true);
    n = lora_part_build(PEER_ONE, 4, 0, 2, payload, 10, part, sizeof(part));
    CHECK_EQ(lora_asm_feed(&a, 3000, part, n, &slot), LORA_ASM_BAD);
    /* The same message number with a different part count is not the same message. */
    n = build(part, sizeof(part), PEER_ONE, 5, 0, 2, payload, sizeof(payload));
    CHECK_EQ(lora_asm_feed(&a, 3000, part, n, &slot), LORA_ASM_NEED_MORE);
    n = build(part, sizeof(part), PEER_ONE, 5, 0, 3, payload, sizeof(payload));
    CHECK_EQ(lora_asm_feed(&a, 3000, part, n, &slot), LORA_ASM_BAD);
    CHECK(a.dropped > 0);
}

/* ---- the send policy and its queue ---- */

static size_t make_frame(uint8_t type, uint8_t scope, uint16_t flags, uint8_t *out, size_t cap, size_t body)
{
    static uint8_t filler[LG_TEXT_MAX];
    lg_env_t e = { 0 };
    e.type = type;
    e.scope = scope;
    e.flags = flags;
    e.origin_id = 5;
    e.origin_boot = 1;
    e.origin_seq = 9;
    e.target = scope == LG_SCOPE_BROADCAST ? LG_TARGET_ALL : 6;
    int n = lg_frame_build(&e, filler, body, out, cap);
    return n > 0 ? (size_t)n : 0u;
}

static void test_policy(void)
{
    static uint8_t frame[LORA_SMALL_MAX];
    size_t n;

    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), LG_VOICE_DATA_MAX / 4u);
    CHECK(n > 0);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    CHECK(!lora_is_alert_frame(frame, n));
    /* Live voice is refused even when it is marked urgent: seconds of airtime for 100 ms of sound. */
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, LG_FLAG_URGENT, frame, sizeof(frame), 40);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    /*
     * The compact codec is the exception (D95): marked on the envelope and no longer than a compact
     * packet, it goes like text, when Wi-Fi is not carrying. The same marker on a frame as long as a
     * live one buys nothing, so a forged marker cannot put 400 bytes on the air.
     */
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, LG_FLAG_COMPACT, frame, sizeof(frame),
                   LG_VOICE_C2_PAYLOAD_MAX + LG_AEAD_TAG_LEN);
    CHECK(n > 0);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_IF_WIFI_DOWN);
    CHECK(lglh_type_allowed(frame, n));
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, LG_FLAG_COMPACT, frame, sizeof(frame),
                   LG_VOICE_C2_PAYLOAD_MAX + LG_AEAD_TAG_LEN + 1u);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    CHECK(!lglh_type_allowed(frame, n));
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 40);
    CHECK(!lglh_type_allowed(frame, n));

    n = make_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_URGENT, frame, sizeof(frame), 20);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_ALWAYS);
    CHECK(lora_is_alert_frame(frame, n));
    n = make_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_ALL_CLEAR | LG_FLAG_URGENT, frame, sizeof(frame), 20);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_ALWAYS);

    n = make_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 20);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_IF_WIFI_DOWN);
    n = make_frame(LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 8);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_IF_WIFI_DOWN);
    n = make_frame(LG_T_POSITION, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 18);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_IF_WIFI_DOWN);

    /*
     * Housekeeping gives way. The shared state and the handheld names heal a miss and can wait;
     * on a link carrying one message every three seconds they must not sit in front of what
     * someone is waiting for (D74).
     */
    n = make_frame(LG_T_GRID_STATE, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 40);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_SPARE_ROOM);
    CHECK(!lora_is_alert_frame(frame, n));
    n = make_frame(LG_T_NAME, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 12);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_SPARE_ROOM);
    /* D101: firmware never goes by LoRa, urgent or not, and a handheld may not send it at all. */
    n = make_frame(LG_T_FW_DATA, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 520);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    CHECK(!lglh_type_allowed(frame, n));
    n = make_frame(LG_T_FW_OFFER, LG_SCOPE_SYSTEM, LG_FLAG_URGENT, frame, sizeof(frame), 12);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    n = make_frame(LG_T_FW_GET, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 14);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
    CHECK(!lglh_type_allowed(frame, n));
    /* D99: a trail waits for room between APs, and a handheld may send one and ask for one. */
    n = make_frame(LG_T_TRAIL, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), LG_TRAIL_LEN_MAX);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_SPARE_ROOM);
    CHECK(lglh_type_allowed(frame, n));
    n = make_frame(LG_T_TRAIL_REQ, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), LG_TRAIL_REQ_LEN);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_IF_WIFI_DOWN);
    CHECK(lglh_type_allowed(frame, n));
    /* An urgent one still goes at once: the class never outranks an alert. */
    n = make_frame(LG_T_GRID_STATE, LG_SCOPE_SYSTEM, LG_FLAG_URGENT, frame, sizeof(frame), 40);
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_ALWAYS);

    /* Anything we cannot read is not put on the air. */
    CHECK_EQ(lora_policy_for_frame(frame, 4), LORA_SEND_NEVER);
    frame[0] = 0xFFu;
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);
}

static uint8_t s_txq_small[4][LORA_SMALL_MAX];
static uint8_t s_txq_large[LORA_PAYLOAD_MAX];

static void txq_setup(lora_txq_t *q, bool with_large)
{
    lora_txq_init(q);
    for (size_t i = 0; i < 4; i++) {
        lora_txq_set_slot(q, i, s_txq_small[i], sizeof(s_txq_small[i]));
    }
    lora_txq_set_slot(q, 4, with_large ? s_txq_large : NULL, sizeof(s_txq_large));
}

static void test_queue(void)
{
    static lora_txq_t q;
    static uint8_t body[64];

    txq_setup(&q, true);
    CHECK(lora_txq_peek(&q, 0) == NULL);

    /* Nothing is transmitted before its backoff is up. */
    CHECK(lora_txq_push(&q, 2, false, 1, body, sizeof(body), 500));
    CHECK(lora_txq_peek(&q, 100) == NULL);
    lora_txq_slot_t *s = lora_txq_peek(&q, 500);
    CHECK(s != NULL);
    lora_txq_release(&q, s);
    CHECK_EQ(q.depth, 0);

    /* First in, first out, until an alert arrives: it goes next whatever it queued behind. */
    txq_setup(&q, true);
    for (uint8_t i = 0; i < 3; i++) {
        body[0] = i;
        CHECK(lora_txq_push(&q, 2, false, i, body, sizeof(body), 0));
    }
    body[0] = 0xAA;
    CHECK(lora_txq_push(&q, 2, true, 9, body, sizeof(body), 0));
    CHECK(lora_txq_alert_due(&q, 10));
    s = lora_txq_peek(&q, 10);
    CHECK(s != NULL && s->alert && s->data[0] == 0xAA);
    lora_txq_release(&q, s);
    s = lora_txq_peek(&q, 10);
    CHECK(s != NULL && !s->alert && s->data[0] == 0);

    /* A long transfer already on the air keeps the radio, until an alert takes it between parts. */
    txq_setup(&q, true);
    body[0] = 0x11;
    CHECK(lora_txq_push(&q, 2, false, 1, body, sizeof(body), 0));
    s = lora_txq_peek(&q, 0);
    CHECK(s != NULL);
    s->sending = true;
    s->part = 1;   /* one part already on the air */
    body[0] = 0x22;
    CHECK(lora_txq_push(&q, 2, false, 2, body, sizeof(body), 0));
    CHECK(lora_txq_peek(&q, 0) == s);            /* the newcomer waits its turn */
    body[0] = 0x33;
    CHECK(lora_txq_push(&q, 2, true, 3, body, sizeof(body), 0));
    lora_txq_slot_t *a = lora_txq_peek(&q, 0);
    CHECK(a != NULL && a->alert && a->data[0] == 0x33);
    lora_txq_release(&q, a);
    s = lora_txq_peek(&q, 0);
    CHECK(s != NULL && s->sending && s->part == 1 && s->data[0] == 0x11);   /* it resumes where it was */

    /* Full: the oldest entry that is not an alert makes room, and an alert never does. */
    txq_setup(&q, false);   /* four slots, all the same size */
    body[0] = 0xA1;
    CHECK(lora_txq_push(&q, 2, true, 0, body, sizeof(body), 0));
    for (uint8_t i = 1; i < 4; i++) {
        body[0] = i;
        CHECK(lora_txq_push(&q, 2, false, i, body, sizeof(body), 0));
    }
    CHECK_EQ(q.depth, 4);
    CHECK_EQ(q.high, 4);
    body[0] = 0xBB;
    CHECK(lora_txq_push(&q, 2, false, 5, body, sizeof(body), 0));   /* evicts the oldest normal one */
    CHECK_EQ(q.dropped, 1);
    CHECK_EQ(q.depth, 4);
    s = lora_txq_peek(&q, 0);
    CHECK(s != NULL && s->alert && s->data[0] == 0xA1);   /* the alert is still there and goes first */
    /* The entry numbered 1 was the oldest normal one and is the one that went. */
    bool found_one = false;
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        found_one = found_one || (q.slots[i].in_use && q.slots[i].data != NULL && q.slots[i].data[0] == 1);
    }
    CHECK(!found_one);

    /* A queue that is all alerts refuses a newcomer rather than losing one of them. */
    txq_setup(&q, false);
    for (uint8_t i = 0; i < 4; i++) {
        body[0] = i;
        CHECK(lora_txq_push(&q, 2, true, i, body, sizeof(body), 0));
    }
    CHECK(!lora_txq_push(&q, 2, false, 8, body, sizeof(body), 0));
    CHECK(!lora_txq_push(&q, 2, true, 9, body, sizeof(body), 0));
    CHECK_EQ(q.dropped, 2);
    CHECK_EQ(q.depth, 4);
    for (size_t i = 0; i < 4; i++) {
        CHECK(q.slots[i].in_use && q.slots[i].alert);
    }

    /* A transfer already on the air is never thrown out to make room either. */
    txq_setup(&q, false);
    for (uint8_t i = 0; i < 4; i++) {
        body[0] = i;
        CHECK(lora_txq_push(&q, 2, false, i, body, sizeof(body), 0));
    }
    q.slots[0].sending = true;
    body[0] = 0xCC;
    CHECK(lora_txq_push(&q, 2, false, 7, body, sizeof(body), 0));
    CHECK(q.slots[0].in_use && q.slots[0].sending && q.slots[0].data[0] == 0);

    /* A 6 KB payload needs the large slot, and is refused when there is none. */
    txq_setup(&q, true);
    static uint8_t big[LORA_PAYLOAD_MAX];
    CHECK_EQ(lora_txq_capacity(&q), LORA_PAYLOAD_MAX);
    CHECK(lora_txq_push(&q, 2, false, 1, big, sizeof(big), 0));
    s = lora_txq_peek(&q, 0);
    CHECK(s != NULL && s->len == LORA_PAYLOAD_MAX && s->of == LORA_PARTS_MAX);
    txq_setup(&q, false);
    CHECK_EQ(lora_txq_capacity(&q), LORA_SMALL_MAX);
    CHECK(!lora_txq_push(&q, 2, false, 1, big, sizeof(big), 0));
    CHECK(lora_txq_peek(&q, 0) == NULL);

    /* Reserve and commit: a caller builds the payload in the slot, and a length that does not fit
     * frees the slot rather than half filling it. */
    txq_setup(&q, false);
    s = lora_txq_reserve(&q, 2, false, 1, 64, 0);
    CHECK(s != NULL && s->cap >= 64);
    CHECK(lora_txq_peek(&q, 0) == NULL);   /* not yet committed: never transmitted half built */
    CHECK(lora_txq_commit(&q, s, 64));
    CHECK(lora_txq_peek(&q, 0) == s);
    lora_txq_release(&q, s);
    s = lora_txq_reserve(&q, 2, false, 2, 64, 0);
    CHECK(s != NULL);
    CHECK(!lora_txq_commit(&q, s, LORA_SMALL_MAX + 1u));
    CHECK_EQ(q.depth, 0);

    /*
     * D102: what gives way to what. A message in a full queue takes the place of routine traffic, the
     * oldest of it first; routine traffic arriving at a queue full of messages is the one refused; and
     * a message among messages still replaces the oldest, as before.
     */
    txq_setup(&q, false);
    for (uint8_t i = 0; i < 4; i++) {   /* routine 0, message 1, routine 2, message 3 */
        s = lora_txq_reserve_ranked(&q, 2, i % 2u ? LORA_RANK_MESSAGE : LORA_RANK_ROUTINE, i, 64, 0);
        CHECK(s != NULL);
        s->data[0] = i;
        CHECK(lora_txq_commit(&q, s, 64));
    }
    CHECK_EQ(q.dropped, 0);
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 4, 64, 0);   /* a message: routine 0 goes */
    CHECK(s != NULL && s->data[0] == 0);
    s->data[0] = 4;
    CHECK(lora_txq_commit(&q, s, 64));
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 5, 64, 0);   /* another: routine 2 goes */
    CHECK(s != NULL && s->data[0] == 2);
    s->data[0] = 5;
    CHECK(lora_txq_commit(&q, s, 64));
    CHECK_EQ(q.dropped, 2);
    CHECK(lora_txq_reserve_ranked(&q, 2, LORA_RANK_ROUTINE, 6, 64, 0) == NULL);   /* all messages now: refused */
    CHECK_EQ(q.dropped, 3);
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        CHECK(!q.slots[i].in_use || q.slots[i].rank == LORA_RANK_MESSAGE);
    }
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 7, 64, 0);   /* a message among messages: the oldest, 1 */
    CHECK(s != NULL && s->data[0] == 1);
    s->data[0] = 7;
    CHECK(lora_txq_commit(&q, s, 64));
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_ALERT, 8, 64, 0);     /* an alert takes a message's place: 3 */
    CHECK(s != NULL && s->data[0] == 3 && s->alert);
    s->data[0] = 8;
    CHECK(lora_txq_commit(&q, s, 64));
    s = lora_txq_peek(&q, 0);
    CHECK(s != NULL && s->alert && s->data[0] == 8);   /* and still goes first */
    /*
     * Asking first (D102): the answer is the one reserve would give, and asking changes nothing. This is
     * what lets a heartbeat wait half a second for room instead of being refused for a minute.
     */
    CHECK(!lora_txq_would_take(&q, LORA_RANK_ROUTINE, 64));   /* three messages and an alert: no place for routine */
    CHECK(lora_txq_would_take(&q, LORA_RANK_MESSAGE, 64));    /* a message would take the oldest message's */
    CHECK(lora_txq_would_take(&q, LORA_RANK_ALERT, 64));
    CHECK(!lora_txq_would_take(&q, LORA_RANK_ALERT, LORA_SMALL_MAX + 1u));   /* nothing here is that big */
    CHECK(!lora_txq_would_take(&q, LORA_RANK_ROUTINE, 0));
    CHECK_EQ(q.dropped, 5);   /* as it was after the alert took a message's place: asking counted nothing */
    CHECK_EQ(q.dropped_messages, 2);   /* the two messages that gave way, counted apart from routine drops */
    CHECK_EQ(q.depth, 4);
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        if (q.slots[i].in_use && !q.slots[i].alert) {
            lora_txq_release(&q, &q.slots[i]);   /* one message sent: a place comes up */
            break;
        }
    }
    CHECK(lora_txq_would_take(&q, LORA_RANK_ROUTINE, 64));
    CHECK(lora_txq_reserve_ranked(&q, 2, LORA_RANK_ROUTINE, 9, 64, 0) != NULL);
    /* The old call keeps its meaning: an alert, or routine. */
    txq_setup(&q, false);
    s = lora_txq_reserve(&q, 2, true, 1, 64, 0);
    CHECK(s != NULL && s->rank == LORA_RANK_ALERT && s->alert);
    s = lora_txq_reserve(&q, 2, false, 2, 64, 0);
    CHECK(s != NULL && s->rank == LORA_RANK_ROUTINE && !s->alert);
    /* What a frame is worth is read off the frame. */
    static uint8_t ranked[LG_FRAME_MAX];
    size_t rn = make_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, ranked, sizeof(ranked), 40);
    CHECK_EQ(lora_frame_rank(ranked, rn), LORA_RANK_MESSAGE);
    rn = make_frame(LG_T_MSG_ACK, LG_SCOPE_DIRECT, 0, ranked, sizeof(ranked), 13);
    CHECK_EQ(lora_frame_rank(ranked, rn), LORA_RANK_MESSAGE);
    rn = make_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_URGENT, ranked, sizeof(ranked), 20);
    CHECK_EQ(lora_frame_rank(ranked, rn), LORA_RANK_ALERT);
    rn = make_frame(LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, 0, ranked, sizeof(ranked), 8);
    CHECK_EQ(lora_frame_rank(ranked, rn), LORA_RANK_ROUTINE);
    rn = make_frame(LG_T_POSITION, LG_SCOPE_SYSTEM, 0, ranked, sizeof(ranked), 18);
    CHECK_EQ(lora_frame_rank(ranked, rn), LORA_RANK_ROUTINE);
    CHECK_EQ(lora_frame_rank(ranked, 3), LORA_RANK_ROUTINE);

    /* Bounds. */
    CHECK(!lora_txq_push(&q, 2, false, 0, body, 0, 0));
    CHECK(lora_txq_reserve(&q, 2, false, 0, LORA_PAYLOAD_MAX + 1u, 0) == NULL);
}

/* ---- the module's AT replies ---- */

static void test_at_parser(void)
{
    char line[256];
    lora_at_t at;

    snprintf(line, sizeof(line), "+OK");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_OK);

    snprintf(line, sizeof(line), "+OK\r");   /* the module ends its lines with CR LF */
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_OK);

    snprintf(line, sizeof(line), "+ERR=4");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_ERR);
    CHECK_EQ(at.err, 4);

    snprintf(line, sizeof(line), "+READY");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_READY);

    snprintf(line, sizeof(line), "+VER=RYLR998_V1.2.1");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_VALUE);
    CHECK(strcmp(at.name, "VER") == 0);
    CHECK(strcmp(at.value, "RYLR998_V1.2.1") == 0);

    snprintf(line, sizeof(line), "+PARAMETER=9,7,1,12");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_VALUE);
    CHECK(strcmp(at.name, "PARAMETER") == 0);
    CHECK(strcmp(at.value, "9,7,1,12") == 0);

    /* A received part, with a negative RSSI and a negative SNR. */
    snprintf(line, sizeof(line), "+RCV=2,8,TEBYWQAA,-104,-7");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_RCV);
    CHECK_EQ(at.addr, 2);
    CHECK_EQ(at.len, 8);
    CHECK(strcmp(at.data, "TEBYWQAA") == 0);
    CHECK_EQ(at.rssi, -104);
    CHECK_EQ(at.snr, -7);

    snprintf(line, sizeof(line), "+RCV=1,4,Zg==,-33,10");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_RCV);
    CHECK(strcmp(at.data, "Zg==") == 0);
    CHECK_EQ(at.rssi, -33);
    CHECK_EQ(at.snr, 10);

    /* The declared length ends the data, not the next comma, so a payload with a comma in it
     * still reads correctly. Ours is base64 and never has one. */
    snprintf(line, sizeof(line), "+RCV=3,5,a,b,c,-90,3");
    CHECK(lora_at_parse(line, &at));
    CHECK_EQ(at.kind, LORA_AT_RCV);
    CHECK_EQ(at.len, 5);
    CHECK(strcmp(at.data, "a,b,c") == 0);
    CHECK_EQ(at.rssi, -90);

    /* Lines that are not ours, or are cut short, are refused rather than half read. */
    snprintf(line, sizeof(line), "+RCV=2,80,short,-104,-7");
    CHECK(!lora_at_parse(line, &at));
    snprintf(line, sizeof(line), "+RCV=2,4");
    CHECK(!lora_at_parse(line, &at));
    snprintf(line, sizeof(line), "+ERR=x");
    CHECK(!lora_at_parse(line, &at));
    snprintf(line, sizeof(line), "AT+SEND=1,4,Zg==");   /* our own echo, if echo were ever on */
    CHECK(!lora_at_parse(line, &at));
    snprintf(line, sizeof(line), "%s", "");
    CHECK(!lora_at_parse(line, &at));
}

/* ---- a whole payload, out and back through base64 ---- */

static void test_round_trip(void)
{
    static lora_asm_t a;
    static uint8_t sealed[512];
    uint8_t part[LORA_PART_RAW_MAX];
    char b64[LORA_PART_B64_MAX + 1];
    uint8_t back[LORA_PART_RAW_MAX];
    lora_asm_slot_t *slot = NULL;

    for (size_t i = 0; i < sizeof(sealed); i++) {
        sealed[i] = (uint8_t)(i * 11u + 5u);
    }
    /* 288 bytes is the frame size docs/lora.md sizes the radio against. */
    const size_t len = 288;
    size_t of = lora_part_count(len);
    CHECK_EQ(of, (len + LORA_SLICE_MAX - 1u) / LORA_SLICE_MAX);
    asm_setup(&a, true);
    lora_asm_result_t r = LORA_ASM_NEED_MORE;
    for (size_t i = 0; i < of; i++) {
        size_t n = build(part, sizeof(part), PEER_ONE, 42, (uint8_t)i, (uint8_t)of, sealed, len);
        size_t blen = lora_b64_encode(part, n, b64, sizeof(b64) - 1u);
        CHECK(blen > 0 && blen <= LORA_PART_B64_MAX);
        b64[blen] = '\0';
        size_t m = lora_b64_decode(b64, blen, back, sizeof(back));
        CHECK_EQ(m, n);
        r = lora_asm_feed(&a, 1000 + (uint32_t)i * 900u, back, m, &slot);
    }
    CHECK_EQ(r, LORA_ASM_COMPLETE);
    CHECK(slot != NULL && slot->total == len);
    CHECK(memcmp(slot->data, sealed, len) == 0);
    CHECK_EQ(a.dropped, 0);
    CHECK_EQ(a.timeouts, 0);
    lora_asm_done(&a, slot);
}

static void test_airtime(void)
{
    /* SF9, BW125, CR4/5, preamble 12. A full 120-character part is about two thirds of a second;
     * four of them, plus the random backoff, are the couple of seconds docs/lora.md promises for
     * one 288-byte frame. */
    uint32_t full = lora_airtime_ms(LORA_PART_B64_MAX);
    CHECK(full > 500 && full < 800);
    CHECK(lora_airtime_ms(8) < full);
    CHECK(lora_airtime_ms(0) > 0);   /* the preamble and header cost something on their own */
    CHECK(lora_airtime_ms(240) > full);
    /* A 6 KB voice note is LORA_PARTS_MAX of them: under a minute of airtime, which is why its
     * reassembly timeout has to be an idle one and not ten seconds from the first part. */
    CHECK(LORA_PARTS_MAX * full < LORA_ASM_MAX_MS);
}

/*
 * D104: a message between APs is acknowledged, and sent again until it is.
 *
 * Until this a frame went on the air once. With the Wi-Fi backbone off, a 90 minute run on 2026-10-06
 * lost five messages between registered handhelds, none of them to a full queue: they were transmitted
 * and never arrived, on a channel three radios share without listening first.
 */
static void test_ack(void)
{
    static lora_txq_t q;
    lora_txq_slot_t *s;
    uint8_t ack[LORA_ACK_LEN];
    uint32_t seq = 0;

    /* The acknowledgement itself: six bytes, and nothing else is one. */
    CHECK_EQ(lora_ack_build(0x01020304u, ack, sizeof(ack)), LORA_ACK_LEN);
    CHECK(lora_ack_parse(ack, sizeof(ack), &seq) && seq == 0x01020304u);
    CHECK(!lora_ack_parse(ack, sizeof(ack) - 1u, &seq));
    ack[0] ^= 1u;
    CHECK(!lora_ack_parse(ack, sizeof(ack), &seq));
    CHECK_EQ(lora_ack_build(1, ack, sizeof(ack) - 1u), 0);

    /* What is acknowledged: what a person wrote, the word that it arrived, an alert. Not the rest. */
    static uint8_t f[LG_FRAME_MAX];
    size_t n = make_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, f, sizeof(f), 40);
    CHECK(lora_frame_wants_ack(f, n));
    n = make_frame(LG_T_MSG_ACK, LG_SCOPE_DIRECT, 0, f, sizeof(f), 13);
    CHECK(lora_frame_wants_ack(f, n));
    n = make_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_URGENT, f, sizeof(f), 20);
    CHECK(lora_frame_wants_ack(f, n));
    n = make_frame(LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, 0, f, sizeof(f), 8);
    CHECK(!lora_frame_wants_ack(f, n));
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, LG_FLAG_COMPACT, f, sizeof(f), 30);
    CHECK(!lora_frame_wants_ack(f, n));
    CHECK(lora_frame_wants_ap_ack(f, n));        /* between two APs a compact voice packet is answered (D95) */
    CHECK(!lora_ack_parse(f, n, &seq));          /* an envelope is never read as an acknowledgement */
    n = make_frame(LG_T_VOICE, LG_SCOPE_DIRECT, 0, f, sizeof(f), 30);
    CHECK(!lora_frame_wants_ap_ack(f, n));       /* live voice is never on LoRa to be answered */
    n = make_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, f, sizeof(f), 40);
    CHECK(lora_frame_wants_ap_ack(f, n));        /* and everything that wanted an answer still does */
    n = make_frame(LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, 0, f, sizeof(f), 8);
    CHECK(!lora_frame_wants_ap_ack(f, n));
    lora_ack_build(7, ack, sizeof(ack));
    CHECK(!lora_frame_wants_ack(ack, sizeof(ack)));   /* and an acknowledgement is never acknowledged */
    CHECK(!lora_frame_wants_ap_ack(ack, sizeof(ack)));

    /* A frame nobody is to answer is finished when its last part has gone, as it always was. */
    txq_setup(&q, false);
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_ROUTINE, 1, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    CHECK(!lora_txq_sent(&q, s, 1000));
    CHECK_EQ(q.depth, 0);
    CHECK(!lora_txq_awaiting(&q));

    /* One that is awaited stays, is not transmitted again while it waits, and goes when the answer comes. */
    s = lora_txq_reserve_ranked(&q, LG_LORA_ADDR_AP(1), LORA_RANK_MESSAGE, 2, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    lora_txq_want_ack(s, 500, 1u << 1);
    CHECK(lora_txq_peek(&q, 0) == s);
    CHECK(lora_txq_sent(&q, s, 1000));
    CHECK(lora_txq_awaiting(&q));
    CHECK(lora_txq_peek(&q, 999) == NULL);
    CHECK(!lora_txq_acked(&q, 501, 1));            /* another frame's number */
    CHECK(!lora_txq_acked(&q, 500, 2));            /* an AP it was not for */
    CHECK(lora_txq_awaiting(&q));
    CHECK(lora_txq_acked(&q, 500, 1));
    CHECK(!lora_txq_awaiting(&q));
    CHECK_EQ(q.depth, 0);
    CHECK_EQ(q.acked, 1);
    CHECK(!lora_txq_acked(&q, 500, 1));            /* the same answer twice changes nothing */
    CHECK_EQ(q.acked, 1);

    /*
     * No answer: it goes out again from its first part, LORA_TX_TRIES times in all, and is then given
     * up and counted. Sent to every AP, it is addressed the second time to the one that has not answered.
     */
    txq_setup(&q, false);
    s = lora_txq_reserve_ranked(&q, LG_LORA_ADDR_BROADCAST, LORA_RANK_MESSAGE, 3, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    lora_txq_want_ack(s, 600, (1u << 1) | (1u << 2));
    s->part = s->of;
    CHECK(lora_txq_sent(&q, s, 1000));
    CHECK(lora_txq_acked(&q, 600, 2));             /* one of the two answers */
    CHECK(lora_txq_awaiting(&q));
    CHECK_EQ(q.acked, 0);
    CHECK(lora_txq_peek(&q, 1000) == s);           /* the wait is over: due again */
    CHECK_EQ(s->part, 0);
    CHECK_EQ(s->dest, LG_LORA_ADDR_AP(1));
    CHECK(!s->waiting);
    CHECK_EQ(q.resent, 1);
    CHECK(lora_txq_sent(&q, s, 2000));
    CHECK(lora_txq_peek(&q, 2000) == s);
    CHECK_EQ(q.resent, 2);
    CHECK(lora_txq_sent(&q, s, 3000));             /* the third time */
    CHECK(lora_txq_peek(&q, 3000) == NULL);        /* and no fourth */
    CHECK_EQ(q.unacked, 1);
    CHECK_EQ(q.depth, 0);

    /* An answer that arrives while the second copy is going out frees it when that copy is done. */
    txq_setup(&q, false);
    s = lora_txq_reserve_ranked(&q, LG_LORA_ADDR_AP(1), LORA_RANK_MESSAGE, 4, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    lora_txq_want_ack(s, 700, 1u << 1);
    CHECK(lora_txq_sent(&q, s, 1000));
    CHECK(lora_txq_peek(&q, 1000) == s);
    s->sending = true;
    CHECK(lora_txq_acked(&q, 700, 1));
    CHECK_EQ(q.depth, 1);                          /* not pulled from under the transmitter */
    CHECK(!lora_txq_sent(&q, s, 2000));
    CHECK_EQ(q.depth, 0);
    CHECK_EQ(q.acked, 1);
    CHECK_EQ(q.unacked, 0);

    /*
     * While this AP listens for an answer it transmits nothing that could talk over it: only an alert,
     * and an answer it owes somebody else. And an answer goes before a message that has waited longer.
     */
    txq_setup(&q, false);
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 5, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    lora_txq_slot_t *msg = s;
    CHECK(lora_txq_peek_for(&q, 0, true) == NULL);
    CHECK(lora_txq_peek_for(&q, 0, false) == msg);
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 6, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    s->is_ack = true;
    CHECK(lora_txq_peek_for(&q, 0, true) == s);
    CHECK(lora_txq_peek_for(&q, 0, false) == s);
    lora_txq_release(&q, s);
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_ALERT, 7, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    CHECK(lora_txq_peek_for(&q, 0, true) == s);

    /*
     * A full queue: a message that has been on the air and is only waiting to hear so gives way before
     * one that has never been sent, and is counted as not known to have arrived rather than as dropped
     * unsent.
     */
    txq_setup(&q, false);
    lora_txq_slot_t *four[4];
    for (uint8_t i = 0; i < 4; i++) {
        four[i] = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, i, 64, 0);
        CHECK(four[i] != NULL && lora_txq_commit(&q, four[i], 64));
        four[i]->data[0] = i;
    }
    lora_txq_want_ack(four[2], 800, 1u << 1);      /* the third was sent and waits; the first is the oldest */
    CHECK(lora_txq_sent(&q, four[2], 5000));
    s = lora_txq_reserve_ranked(&q, 2, LORA_RANK_MESSAGE, 9, 64, 0);
    CHECK(s == four[2]);
    CHECK(!s->waiting && s->await == 0u && s->tries == 0u);   /* the slot starts clean for its new frame */
    CHECK_EQ(q.unacked, 1);
    CHECK_EQ(q.dropped_messages, 0);
    CHECK(lora_txq_reserve_ranked(&q, 2, LORA_RANK_ROUTINE, 10, 64, 0) == NULL);   /* routine still never does */

    /*
     * D109: a frame for one handheld waits for that handheld's answer, and a repeat keeps its address -
     * it is not readdressed to an access point the way a broadcast's repeat is.
     */
    txq_setup(&q, false);
    const uint16_t hh = 101u;   /* a handheld's LoRa address */
    s = lora_txq_reserve_ranked(&q, hh, LORA_RANK_MESSAGE, 11, 64, 0);
    CHECK(s != NULL && lora_txq_commit(&q, s, 64));
    lora_txq_want_ack(s, 900, LORA_AWAIT_HANDHELD);
    CHECK(lora_txq_sent(&q, s, 1000));
    CHECK(!lora_txq_acked(&q, 900, 1));               /* an access point's answer is not the handheld's */
    CHECK(lora_txq_peek(&q, 1000) == s);              /* no answer: again */
    CHECK_EQ(s->dest, hh);
    CHECK(lora_txq_sent(&q, s, 2000));
    CHECK(lora_txq_acked(&q, 900, 15));               /* the handheld's answer */
    CHECK_EQ(q.depth, 0);
    CHECK_EQ(q.acked, 1);
}

void test_lora(void)
{
    test_base64();
    test_parts();
    test_reassembly();
    test_policy();
    test_queue();
    test_ack();
    test_at_parser();
    test_round_trip();
    test_airtime();
}
