#include <string.h>

#include "lg_body.h"
#include "lg_dedup.h"
#include "lg_envelope.h"
#include "lg_roster.h"
#include "lg_test.h"

static void test_envelope(void)
{
    lg_env_t e = {
        .type = LG_T_TEXT, .flags = LG_FLAG_URGENT, .scope = LG_SCOPE_GROUP, .ttl = 3,
        .origin_id = 7, .origin_boot = 42, .origin_seq = 19, .target = 1,
        .grid_time = 1790000000u, .origin_node = 2,
    };
    const uint8_t body[] = "Dinner at 7.";
    uint8_t buf[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, sizeof(body) - 1, buf, sizeof(buf));
    CHECK_EQ(len, (int)(LG_ENV_SIZE + sizeof(body) - 1));

    lg_env_t d;
    CHECK_EQ(lg_env_decode(buf, (size_t)len, &d), LG_OK);
    CHECK_EQ(d.type, LG_T_TEXT);
    CHECK_EQ(d.flags, LG_FLAG_URGENT);
    CHECK_EQ(d.scope, LG_SCOPE_GROUP);
    CHECK_EQ(d.ttl, 3);
    CHECK_EQ(d.body_len, sizeof(body) - 1);
    CHECK_EQ(d.origin_id, 7);
    CHECK_EQ(d.origin_boot, 42);
    CHECK_EQ(d.origin_seq, 19);
    CHECK_EQ(d.target, 1);
    CHECK_EQ(d.grid_time, 1790000000u);
    CHECK_EQ(d.origin_node, 2);
    CHECK(memcmp(lg_frame_body(buf), body, sizeof(body) - 1) == 0);

    CHECK_EQ(lg_env_decode(buf, 10, &d), LG_ERR_SHORT);
    CHECK_EQ(lg_env_decode(buf, (size_t)len - 1, &d), LG_ERR_LENGTH);

    uint8_t bad[LG_FRAME_MAX];
    memcpy(bad, buf, (size_t)len);
    bad[0] = 0x11;                                  /* major version 1 */
    CHECK_EQ(lg_env_decode(bad, (size_t)len, &d), LG_ERR_VERSION);

    memcpy(bad, buf, (size_t)len);
    bad[31] = 1;                                    /* reserved */
    CHECK_EQ(lg_env_decode(bad, (size_t)len, &d), LG_ERR_RESERVED);

    lg_env_t b = e;
    b.scope = LG_SCOPE_BROADCAST;
    b.target = 5;                                   /* broadcast must target all */
    len = lg_frame_build(&b, body, 4, buf, sizeof(buf));
    CHECK_EQ(lg_env_decode(buf, (size_t)len, &d), LG_ERR_SCOPE);

    lg_env_t s = e;
    s.scope = LG_SCOPE_SYSTEM;                      /* text cannot be SYSTEM scope */
    len = lg_frame_build(&s, body, 4, buf, sizeof(buf));
    CHECK_EQ(lg_env_decode(buf, (size_t)len, &d), LG_ERR_SCOPE);

    lg_env_t z = e;
    z.origin_seq = 0;
    len = lg_frame_build(&z, body, 4, buf, sizeof(buf));
    CHECK_EQ(lg_env_decode(buf, (size_t)len, &d), LG_ERR_ID);

    uint8_t big[LG_BODY_MAX + 1];
    memset(big, 'a', sizeof(big));
    CHECK_EQ(lg_frame_build(&e, big, sizeof(big), buf, sizeof(buf)), LG_ERR_LENGTH);

    /* AAD must not change when a node rewrites ttl, origin_node, or RELAYED. */
    uint8_t aad1[LG_E2E_AAD_LEN], aad2[LG_E2E_AAD_LEN];
    lg_e2e_aad(&e, aad1);
    lg_env_t r = e;
    r.ttl = 1;
    r.origin_node = 9;
    r.flags |= LG_FLAG_RELAYED;
    lg_e2e_aad(&r, aad2);
    CHECK(memcmp(aad1, aad2, sizeof(aad1)) == 0);
    r.target = 2;
    lg_e2e_aad(&r, aad2);
    CHECK(memcmp(aad1, aad2, sizeof(aad1)) != 0);
}

static void test_dedup(void)
{
    lg_dedup_entry_t slots[2];
    lg_dedup_t d;
    lg_dedup_init(&d, slots, 2);

    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 10), LG_DEDUP_NEW);
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 10), LG_DEDUP_DUPLICATE);
    CHECK_EQ(lg_dedup_check(&d, 1, 1, 9), LG_DEDUP_NEW);       /* check does not record */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 9), LG_DEDUP_NEW);        /* out of order, inside window */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 9), LG_DEDUP_DUPLICATE);
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 200), LG_DEDUP_NEW);      /* jump ahead */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 10), LG_DEDUP_STALE);     /* now below the 128 window */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 73), LG_DEDUP_NEW);       /* offset 127: last slot of window */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 73), LG_DEDUP_DUPLICATE);
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 136), LG_DEDUP_NEW);      /* offset 64: crosses into hi word */
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 136), LG_DEDUP_DUPLICATE);
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 0), LG_DEDUP_STALE);

    CHECK_EQ(lg_dedup_mark(&d, 1, 2, 1), LG_DEDUP_NEW);        /* new boot resets window */
    CHECK_EQ(lg_dedup_mark(&d, 1, 2, 1), LG_DEDUP_DUPLICATE);
    CHECK_EQ(lg_dedup_mark(&d, 1, 1, 500), LG_DEDUP_STALE);    /* older boot */

    CHECK_EQ(lg_dedup_mark(&d, 2, 1, 1), LG_DEDUP_NEW);
    CHECK_EQ(lg_dedup_mark(&d, 1, 2, 2), LG_DEDUP_NEW);        /* touch author 1 so author 2 is LRU */
    CHECK_EQ(lg_dedup_mark(&d, 3, 1, 1), LG_DEDUP_NEW);        /* evicts author 2 */
    CHECK_EQ(lg_dedup_mark(&d, 1, 2, 2), LG_DEDUP_DUPLICATE);  /* author 1 kept */
    CHECK_EQ(lg_dedup_check(&d, 2, 1, 1), LG_DEDUP_NEW);       /* author 2 forgotten: documented limit */
}

static void test_utf8(void)
{
    CHECK(lg_utf8_valid((const uint8_t *)"Where are you?", 14));
    CHECK(lg_utf8_valid((const uint8_t *)"h\xc3\xa9llo", 6));                 /* é */
    CHECK(lg_utf8_valid((const uint8_t *)"\xf0\x9f\x94\xa5", 4));             /* fire emoji */
    CHECK(!lg_utf8_valid((const uint8_t *)"\xc0\x80", 2));                    /* overlong NUL */
    CHECK(!lg_utf8_valid((const uint8_t *)"\xed\xa0\x80", 3));                /* surrogate */
    CHECK(!lg_utf8_valid((const uint8_t *)"\xe2\x82", 2));                    /* truncated */
    CHECK(!lg_utf8_valid((const uint8_t *)"\xf4\x90\x80\x80", 4));            /* above U+10FFFF */
    CHECK(!lg_utf8_valid((const uint8_t *)"a\0b", 3));                        /* NUL */
    CHECK(!lg_text_valid((const uint8_t *)"", 0));
    uint8_t long_text[LG_TEXT_MAX + 1];
    memset(long_text, 'x', sizeof(long_text));
    CHECK(lg_text_valid(long_text, LG_TEXT_MAX));
    CHECK(!lg_text_valid(long_text, LG_TEXT_MAX + 1));
}

static void test_bodies(void)
{
    /* TIME_SYNC carries milliseconds and stratum; the 5-byte form still decodes as unknown. */
    {
        uint8_t tb[LG_TIME_SYNC_LEN];
        lg_time_sync_t t = { .grid_time = 1789590000u, .millis = 987, .quality = LG_TIME_CARRIED, .stratum = 2 };
        CHECK_EQ(lg_time_sync_enc(&t, tb), LG_TIME_SYNC_LEN);
        lg_time_sync_t t2;
        CHECK(lg_time_sync_dec(tb, LG_TIME_SYNC_LEN, &t2));
        CHECK_EQ(t2.grid_time, 1789590000u);
        CHECK_EQ(t2.millis, 987);
        CHECK_EQ(t2.quality, LG_TIME_CARRIED);
        CHECK_EQ(t2.stratum, 2);
        uint8_t v1[LG_TIME_SYNC_LEN_V1];                          /* old senders: seconds, then quality */
        lg_wr32(v1, 1789590000u);
        v1[4] = LG_TIME_CARRIED;
        CHECK(lg_time_sync_dec(v1, LG_TIME_SYNC_LEN_V1, &t2));
        CHECK_EQ(t2.grid_time, 1789590000u);
        CHECK_EQ(t2.millis, 0);
        CHECK_EQ(t2.stratum, LG_STRATUM_UNKNOWN);
        lg_wr16(tb + 4, 1000u);
        CHECK(!lg_time_sync_dec(tb, LG_TIME_SYNC_LEN, &t2));     /* milliseconds past the second */
        CHECK(!lg_time_sync_dec(tb, LG_TIME_SYNC_LEN - 1u, &t2));
    }

    uint8_t buf[64];
    lg_register_t r = { .device = 2, .attach_epoch = 4097, .client_time = 123, .caps = 5 };
    for (size_t i = 0; i < LG_PUBKEY_LEN; i++) {
        r.pubkey[i] = (uint8_t)i;
    }
    CHECK_EQ(lg_register_enc(&r, buf), LG_REGISTER_LEN);
    lg_register_t r2;
    CHECK(lg_register_dec(buf, LG_REGISTER_LEN, &r2));
    CHECK_EQ(r2.device, 2);
    CHECK_EQ(r2.attach_epoch, 4097);
    CHECK(memcmp(r2.pubkey, r.pubkey, LG_PUBKEY_LEN) == 0);
    CHECK(!lg_register_dec(buf, LG_REGISTER_LEN - 1, &r2));

    lg_msg_ack_t a = { .author = 1, .boot = 2, .seq = 3, .status = LG_ACK_DELIVERED };
    CHECK_EQ(lg_msg_ack_enc(&a, buf), LG_MSG_ACK_LEN);
    lg_msg_ack_t a2;
    CHECK(lg_msg_ack_dec(buf, LG_MSG_ACK_LEN, &a2));
    CHECK_EQ(a2.seq, 3);
    buf[12] = 99;
    CHECK(!lg_msg_ack_dec(buf, LG_MSG_ACK_LEN, &a2));

    const lg_roster_t *ro = lg_roster_prototype();
    CHECK(lg_roster_is_member(ro, LG_PROTO_DAD, LG_PROTO_FAMILY));
    CHECK(lg_roster_is_member(ro, LG_PROTO_DAD, LG_PROTO_LEADERS));
    CHECK(!lg_roster_is_member(ro, LG_PROTO_RANGER, LG_PROTO_FAMILY));
    CHECK(!lg_roster_is_member(ro, LG_PROTO_EMMA, 99));
    CHECK(lg_roster_user(ro, 99) == NULL);
}

void test_core(void)
{
    test_envelope();
    test_dedup();
    test_utf8();
    test_bodies();
}
