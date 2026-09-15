/*
 * End-to-end messaging scenarios on a simulated grid.
 *
 * Chain topology: node A(0) -- node B(1) -- node C(2); A and C cannot hear each other.
 *   Dad (client 0) and Ranger (client 3) on A, Alex (client 2) on B, Emma (client 1) on C.
 * These mirror acceptance steps 19-29 of the brief.
 */
#include <string.h>

#include "lg_test.h"
#include "sim.h"

#define DAD    0
#define EMMA   1
#define ALEX   2
#define RANGER 3
#define T0     1790000000u

#define TXT(s) (const uint8_t *)(s), strlen(s)

static lg_client_t *cl(sim_t *s, int i)
{
    return &s->clients[i].client;
}

static bool newest_is(sim_t *s, int client, const char *text)
{
    const lg_in_msg_t *m = lg_client_inbox(cl(s, client), 0);
    return m != NULL && m->len == strlen(text) && memcmp(m->text, text, m->len) == 0;
}

static sim_t *make_chain(void)
{
    sim_t *s = sim_create();
    if (s == NULL) {
        return NULL;
    }
    s->grid_time = T0;
    s->now_ms = 1000;
    sim_link(s, 0, 1, true);
    sim_link(s, 1, 2, true);
    sim_attach(s, DAD, 0);
    sim_attach(s, RANGER, 0);
    sim_attach(s, ALEX, 1);
    sim_attach(s, EMMA, 2);
    return s;
}

static void test_registration_and_presence(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    for (int i = 0; i < SIM_CLIENTS; i++) {
        CHECK(cl(s, i)->registered);
        CHECK(!lg_client_time_restricted(cl(s, i)));
        CHECK_EQ(s->clients[i].clock, T0);   /* handheld clocks synced from their node */
    }
    /* Node A knows Emma is online at node C, two hops away. */
    const lg_presence_entry_t *p = lg_node_presence(&s->nodes[0].node, LG_PROTO_EMMA);
    CHECK(p != NULL && p->state == LG_PRES_ONLINE && p->node == 2);
    /* Dad has Emma's public key from presence. */
    const lg_peer_t *peer = lg_client_peer(cl(s, DAD), LG_PROTO_EMMA);
    CHECK(peer != NULL && peer->has_key);
    CHECK(peer != NULL && memcmp(peer->pubkey, s->clients[EMMA].e2e.pub, LG_PUBKEY_LEN) == 0);
    sim_destroy(s);
}

static void test_direct_two_hops_encrypted(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Where are you?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);
    CHECK(newest_is(s, EMMA, "Where are you?"));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);

    int reply = lg_client_send_text(cl(s, EMMA), LG_SCOPE_DIRECT, LG_PROTO_DAD, 0, TXT("At the fire."));
    CHECK(reply >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->inbox_count, 1);
    CHECK(newest_is(s, DAD, "At the fire."));
    CHECK_EQ(cl(s, EMMA)->outbox[reply].state, LG_OUT_DELIVERED);

    CHECK_EQ(cl(s, ALEX)->inbox_count, 0);
    CHECK_EQ(cl(s, RANGER)->inbox_count, 0);

    /* Nothing that crossed the backbone contained the 1:1 plaintext. */
    CHECK(!sim_captured_contains(s, "Where are you?"));
    CHECK(!sim_captured_contains(s, "At the fire."));

    /* A captured 1:1 frame opens for Emma but not for Ranger, even with Dad's public key. */
    bool found = false;
    for (size_t i = 0; i < s->capture_count && !found; i++) {
        lg_env_t e;
        const sim_ev_t *ev = &s->capture[i];
        if (lg_env_decode(ev->data, ev->len, &e) != LG_OK || e.type != LG_T_TEXT || e.scope != LG_SCOPE_DIRECT ||
            e.origin_id != LG_PROTO_DAD) {
            continue;
        }
        found = true;
        uint8_t nonce[LG_E2E_NONCE_LEN], aad[LG_E2E_AAD_LEN], pt[LG_TEXT_MAX];
        lg_e2e_nonce(&e, nonce);
        lg_e2e_aad(&e, aad);
        CHECK(lg_e2e_open(&s->clients[RANGER].e2e, LG_PROTO_DAD, s->clients[DAD].e2e.pub, nonce, aad, sizeof(aad),
                          lg_frame_body(ev->data), e.body_len, pt) < 0);
        CHECK_EQ(lg_e2e_open(&s->clients[EMMA].e2e, LG_PROTO_DAD, s->clients[DAD].e2e.pub, nonce, aad, sizeof(aad),
                             lg_frame_body(ev->data), e.body_len, pt), 14);
    }
    CHECK(found);
    sim_destroy(s);
}

static void test_direct_same_node(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_RANGER, 0, TXT("Radio check"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, RANGER, "Radio check"));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);
    sim_destroy(s);
}

static void test_group(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Dinner at 7."));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Dinner at 7."));
    CHECK(newest_is(s, ALEX, "Dinner at 7."));
    CHECK_EQ(cl(s, RANGER)->inbox_count, 0);                 /* non-member */
    CHECK_EQ(cl(s, DAD)->inbox_count, 0);                    /* author does not receive own copy */
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_ACCEPTED);
    CHECK_EQ(cl(s, DAD)->outbox[slot].delivered_count, 2);

    int bad = lg_client_send_text(cl(s, RANGER), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Let me in"));
    CHECK(bad >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, RANGER)->outbox[bad].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, RANGER)->outbox[bad].reject_reason, LG_ACK_REJ_NOT_MEMBER);
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);

    int kids = lg_client_send_text(cl(s, EMMA), LG_SCOPE_GROUP, LG_PROTO_KIDS, 0, TXT("Swim?"));
    CHECK(kids >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, ALEX, "Swim?"));
    CHECK_EQ(cl(s, DAD)->inbox_count, 0);
    sim_destroy(s);
}

static void test_broadcast_and_rate_limit(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int slot = lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, 0, TXT("Storm coming. Return to camp."));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, DAD, "Storm coming. Return to camp."));
    CHECK(newest_is(s, EMMA, "Storm coming. Return to camp."));
    CHECK(newest_is(s, ALEX, "Storm coming. Return to camp."));
    CHECK_EQ(cl(s, RANGER)->inbox_count, 0);
    CHECK_EQ(cl(s, RANGER)->outbox[slot].delivered_count, 3);

    int again = lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, 0, TXT("Seriously, storm."));
    CHECK(again >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, RANGER)->outbox[again].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, RANGER)->outbox[again].reject_reason, LG_ACK_REJ_RATE);
    CHECK_EQ(cl(s, DAD)->inbox_count, 1);

    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    int urgent = lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("URGENT: lightning"));
    CHECK(urgent >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "URGENT: lightning"));

    s->now_ms += LG_BROADCAST_INTERVAL_MS + 1;
    int later = lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, 0, TXT("All clear."));
    CHECK(later >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, ALEX, "All clear."));
    sim_destroy(s);
}

static void test_duplicates_and_retransmit(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_link(s, 0, 2, true);           /* full mesh: every flood reaches nodes twice */
    s->duplicate_backbone = true;      /* and every backbone frame is delivered twice */

    int b = lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, 0, TXT("Once only"));
    CHECK(b >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);
    CHECK_EQ(cl(s, ALEX)->inbox_count, 1);
    CHECK_EQ(cl(s, RANGER)->inbox_count, 1);

    int d = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Retry me"));
    CHECK(d >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 2);

    /* Force a retransmission of the same message id, as after a lost acknowledgement. */
    uint32_t dups_before = s->nodes[0].node.stats.duplicates;
    cl(s, DAD)->outbox[d].state = LG_OUT_PENDING;
    s->now_ms += LG_RESEND_MS + 1;
    lg_client_tick(cl(s, DAD));
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 2);                   /* still shown once */
    CHECK(s->nodes[0].node.stats.duplicates > dups_before);
    CHECK_EQ(cl(s, DAD)->outbox[d].state, LG_OUT_ACCEPTED);  /* node re-acknowledged */
    sim_destroy(s);
}

static void test_offline_and_roam(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_detach(s, EMMA);
    const lg_presence_entry_t *p = lg_node_presence(&s->nodes[0].node, LG_PROTO_EMMA);
    CHECK(p != NULL && p->state == LG_PRES_OFFLINE);

    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("You there?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, DAD)->outbox[slot].reject_reason, LG_ACK_REJ_OFFLINE);

    sim_attach(s, EMMA, 0);            /* Emma walked over to node A */
    p = lg_node_presence(&s->nodes[2].node, LG_PROTO_EMMA);
    CHECK(p != NULL && p->state == LG_PRES_ONLINE && p->node == 0);

    int again = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Found you"));
    CHECK(again >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Found you"));
    CHECK_EQ(cl(s, DAD)->outbox[again].state, LG_OUT_DELIVERED);

    int from_alex = lg_client_send_text(cl(s, ALEX), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Over here"));
    CHECK(from_alex >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Over here"));
    sim_destroy(s);
}

static void test_time_rule(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Full power-off: grid time unset until the admin re-enters it. */
    s->grid_time = 0;
    sim_detach(s, DAD);
    s->clients[DAD].clock = 0;
    sim_attach(s, DAD, 0);
    CHECK(cl(s, DAD)->registered);
    CHECK(lg_client_time_restricted(cl(s, DAD)));
    CHECK_EQ(lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("hi")), LG_ERR_TIME);
    CHECK_EQ(lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("hi")), LG_ERR_TIME);

    int urgent = lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("Storm coming. Return to camp."));
    CHECK(urgent >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Storm coming. Return to camp."));   /* receiving and urgent still work */

    /* Admin re-enters time; Dad reconnects and is unrestricted again. */
    s->grid_time = T0;
    sim_detach(s, DAD);
    sim_attach(s, DAD, 0);
    CHECK(!lg_client_time_restricted(cl(s, DAD)));

    /* A handheld whose clock drifted past the tolerance is refused by its node. */
    s->clients[DAD].clock = T0 - (LG_TIME_TOLERANCE_S + 60);
    int late = lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Dinner moved"));
    CHECK(late >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[late].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, DAD)->outbox[late].reject_reason, LG_ACK_REJ_TIME);
    CHECK(!newest_is(s, ALEX, "Dinner moved"));
    sim_destroy(s);
}

static void test_node_refuses_unsafe_frames(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_node_t *a = &s->nodes[0].node;

    /* A 1:1 text without end-to-end encryption is refused. */
    lg_env_t e = {
        .type = LG_T_TEXT, .scope = LG_SCOPE_DIRECT, .target = LG_PROTO_EMMA,
        .origin_id = LG_PROTO_DAD, .origin_boot = 1, .origin_seq = 5000, .grid_time = T0,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, (const uint8_t *)"plaintext secret", 16, frame, sizeof(frame));
    uint32_t rejected = a->stats.rejected;
    lg_node_on_session_frame(a, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK(a->stats.rejected == rejected + 1);
    CHECK_EQ(cl(s, EMMA)->inbox_count, 0);
    CHECK(!sim_captured_contains(s, "plaintext secret"));

    /* Dad's session cannot author a message as Emma. */
    lg_env_t spoof = {
        .type = LG_T_TEXT, .scope = LG_SCOPE_BROADCAST, .target = LG_TARGET_ALL,
        .origin_id = LG_PROTO_EMMA, .origin_boot = 1, .origin_seq = 6000, .grid_time = T0,
    };
    len = lg_frame_build(&spoof, (const uint8_t *)"fake", 4, frame, sizeof(frame));
    rejected = a->stats.rejected;
    lg_node_on_session_frame(a, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK(a->stats.rejected == rejected + 1);
    CHECK_EQ(cl(s, ALEX)->inbox_count, 0);

    /* An unknown device cannot register. */
    lg_register_t reg = { .device = 99, .attach_epoch = 1 };
    uint8_t body[LG_REGISTER_LEN];
    size_t blen = lg_register_enc(&reg, body);
    lg_env_t re = { .type = LG_T_REGISTER, .scope = LG_SCOPE_SYSTEM, .origin_id = 99, .origin_boot = 1, .origin_seq = 1 };
    len = lg_frame_build(&re, body, blen, frame, sizeof(frame));
    uint32_t session = 0;
    lg_node_on_session_frame(a, &session, frame, (size_t)len);
    CHECK_EQ(session, 0);

    /* Garbage is counted and ignored. */
    uint32_t malformed = a->stats.malformed;
    lg_node_on_backbone_frame(a, 1, (const uint8_t *)"\x01\x02\x03", 3);
    CHECK(a->stats.malformed == malformed + 1);
    sim_destroy(s);
}

static void test_key_pinning(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    uint8_t pinned[LG_PUBKEY_LEN];
    memcpy(pinned, lg_client_peer(cl(s, EMMA), LG_PROTO_DAD)->pubkey, LG_PUBKEY_LEN);

    /* A presence update advertising a different key for Dad (for example from a rogue node). */
    lg_presence_t fake = { .device = LG_PROTO_DAD, .node = 0, .epoch = 0xFFFFFFF0u, .state = LG_PRES_ONLINE };
    memset(fake.pubkey, 0xAB, LG_PUBKEY_LEN);
    uint8_t body[LG_PRESENCE_LEN];
    size_t blen = lg_presence_enc(&fake, body);
    lg_env_t e = {
        .type = LG_T_PRESENCE_UPDATE, .scope = LG_SCOPE_SYSTEM, .target = LG_PROTO_DAD,
        .origin_id = lg_node_origin_id(2), .origin_boot = 1, .origin_seq = 90000,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, blen, frame, sizeof(frame));
    lg_client_on_frame(cl(s, EMMA), frame, (size_t)len);

    CHECK_EQ(s->clients[EMMA].key_changes, 1);
    CHECK(memcmp(lg_client_peer(cl(s, EMMA), LG_PROTO_DAD)->pubkey, pinned, LG_PUBKEY_LEN) == 0);

    /* Messages still flow under the pinned key. */
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Still me"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Still me"));
    sim_destroy(s);
}

static void test_diag_echo(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    CHECK_EQ(lg_node_send_diag(&s->nodes[0].node, TXT("grid ping")), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].diag_count, 0);     /* the sender does not report its own echo */
    CHECK_EQ(s->nodes[1].diag_count, 1);
    CHECK_EQ(s->nodes[1].diag_hops, 1);
    CHECK_EQ(s->nodes[2].diag_count, 1);     /* crossed two hops exactly once */
    CHECK_EQ(s->nodes[2].diag_hops, 2);

    sim_link(s, 0, 2, true);
    s->duplicate_backbone = true;
    CHECK_EQ(lg_node_send_diag(&s->nodes[0].node, TXT("mesh ping")), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].diag_count, 2);
    CHECK_EQ(s->nodes[2].diag_count, 2);
    CHECK_EQ(s->nodes[2].diag_hops, 1);      /* direct path now */
    CHECK_EQ(lg_node_send_diag(&s->nodes[0].node, (const uint8_t *)"", 0), LG_ERR_ARG);
    sim_destroy(s);
}

static void test_time_announce(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Every node restarted without time; Emma reconnects and is restricted. */
    s->grid_time = 0;
    sim_detach(s, EMMA);
    s->clients[EMMA].clock = 0;
    sim_attach(s, EMMA, 2);
    CHECK(lg_client_time_restricted(cl(s, EMMA)));

    /* The admin sets time on node A, which announces it grid-wide. */
    s->nodes[0].time = T0 + 5;
    lg_node_announce_time(&s->nodes[0].node, LG_TIME_AUTHORITATIVE);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].time, T0 + 5);
    CHECK_EQ(s->nodes[2].time, T0 + 5);
    CHECK_EQ(s->clients[EMMA].clock, T0 + 5);      /* pushed by node C after adopting */
    CHECK(!lg_client_time_restricted(cl(s, EMMA)));
    CHECK_EQ(s->clients[DAD].clock, T0 + 5);       /* pushed by node A directly */

    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Clock is back"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, DAD, "Clock is back"));
    sim_destroy(s);
}

void test_messaging(void)
{
    test_diag_echo();
    test_time_announce();
    test_registration_and_presence();
    test_direct_two_hops_encrypted();
    test_direct_same_node();
    test_group();
    test_broadcast_and_rate_limit();
    test_duplicates_and_retransmit();
    test_offline_and_roam();
    test_time_rule();
    test_node_refuses_unsafe_frames();
    test_key_pinning();
}
