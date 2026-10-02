/*
 * test_lora_hh.c - sealing LoRa frames between a handheld and an AP (D77), without a radio.
 *
 * D71 left open which key seals a handheld's LoRa frames; D77 answers it with a key derived from
 * the grid secret and held by handhelds and APs alike, never the APs' backbone key. What is proved
 * here is the part that actually protects the grid: the two key domains cannot be made to open
 * each other's frames, a replay is caught, a handheld and an AP of the same index keep separate
 * sequence spaces, and an AP refuses the message types a handheld must never be trusted about,
 * however well sealed the frame is.
 */
#include <string.h>

#include "lg_body.h"       /* LG_PING_LEN: the announcement is the smallest body there is */
#include "lg_envelope.h"
#include "lg_lora_hh.h"
#include "lg_lora_wire.h"
#include "lg_test.h"

/* A key with no meaning outside this file: the real one is derived by tools/gen_secrets.py. */
static const uint8_t HH_KEY[LG_AEAD_KEY_LEN] = {
    0x4b, 0x2c, 0x46, 0x07, 0x81, 0x6e, 0x5f, 0x57, 0x74, 0x5e, 0x6c, 0x12, 0x94, 0x21, 0x09, 0x18,
    0xdf, 0xfe, 0xf1, 0x6e, 0xc1, 0x63, 0xf5, 0x61, 0xd6, 0x90, 0x89, 0xa4, 0x47, 0x94, 0x28, 0xf8,
};

static size_t hh_frame(uint8_t type, uint8_t scope, uint16_t flags, uint8_t *out, size_t cap, size_t body)
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

static void test_seal_round_trip(void)
{
    static lglh_t hh, ap;
    static uint8_t frame[LORA_SMALL_MAX];
    static uint8_t sealed[LORA_SMALL_MAX];
    static uint8_t opened[LORA_SMALL_MAX];
    size_t out_len = 0;
    uint16_t sender = 0;

    lglh_init(&hh, HH_KEY, LGLH_SENDER_HH(1), 7);
    lglh_init(&ap, HH_KEY, LGLH_SENDER_AP(0), 3);

    size_t n = hh_frame(LG_T_POSITION, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 18);
    CHECK(n > 0);

    /* Handheld 1 seals a position; AP 0 opens it and knows who sent it. */
    int s = lglh_seal(&hh, sealed, sizeof(sealed), frame, n);
    CHECK_EQ((size_t)s, LGLH_OUTER_LEN + n + LG_AEAD_TAG_LEN);
    CHECK_EQ(sealed[0], LGLH_VERSION);
    CHECK_EQ(sealed[1], LGLH_EPOCH_HH);
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len), LGLH_OPEN_OK);
    CHECK_EQ(sender, LGLH_SENDER_HH(1));
    CHECK_EQ(LGLH_KIND(sender), LGLH_KIND_HH);
    CHECK_EQ(LGLH_INDEX(sender), 1);
    CHECK_EQ(out_len, n);
    CHECK(memcmp(opened, frame, n) == 0);

    /* The same bytes again are a replay, not a second message. */
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len), LGLH_OPEN_REPLAY);
    CHECK_EQ(ap.replayed, 1);

    /* The sequence rises within a boot, so the nonce never repeats and the next frame opens. */
    int s2 = lglh_seal(&hh, sealed, sizeof(sealed), frame, n);
    CHECK(s2 > 0);
    CHECK_EQ(lg_rd32(sealed + 8), 2u);
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s2, &sender, opened, sizeof(opened), &out_len), LGLH_OPEN_OK);

    /* A tampered tag is counted as an authentication failure and never opened. */
    sealed[(size_t)s2 - 1] ^= 0x01u;
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s2, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_AUTH_FAIL);
    CHECK_EQ(ap.auth_fail, 1);
}

static void test_key_domains(void)
{
    static lglh_t ap, other;
    static uint8_t frame[LORA_SMALL_MAX];
    static uint8_t sealed[LORA_SMALL_MAX];
    static uint8_t opened[LORA_SMALL_MAX];
    static uint8_t other_key[LG_AEAD_KEY_LEN];
    size_t out_len = 0;
    uint16_t sender = 0;

    lglh_init(&ap, HH_KEY, LGLH_SENDER_AP(0), 3);
    size_t n = hh_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 20);
    CHECK(n > 0);

    /*
     * A frame sealed with a different key - which is what the backbone key, or another grid's
     * module, looks like from here - never authenticates. This is the whole point of D77: an
     * attacker holding one key cannot read or forge a byte of the other domain.
     */
    memcpy(other_key, HH_KEY, sizeof(other_key));
    other_key[0] ^= 0xFFu;
    lglh_init(&other, other_key, LGLH_SENDER_HH(2), 1);
    int s = lglh_seal(&other, sealed, sizeof(sealed), frame, n);
    CHECK(s > 0);
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_AUTH_FAIL);

    /*
     * A payload from the other key domain is recognised by its epoch byte and is NOT counted as a
     * fault: an AP feeds every LoRa payload to both readers, so a backbone frame arriving here is
     * ordinary traffic. A counter that rose on it would make the admin page cry wolf all day.
     */
    uint32_t faults = ap.malformed + ap.auth_fail;
    sealed[1] = 0u;   /* the backbone's key epoch */
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_NOT_MINE);
    CHECK_EQ(ap.malformed + ap.auth_fail, faults);
    sealed[1] = LGLH_EPOCH_HH;
    sealed[0] = 0xFFu;   /* a version we do not speak: also not ours, also not a fault */
    CHECK_EQ(lglh_open(&ap, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_NOT_MINE);
    CHECK_EQ(ap.malformed + ap.auth_fail, faults);
}

static void test_malformed_and_indices(void)
{
    static lglh_t hh, twin, ap_one, hh_one, reader;
    static uint8_t frame[LORA_SMALL_MAX];
    static uint8_t sealed[LORA_SMALL_MAX];
    static uint8_t opened[LORA_SMALL_MAX];
    size_t out_len = 0;
    uint16_t sender = 0;

    lglh_init(&hh, HH_KEY, LGLH_SENDER_HH(1), 7);
    size_t n = hh_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 20);
    CHECK(n > 0);
    int s = lglh_seal(&hh, sealed, sizeof(sealed), frame, n);
    CHECK(s > 0);

    /* Too short to hold an outer header and a tag. */
    CHECK_EQ(lglh_open(&hh, sealed, LGLH_SEALED_MIN - 1u, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_MALFORMED);

    /* Our own identity coming back at us: two boards flashed with one ID, or a loop (D75). */
    lglh_init(&twin, HH_KEY, LGLH_SENDER_HH(1), 7);
    CHECK_EQ(lglh_open(&twin, sealed, (size_t)s, &sender, opened, sizeof(opened), &out_len),
             LGLH_OPEN_MALFORMED);
    CHECK_EQ(twin.malformed, 1);

    /*
     * A handheld index and an AP index of the same number keep separate sequence spaces, because
     * the replay window is keyed on the whole sender field, kind bit included. Without that, AP 1's
     * frames would retire handheld 1's and half the traffic would vanish as "duplicates".
     */
    lglh_init(&ap_one, HH_KEY, LGLH_SENDER_AP(1), 1);
    lglh_init(&hh_one, HH_KEY, LGLH_SENDER_HH(1), 1);
    lglh_init(&reader, HH_KEY, LGLH_SENDER_HH(9), 1);
    int a = lglh_seal(&ap_one, sealed, sizeof(sealed), frame, n);
    CHECK(a > 0);
    CHECK_EQ(lglh_open(&reader, sealed, (size_t)a, &sender, opened, sizeof(opened), &out_len), LGLH_OPEN_OK);
    CHECK_EQ(LGLH_KIND(sender), LGLH_KIND_AP);
    int b = lglh_seal(&hh_one, sealed, sizeof(sealed), frame, n);
    CHECK(b > 0);
    CHECK_EQ(lglh_open(&reader, sealed, (size_t)b, &sender, opened, sizeof(opened), &out_len), LGLH_OPEN_OK);
    CHECK_EQ(LGLH_KIND(sender), LGLH_KIND_HH);
    CHECK_EQ(reader.replayed, 0);   /* sequence 1 from both, and neither retired the other */
}

/*
 * What an AP accepts from a handheld over LoRa. The refusals matter more than the acceptances: a
 * stolen handheld holds this key, so anything an AP would have to trust it about is refused.
 */
static void test_allowed_types(void)
{
    static uint8_t frame[LORA_SMALL_MAX];
    size_t n;

    n = hh_frame(LG_T_POSITION, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 18);
    CHECK(lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 8);
    CHECK(lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_TEXT, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 20);
    CHECK(lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_URGENT, frame, sizeof(frame), 20);
    CHECK(lglh_type_allowed(frame, n));   /* the SOS this radio exists for */
    /* An all clear is itself an urgent broadcast (D66, D79), so it carries both flags, exactly as
     * hh_service_send_all_clear builds it. */
    n = hh_frame(LG_T_TEXT, LG_SCOPE_BROADCAST, LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR, frame, sizeof(frame), 20);
    CHECK(lglh_type_allowed(frame, n));
    /* A MSG_ACK is 1:1 by the envelope's own rule (lg_env_decode rejects any other scope), so it is
     * built that way here: the first run of this suite on a board caught the test getting it wrong. */
    n = hh_frame(LG_T_MSG_ACK, LG_SCOPE_DIRECT, 0, frame, sizeof(frame), 8);
    CHECK(lglh_type_allowed(frame, n));

    /*
     * D80's announcement pair. A handheld sends PING on a timer so an AP knows it can be reached
     * this way before it has anything urgent to say; the AP's PONG is how the handheld learns the
     * path works both ways. Admitting them asserts nothing about the grid, which is the test every
     * type on this list has to pass.
     */
    n = hh_frame(LG_T_PING, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), LG_PING_LEN);
    CHECK(lglh_type_allowed(frame, n));
    CHECK(lora_policy_for_frame(frame, n) != LORA_SEND_NEVER);
    n = hh_frame(LG_T_PONG, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 0);
    CHECK(lglh_type_allowed(frame, n));

    /* Refused: the grid's own state, and anything that would let one handheld rewrite it. */
    n = hh_frame(LG_T_GRID_STATE, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 20);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_GROUPS, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 20);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_GROUP_EDIT, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 20);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_TIME_SYNC, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 12);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_TIME_ZONE, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 12);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_REGISTER, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 12);
    CHECK(!lglh_type_allowed(frame, n));
    n = hh_frame(LG_T_NAME, LG_SCOPE_SYSTEM, 0, frame, sizeof(frame), 12);
    CHECK(!lglh_type_allowed(frame, n));

    /* Voice is refused here as well as by the send policy: two doors, not one. */
    n = hh_frame(LG_T_VOICE, LG_SCOPE_DIRECT, LG_FLAG_URGENT, frame, sizeof(frame), 40);
    CHECK(!lglh_type_allowed(frame, n));
    CHECK_EQ(lora_policy_for_frame(frame, n), LORA_SEND_NEVER);

    /* Nothing readable is not allowed. */
    CHECK(!lglh_type_allowed(NULL, 0));
    CHECK(!lglh_type_allowed(frame, 4));
}

void test_lora_hh(void)
{
    test_seal_round_trip();
    test_key_domains();
    test_malformed_and_indices();
    test_allowed_types();
}
