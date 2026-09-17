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

}

/* Groups made at run time (D52): the edit rules and the two body layouts. */
static void test_groups(void)
{
    static lg_roster_t r;
    lg_roster_init_prototype(&r);
    CHECK_EQ(r.groups.count, 0);                        /* no built-in groups */
    CHECK(lg_roster_user(&r, 99) == NULL);

    /* Who may announce (D56): everyone until the admin page says otherwise. */
    CHECK_EQ(r.groups.announcers, LG_ANNOUNCE_EVERYONE);
    CHECK(lg_roster_may_announce(&r, 1));
    CHECK(lg_roster_may_announce(&r, 4));
    CHECK(!lg_roster_may_announce(&r, 99));             /* not in the roster */
    lg_group_edit_t who = { .op = LG_GROUP_ANNOUNCERS, .members = (1u << 0) | (1u << 3) };
    CHECK_EQ(lg_groups_apply_edit(&r, 1, &who, 0), LG_ACK_REJ_NOT_ALLOWED);   /* a handheld may not */
    CHECK_EQ(r.groups.seq, 0);
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &who, 3), 0);  /* the admin page may */
    CHECK_EQ(r.groups.seq, 1);
    CHECK_EQ(r.groups.author, 3);
    CHECK(lg_roster_may_announce(&r, 1));
    CHECK(!lg_roster_may_announce(&r, 2));
    CHECK(lg_roster_may_announce(&r, 4));
    lg_group_edit_t ghost = { .op = LG_GROUP_ANNOUNCERS, .members = 1u << 20 };
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &ghost, 0), LG_ACK_REJ_INVALID);     /* no such user */
    who.members = LG_ANNOUNCE_EVERYONE;
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &who, 0), 0);  /* back to everyone */
    CHECK(lg_roster_may_announce(&r, 2));
    lg_roster_init_prototype(&r);

    lg_group_edit_t e = { .op = LG_GROUP_CREATE, .members = 1u << 1 };
    memcpy(e.name, "Cooks", 5);
    CHECK_EQ(lg_groups_apply_edit(&r, 1, &e, 2), 0);   /* device 1 (user 0) makes it */
    CHECK_EQ(r.groups.count, 1);
    CHECK_EQ(r.groups.groups[0].id, 1);
    CHECK_EQ(r.groups.seq, 1);
    CHECK_EQ(r.groups.author, 2);
    CHECK(lg_roster_is_member(&r, 1, 1));               /* the maker is always in it */
    CHECK(lg_roster_is_member(&r, 2, 1));
    CHECK(!lg_roster_is_member(&r, 3, 1));
    CHECK(!lg_roster_is_member(&r, 2, 99));

    lg_group_edit_t u = { .op = LG_GROUP_UPDATE, .id = 1, .members = 1u << 3 };
    memcpy(u.name, "Kitchen", 7);
    CHECK_EQ(lg_groups_apply_edit(&r, 3, &u, 0), LG_ACK_REJ_NOT_MEMBER);   /* device 3 is not in it */
    CHECK_EQ(r.groups.seq, 1);                          /* a refusal changes nothing */
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &u, 0), 0);    /* the admin may */
    CHECK(strcmp(r.groups.groups[0].name, "Kitchen") == 0);
    CHECK(lg_roster_is_member(&r, 4, 1));
    CHECK(!lg_roster_is_member(&r, 1, 1));

    lg_group_edit_t bad = { .op = LG_GROUP_CREATE, .members = 1u << 20 };
    memcpy(bad.name, "Ghosts", 6);
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &bad, 0), LG_ACK_REJ_INVALID);    /* no such user */
    bad.members = 0;
    memset(bad.name, 0, sizeof(bad.name));
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &bad, 0), LG_ACK_REJ_INVALID);    /* empty name */
    u.members = 0;
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &u, 0), LG_ACK_REJ_INVALID);      /* empty group */
    lg_group_edit_t gone = { .op = LG_GROUP_DELETE, .id = 7 };
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &gone, 0), LG_ACK_REJ_UNKNOWN_TARGET);

    for (int i = 0; i < (int)LG_MAX_GROUPS - 1; i++) {
        CHECK_EQ(lg_groups_apply_edit(&r, 0, &e, 0), 0);
    }
    CHECK_EQ(r.groups.count, LG_MAX_GROUPS);
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &e, 0), LG_ACK_REJ_INVALID);      /* table full */

    /* The table round-trips, and exact length and field checks hold. */
    static uint8_t buf[LG_GROUPS_MAX_LEN + 1];
    static lg_groups_t back;
    size_t n = lg_groups_enc(&r.groups, buf);
    CHECK_EQ(n, LG_GROUPS_MAX_LEN);
    CHECK(lg_groups_dec(buf, n, &back));
    CHECK(memcmp(&back, &r.groups, sizeof(back)) == 0);
    CHECK(!lg_groups_dec(buf, n - 1, &back));
    buf[LG_GROUPS_HEAD_LEN + LG_GROUP_ENTRY_LEN] = buf[LG_GROUPS_HEAD_LEN];   /* duplicate id */
    buf[LG_GROUPS_HEAD_LEN + LG_GROUP_ENTRY_LEN + 1] = buf[LG_GROUPS_HEAD_LEN + 1];
    CHECK(!lg_groups_dec(buf, n, &back));

    /* Removing the first group: it is reported, ids are never reused, and the rest move up. */
    static lg_groups_t before;
    before = r.groups;
    lg_group_edit_t del = { .op = LG_GROUP_DELETE, .id = 1 };
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &del, 0), 0);
    uint16_t removed[LG_MAX_GROUPS];
    CHECK_EQ(lg_groups_removed(&before, &r.groups, removed), 1);
    CHECK_EQ(removed[0], 1);
    CHECK_EQ(lg_groups_apply_edit(&r, 0, &e, 0), 0);
    CHECK_EQ(r.groups.groups[r.groups.count - 1u].id, 9);
    CHECK(lg_groups_newer(&r.groups, &before));
    CHECK(!lg_groups_newer(&before, &r.groups));

    lg_group_edit_t w = { .op = LG_GROUP_UPDATE, .id = 5, .members = 3 };
    memcpy(w.name, "Hikers", 6);
    uint8_t eb[LG_GROUP_EDIT_LEN];
    CHECK_EQ(lg_group_edit_enc(&w, eb), LG_GROUP_EDIT_LEN);
    lg_group_edit_t w2;
    CHECK(lg_group_edit_dec(eb, LG_GROUP_EDIT_LEN, &w2));
    CHECK(memcmp(&w, &w2, sizeof(w)) == 0);
    CHECK(!lg_group_edit_dec(eb, LG_GROUP_EDIT_LEN - 1, &w2));
    eb[7 + 10] = 'x';   /* bytes after the name's NUL must be padding */
    CHECK(!lg_group_edit_dec(eb, LG_GROUP_EDIT_LEN, &w2));
}


void test_core(void)
{
    test_envelope();
    test_dedup();
    test_utf8();
    test_bodies();
    test_groups();
}
