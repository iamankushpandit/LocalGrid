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


/* Who may announce (D56): the admin page's list, enforced by the handheld and by the AP. */
static void test_announce_permission(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* The admin page on AP A allows only Ranger; every AP and handheld hears it. */
    lg_group_edit_t who = { .op = LG_GROUP_ANNOUNCERS, .members = 1u << RANGER };
    CHECK_EQ(lg_node_edit_groups(&s->nodes[0].node, &who), 0);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].roster.groups.announcers, 1u << RANGER);
    CHECK_EQ(s->nodes[2].roster.groups.announcers, 1u << RANGER);

    /* Dad's handheld refuses an announcement before it leaves, and the urgent one still goes. */
    CHECK_EQ(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, 0, TXT("Cake in the tent")), LG_ERR_DENIED);
    int urgent = lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("URGENT: bear"));
    CHECK(urgent >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "URGENT: bear"));

    /* Ranger is on the list, so an ordinary announcement is delivered. */
    s->now_ms += LG_BROADCAST_INTERVAL_MS + 1;
    int ok = lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, 0, TXT("Trail closed"));
    CHECK(ok >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, ALEX, "Trail closed"));

    /* A handheld holding an older table still tries: its AP refuses the message itself. */
    s->clients[DAD].roster.groups.announcers = LG_ANNOUNCE_EVERYONE;
    s->now_ms += LG_BROADCAST_INTERVAL_MS + 1;
    int sneaky = lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, 0, TXT("Cake anyway"));
    CHECK(sneaky >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[sneaky].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, DAD)->outbox[sneaky].reject_reason, LG_ACK_REJ_NOT_ALLOWED);
    CHECK(!newest_is(s, EMMA, "Cake anyway"));

    /* Back to everyone, and Dad's announcement is carried again. */
    who.members = LG_ANNOUNCE_EVERYONE;
    CHECK_EQ(lg_node_edit_groups(&s->nodes[1].node, &who), 0);
    CHECK(sim_pump(s));
    s->now_ms += LG_BROADCAST_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, 0, TXT("Cake now")) >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Cake now"));
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

/* D53: an AP that restarted with no clock takes grid time back from a handheld that kept it. */
static void test_time_from_handheld(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Every AP lost its clock (all restarted); the handhelds kept theirs from the grid. */
    s->grid_time = 0;
    for (int i = 0; i < SIM_NODES; i++) {
        s->nodes[i].time = 0;
    }
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 2);
    CHECK_EQ(s->nodes[2].client_time_count, 1u);
    CHECK_EQ(s->nodes[2].time, T0);                       /* taken from Emma's clock */
    CHECK_EQ(s->nodes[2].time_stratum, LG_STRATUM_UNKNOWN);
    CHECK(!lg_client_time_restricted(cl(s, EMMA)));       /* the ack carried it straight back */

    /* An AP that already has time is never offered a handheld's. */
    sim_detach(s, ALEX);
    sim_attach(s, ALEX, 2);
    CHECK_EQ(s->nodes[2].client_time_count, 1u);

    /* A handheld whose clock the grid never set offers nothing. */
    sim_detach(s, DAD);
    s->clients[DAD].clock = 0;
    sim_attach(s, DAD, 0);
    CHECK_EQ(s->nodes[0].client_time_count, 0u);
    CHECK_EQ(s->nodes[0].time, 0u);
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

/* D45: grid state floods from one AP to every other exactly once, opaque and intact. */
static void test_grid_state(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    uint8_t body[LG_GRID_STATE_MAX];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (uint8_t)(i * 7u + 1u);
    }
    CHECK_EQ(lg_node_announce_grid_state(&s->nodes[0].node, body, 147), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].grid_state_count, 0);   /* the author does not hear its own */
    CHECK_EQ(s->nodes[1].grid_state_count, 1);
    CHECK_EQ(s->nodes[2].grid_state_count, 1);   /* two hops, once */
    CHECK_EQ(s->nodes[2].grid_state_origin, 0);
    CHECK_EQ(s->nodes[2].grid_state_len, 147u);
    CHECK(memcmp(s->nodes[2].grid_state_last, body, 147) == 0);

    /* A second path and duplicated backbone frames still report it once per node. */
    sim_link(s, 0, 2, true);
    s->duplicate_backbone = true;
    CHECK_EQ(lg_node_announce_grid_state(&s->nodes[2].node, body, sizeof(body)), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].grid_state_count, 1);
    CHECK_EQ(s->nodes[1].grid_state_count, 2);
    CHECK_EQ(s->nodes[2].grid_state_count, 1);
    CHECK_EQ(s->nodes[0].grid_state_len, (size_t)LG_GRID_STATE_MAX);

    /* Empty and oversize bodies are refused at the author. */
    CHECK_EQ(lg_node_announce_grid_state(&s->nodes[0].node, body, 0), LG_ERR_ARG);
    CHECK_EQ(lg_node_announce_grid_state(&s->nodes[0].node, body, LG_GRID_STATE_MAX + 1u), LG_ERR_ARG);

    /* An oversize body forged onto the backbone is counted malformed and goes no further. */
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.major = LG_PROTO_MAJOR;
    e.minor = LG_PROTO_MINOR;
    e.type = LG_T_GRID_STATE;
    e.scope = LG_SCOPE_SYSTEM;
    e.ttl = LG_TTL_DEFAULT;
    e.origin_id = LG_NODE_ID_BASE | 1u;
    e.origin_node = 1;
    e.origin_boot = 1;
    e.origin_seq = 900;
    static uint8_t big[LG_GRID_STATE_MAX + 1u];
    static uint8_t frame[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, big, sizeof(big), frame, sizeof(frame));
    CHECK(flen > 0);
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    uint32_t forwarded = s->nodes[0].node.stats.forwarded;
    lg_node_on_backbone_frame(&s->nodes[0].node, 1, frame, (size_t)flen);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 1u);
    CHECK_EQ(s->nodes[0].node.stats.forwarded, forwarded);
    CHECK_EQ(s->nodes[0].grid_state_count, 1);
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
    CHECK_EQ(s->nodes[2].time_millis, 250);   /* milliseconds travel with the time */
    CHECK_EQ(s->nodes[2].time_stratum, 0);    /* the stratum of the node that announced it, forwarded unchanged */
    CHECK_EQ(s->clients[EMMA].clock, T0 + 5);      /* pushed by node C after adopting */
    CHECK(!lg_client_time_restricted(cl(s, EMMA)));
    CHECK_EQ(s->clients[DAD].clock, T0 + 5);       /* pushed by node A directly */

    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Clock is back"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, DAD, "Clock is back"));
    sim_destroy(s);
}

/* Keepalive: a handheld's PING is answered by its own node, and only to that session. */
/*
 * A read report travels the same path as a delivery report: recipient -> node -> author, with
 * the message named by (author, boot, seq). It only moves a 1:1 message, it is counted once
 * per reader however many times it arrives, and it never moves backwards to delivered.
 */
static void test_read_receipt(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Are you there?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);
    CHECK_EQ(cl(s, DAD)->outbox[slot].read_count, 0);

    /* Emma's handheld keeps the identity of what it received, which is what it reports. */
    const lg_in_msg_t *in = lg_client_inbox(cl(s, EMMA), 0);
    CHECK(in != NULL);
    CHECK_EQ(in->author, LG_PROTO_DAD);
    CHECK(in->seq == cl(s, DAD)->outbox[slot].seq);
    CHECK(in->boot == cl(s, DAD)->outbox[slot].boot);

    CHECK(lg_client_mark_read(cl(s, EMMA), in->author, in->boot, in->seq));
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_READ);
    CHECK_EQ(cl(s, DAD)->outbox[slot].read_count, 1);

    /* Reported twice, counted once, and still read. */
    CHECK(lg_client_mark_read(cl(s, EMMA), in->author, in->boot, in->seq));
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[slot].read_count, 1);
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_READ);

    /* A handheld does not report its own message read. */
    CHECK(!lg_client_mark_read(cl(s, EMMA), LG_PROTO_EMMA, 1, 1));

    /* A group message carries no read state: one report per member would say little. */
    int g = lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Dinner is ready."));
    CHECK(g >= 0);
    CHECK(sim_pump(s));
    const lg_in_msg_t *gin = lg_client_inbox(cl(s, EMMA), 0);
    CHECK(gin != NULL && gin->scope == LG_SCOPE_GROUP);
    CHECK(lg_client_mark_read(cl(s, EMMA), gin->author, gin->boot, gin->seq));
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[g].read_count, 1);
    CHECK(cl(s, DAD)->outbox[g].state != LG_OUT_READ);
    sim_destroy(s);
}

static bool name_is(const lg_name_t *n, const char *text)
{
    return n != NULL && n->len == strlen(text) && memcmp(n->text, text, n->len) == 0 && n->text[n->len] == 0;
}

/* Sends a NAME frame on Dad's session at node A, as a handheld would, with any contents. */
static void dad_sends_name_frame(sim_t *s, const lg_name_t *name, size_t body_len, uint32_t seq)
{
    uint8_t body[LG_NAME_LEN_MAX + 4];
    memset(body, 0, sizeof(body));
    (void)lg_name_enc(name, body);
    lg_env_t e = {
        .type = LG_T_NAME, .scope = LG_SCOPE_SYSTEM, .target = name->device,
        .origin_id = LG_PROTO_DAD, .origin_boot = 1, .origin_seq = seq,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, body_len, frame, sizeof(frame));
    CHECK(len > 0);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
}

/* D50: a handheld names itself; the newest name reaches every AP and handheld and stays there. */
static void test_names(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }

    /* Wire format: exact lengths, a real device and version, and UTF-8 only. */
    lg_name_t w = { .device = LG_PROTO_DAD, .version = 7, .len = 4 };
    memcpy(w.text, "Papa", 4);
    uint8_t wire[LG_NAME_LEN_MAX + 1];
    lg_name_t back;
    size_t wl = lg_name_enc(&w, wire);
    CHECK_EQ(wl, 13u);
    CHECK(lg_name_dec(wire, wl, &back) && back.version == 7 && name_is(&back, "Papa"));
    CHECK(!lg_name_dec(wire, wl - 1, &back));
    CHECK(!lg_name_dec(wire, wl + 1, &back));
    wire[9] = 0xFF;
    CHECK(!lg_name_dec(wire, wl, &back));                               /* not UTF-8 */
    CHECK_EQ(lg_client_set_name(cl(s, DAD), (const uint8_t *)"", 0), LG_ERR_ARG);
    CHECK_EQ(lg_client_set_name(cl(s, DAD), TXT("123456789012345678901234")), LG_ERR_ARG);   /* 24 bytes */

    /* Nobody has chosen a name: the roster name applies everywhere. */
    CHECK(lg_client_name(cl(s, EMMA), LG_PROTO_DAD) == NULL);
    CHECK(lg_node_name(&s->nodes[2].node, LG_PROTO_DAD) == NULL);

    /* Dad renames himself on node A; Emma on node C, two hops away, and every AP get it. */
    CHECK_EQ(lg_client_set_name(cl(s, DAD), TXT("Papa")), LG_OK);
    CHECK(sim_pump(s));
    const lg_name_t *own = lg_client_name(cl(s, DAD), LG_PROTO_DAD);
    CHECK(name_is(own, "Papa"));
    CHECK(own != NULL && own->version >= (1u << 12));
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(name_is(lg_node_name(&s->nodes[i].node, LG_PROTO_DAD), "Papa"));
        CHECK_EQ(s->nodes[i].name_count, 1u);
    }
    for (int i = 0; i < SIM_CLIENTS; i++) {
        CHECK(name_is(lg_client_name(cl(s, i), LG_PROTO_DAD), "Papa"));
        CHECK_EQ(s->clients[i].name_events, 1u);
    }

    /* A second rename gets a higher version and replaces the first everywhere. */
    uint32_t v1 = own != NULL ? own->version : 0;
    CHECK_EQ(lg_client_set_name(cl(s, DAD), TXT("Dad 🔥")), LG_OK);
    CHECK(sim_pump(s));
    own = lg_client_name(cl(s, DAD), LG_PROTO_DAD);
    CHECK(own != NULL && own->version > v1);
    CHECK(name_is(lg_client_name(cl(s, EMMA), LG_PROTO_DAD), "Dad 🔥"));
    CHECK_EQ(s->nodes[2].name_count, 2u);

    /* Re-registering resends the same version: nothing is kept again, nobody is told again. */
    sim_detach(s, DAD);
    sim_attach(s, DAD, 0);
    CHECK(cl(s, DAD)->registered);
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK_EQ(s->nodes[i].name_count, 2u);
    }
    CHECK_EQ(s->clients[EMMA].name_events, 2u);

    /* A duplicated backbone frame is taken once. */
    s->duplicate_backbone = true;
    CHECK_EQ(lg_client_set_name(cl(s, DAD), TXT("Dad")), LG_OK);
    CHECK(sim_pump(s));
    s->duplicate_backbone = false;
    CHECK_EQ(s->nodes[1].name_count, 3u);
    CHECK_EQ(s->nodes[2].name_count, 3u);

    /* A handheld that was away learns every name held when it registers. */
    sim_detach(s, EMMA);
    CHECK_EQ(lg_client_set_name(cl(s, ALEX), TXT("Alex")), LG_OK);
    CHECK(sim_pump(s));
    memset(cl(s, EMMA)->names, 0, sizeof(cl(s, EMMA)->names));   /* as if Emma had restarted */
    sim_attach(s, EMMA, 2);
    CHECK(name_is(lg_client_name(cl(s, EMMA), LG_PROTO_ALEX), "Alex"));
    CHECK(name_is(lg_client_name(cl(s, EMMA), LG_PROTO_DAD), "Dad"));

    /* A handheld that lost its own name (reflashed) is handed back the grid's newer copy. */
    memset(cl(s, ALEX)->names, 0, sizeof(cl(s, ALEX)->names));
    lg_name_t old = { .device = LG_PROTO_ALEX, .version = 1, .len = 3 };
    memcpy(old.text, "Old", 3);
    CHECK(lg_client_restore_name(cl(s, ALEX), &old));
    sim_detach(s, ALEX);
    sim_attach(s, ALEX, 1);
    CHECK(name_is(lg_client_name(cl(s, ALEX), LG_PROTO_ALEX), "Alex"));
    CHECK(name_is(lg_node_name(&s->nodes[0].node, LG_PROTO_ALEX), "Alex"));   /* the old one went nowhere */

    /* A split grid: a rename on one side reaches the other when the link comes back. */
    sim_link(s, 1, 2, false);
    CHECK_EQ(lg_client_set_name(cl(s, RANGER), TXT("Ranger Rick")), LG_OK);
    CHECK(sim_pump(s));
    CHECK(name_is(lg_node_name(&s->nodes[1].node, LG_PROTO_RANGER), "Ranger Rick"));
    CHECK(lg_client_name(cl(s, EMMA), LG_PROTO_RANGER) == NULL);
    sim_link(s, 1, 2, true);
    CHECK(name_is(lg_node_name(&s->nodes[2].node, LG_PROTO_RANGER), "Ranger Rick"));
    CHECK(name_is(lg_client_name(cl(s, EMMA), LG_PROTO_RANGER), "Ranger Rick"));

    /* Restoring from flash takes only a newer name and sends nothing. */
    lg_node_t *c_node = &s->nodes[2].node;
    uint32_t forwarded = c_node->stats.forwarded;
    CHECK(!lg_node_restore_name(c_node, &old));   /* older than the "Alex" it holds */
    lg_name_t newer = *lg_node_name(c_node, LG_PROTO_ALEX);
    newer.version++;
    memcpy(newer.text, "Alexa", 5);
    newer.len = 5;
    CHECK(lg_node_restore_name(c_node, &newer));
    CHECK(name_is(lg_node_name(c_node, LG_PROTO_ALEX), "Alexa"));
    CHECK_EQ(c_node->stats.forwarded, forwarded);

    /* Refusals: naming another handheld, a body of the wrong length, a scope other than system. */
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    lg_name_t spoof = { .device = LG_PROTO_EMMA, .version = 0x7FFFFFFF, .len = 4 };
    memcpy(spoof.text, "Evil", 4);
    dad_sends_name_frame(s, &spoof, 13, 9001);
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected + 1);
    CHECK(lg_node_name(&s->nodes[0].node, LG_PROTO_EMMA) == NULL);
    CHECK(lg_client_name(cl(s, ALEX), LG_PROTO_EMMA) == NULL);

    lg_name_t mine = { .device = LG_PROTO_DAD, .version = 0x7FFFFFFF, .len = 4 };
    memcpy(mine.text, "Long", 4);
    dad_sends_name_frame(s, &mine, 14, 9002);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 1);
    CHECK(name_is(lg_node_name(&s->nodes[0].node, LG_PROTO_DAD), "Dad"));
    sim_destroy(s);
}

static void test_ping_pong(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    s->now_ms = 5000;
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 0u);
    lg_client_ping(cl(s, EMMA));
    sim_pump(s);
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 5000u);
    CHECK_EQ(cl(s, DAD)->last_pong_ms, 0u);
    CHECK_EQ(cl(s, ALEX)->last_pong_ms, 0u);

    /* A handheld that is not connected sends nothing and records nothing. */
    sim_detach(s, EMMA);
    lg_client_disconnected(cl(s, EMMA));
    s->now_ms = 9000;
    lg_client_ping(cl(s, EMMA));
    sim_pump(s);
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 5000u);
    sim_destroy(s);
}

static bool tables_agree(sim_t *s, uint32_t seq, uint8_t count)
{
    bool ok = true;
    for (int i = 0; i < SIM_NODES; i++) {
        ok = ok && s->nodes[i].roster.groups.seq == seq && s->nodes[i].roster.groups.count == count;
    }
    for (int i = 0; i < SIM_CLIENTS; i++) {
        if (s->clients[i].node >= 0) {
            ok = ok && s->clients[i].roster.groups.seq == seq && s->clients[i].roster.groups.count == count;
        }
    }
    return ok;
}

/* D52: groups made, changed, and removed from a handheld or the admin, across two hops. */
static void test_group_edits(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_detach(s, RANGER);   /* rejoins later, and must be handed the table then */

    /* Emma, on C, makes a group with Alex; the AP she is on authors the new version. */
    lg_group_edit_t make = { .op = LG_GROUP_CREATE, .members = 1u << ALEX };
    memcpy(make.name, "Hikers", 6);
    CHECK_EQ(lg_client_edit_group(cl(s, EMMA), &make), LG_OK);
    CHECK(sim_pump(s));
    CHECK(tables_agree(s, 2, 4));
    CHECK_EQ(s->nodes[0].roster.groups.author, 2);
    CHECK_EQ(s->nodes[0].groups_changed, 1);   /* saved once per AP, however it arrived */
    CHECK_EQ(s->nodes[2].groups_changed, 1);
    CHECK_EQ(s->clients[DAD].groups_events, 1);
    const lg_group_t *g = lg_roster_group(&s->clients[DAD].roster, 4);
    CHECK(g != NULL && strcmp(g->name, "Hikers") == 0);
    CHECK_EQ(s->clients[EMMA].group_refusals, 0);

    /* The new group carries messages at once, to members only. */
    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_GROUP, 4, 0, TXT("Trail at 9"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, ALEX, "Trail at 9"));
    CHECK_EQ(cl(s, DAD)->inbox_count, 0);

    /* A handheld outside the group may not change it: refused, nothing moves. */
    lg_group_edit_t take = { .op = LG_GROUP_UPDATE, .id = 4, .members = 1u << DAD };
    memcpy(take.name, "Mine", 4);
    CHECK_EQ(lg_client_edit_group(cl(s, DAD), &take), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].group_refusals, 1);
    CHECK_EQ(s->clients[DAD].last_refusal, LG_ACK_REJ_NOT_MEMBER);
    CHECK(tables_agree(s, 2, 4));

    /* The admin page on A renames it and adds Dad, with every backbone frame delivered twice. */
    s->duplicate_backbone = true;
    take.members = (1u << EMMA) | (1u << ALEX) | (1u << DAD);
    CHECK_EQ(lg_node_edit_groups(&s->nodes[0].node, &take), 0);
    CHECK(sim_pump(s));
    s->duplicate_backbone = false;
    CHECK(tables_agree(s, 3, 4));
    CHECK_EQ(s->nodes[1].groups_changed, 2);
    CHECK(lg_roster_is_member(&s->clients[EMMA].roster, LG_PROTO_DAD, 4));

    /* An older table forged onto the backbone is forwarded but never adopted. */
    static lg_groups_t old;
    old = s->nodes[1].roster.groups;
    old.seq = 1;
    old.count = 0;
    uint8_t body[LG_GROUPS_MAX_LEN];
    size_t blen = lg_groups_enc(&old, body);
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.major = LG_PROTO_MAJOR;
    e.minor = LG_PROTO_MINOR;
    e.type = LG_T_GROUPS;
    e.scope = LG_SCOPE_SYSTEM;
    e.ttl = LG_TTL_DEFAULT;
    /* From an AP index no simulated AP uses: a forged sequence number under a real AP's identity
     * would make that AP's own later frames look stale to the duplicate filter. */
    e.origin_id = LG_NODE_ID_BASE | 7u;
    e.origin_node = 7;
    e.origin_boot = 1;
    e.origin_seq = 7000;
    static uint8_t frame[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_node_on_backbone_frame(&s->nodes[1].node, 2, frame, (size_t)flen);
    CHECK(sim_pump(s));
    CHECK(tables_agree(s, 3, 4));

    /* A malformed edit is counted and changes nothing. */
    lg_env_t m;
    memset(&m, 0, sizeof(m));
    m.major = LG_PROTO_MAJOR;
    m.minor = LG_PROTO_MINOR;
    m.type = LG_T_GROUP_EDIT;
    m.scope = LG_SCOPE_SYSTEM;
    m.origin_id = LG_PROTO_DAD;
    m.origin_boot = 99;   /* not Dad's real boot, so his own sequence numbers stay fresh */
    m.origin_seq = 8000;
    uint8_t junk[LG_GROUP_EDIT_LEN - 1] = { LG_GROUP_CREATE };
    flen = lg_frame_build(&m, junk, sizeof(junk), frame, sizeof(frame));
    CHECK(flen > 0);
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)flen);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 1u);
    CHECK(tables_agree(s, 3, 4));

    /* Removing FAMILY deletes its messages everywhere: Alex's received copy, and Dad's own
     * unsent one, and nothing can be sent to it afterwards. */
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Dinner at 7.")) >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, ALEX, "Dinner at 7."));
    sim_detach(s, DAD);
    int kept = lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("On my way"));
    CHECK(kept >= 0);
    sim_attach(s, DAD, 0);   /* re-offers it; delivered before the removal below */
    lg_group_edit_t drop = { .op = LG_GROUP_DELETE, .id = LG_PROTO_FAMILY };
    CHECK_EQ(lg_client_edit_group(cl(s, ALEX), &drop), LG_OK);
    CHECK(sim_pump(s));
    CHECK(tables_agree(s, 4, 3));
    CHECK_EQ(s->clients[ALEX].removed_n, 1u);
    CHECK_EQ(s->clients[ALEX].removed[0], LG_PROTO_FAMILY);
    CHECK_EQ(s->clients[DAD].removed_n, 1u);
    for (size_t i = 0; i < LG_INBOX_SIZE; i++) {
        const lg_in_msg_t *in = &cl(s, ALEX)->inbox[i];
        CHECK(!(in->scope == LG_SCOPE_GROUP && in->target == LG_PROTO_FAMILY));
    }
    CHECK_EQ(cl(s, DAD)->outbox[kept].state, LG_OUT_EMPTY);
    CHECK_EQ(lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("hello?")), LG_ERR_ARG);

    /* Ranger was away for all of it and gets the current table on registering. */
    sim_attach(s, RANGER, 0);
    CHECK(tables_agree(s, 4, 3));
    CHECK_EQ(s->clients[RANGER].removed_n, 1u);   /* it held FAMILY, so it deletes it too */

    /* A new group after a removal gets a fresh id: FAMILY's 1 is never reused. */
    CHECK_EQ(lg_node_edit_groups(&s->nodes[2].node, &make), 0);
    CHECK(sim_pump(s));
    CHECK(tables_agree(s, 5, 4));
    CHECK_EQ(s->clients[RANGER].roster.groups.groups[3].id, 5);
    sim_destroy(s);
}

void test_messaging(void)
{
    test_diag_echo();
    test_grid_state();
    test_time_announce();
    test_registration_and_presence();
    test_direct_two_hops_encrypted();
    test_direct_same_node();
    test_group();
    test_group_edits();
    test_broadcast_and_rate_limit();
    test_announce_permission();
    test_duplicates_and_retransmit();
    test_offline_and_roam();
    test_time_rule();
    test_time_from_handheld();
    test_node_refuses_unsafe_frames();
    test_key_pinning();
    test_ping_pong();
    test_read_receipt();
    test_names();
}
