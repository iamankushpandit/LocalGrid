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

/*
 * An AP that missed a presence flood (the chaos run of 2026-09-18: it restarted, and the peer's one flood
 * arrived before the link was up on its side) refused 1:1 messages to a handheld it had never heard of.
 * The periodic re-announcement (D48) must heal it.
 */
static void test_presence_announced_again(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_node_t *a = &s->nodes[0].node;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (a->presence[i].in_use && a->presence[i].device == LG_PROTO_EMMA) {
            memset(&a->presence[i], 0, sizeof(a->presence[i]));   /* node A never heard of Emma */
        }
    }
    CHECK(lg_node_presence(a, LG_PROTO_EMMA) == NULL);
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Missed you"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, DAD)->outbox[slot].reject_reason, LG_ACK_REJ_OFFLINE);

    lg_node_announce_presence(&s->nodes[2].node);   /* Emma's AP, on its timer */
    CHECK(sim_pump(s));
    const lg_presence_entry_t *p = lg_node_presence(a, LG_PROTO_EMMA);
    CHECK(p != NULL && p->state == LG_PRES_ONLINE && p->node == 2);

    int again = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Found you"));
    CHECK(again >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Found you"));
    CHECK_EQ(cl(s, DAD)->outbox[again].state, LG_OUT_DELIVERED);

    /* Announcing again with nothing changed moves nothing, and an AP only announces its own handhelds:
     * node B has none of Emma's to repeat, so node A's view stays exactly as it is. */
    lg_node_announce_presence(&s->nodes[2].node);
    lg_node_announce_presence(&s->nodes[1].node);
    CHECK(sim_pump(s));
    p = lg_node_presence(a, LG_PROTO_EMMA);
    CHECK(p != NULL && p->state == LG_PRES_ONLINE && p->node == 2);
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

/* The backbone TIME_SYNC frames captured since capture_count was last reset, and their lengths. */
static size_t captured_time_syncs(const sim_t *s, size_t *bad_len)
{
    size_t n = 0;
    *bad_len = 0;
    for (size_t i = 0; i < s->capture_count; i++) {
        const sim_ev_t *ev = &s->capture[i];
        if (ev->len > LG_ENV_SIZE && ev->data[1] == LG_T_TIME_SYNC) {
            n++;
            if (ev->len != LG_ENV_SIZE + LG_TIME_SYNC_LEN_V2) {
                (*bad_len)++;
            }
        }
    }
    return n;
}

/*
 * D67: where grid time came from, and the grid's time zone. Each AP says from its own knowledge
 * whether a GPS set the time, in the flags byte only handhelds get (the backbone keeps the 8-byte
 * form so APs before D67 still take it). The zone reaches every handheld at registration and when
 * it changes, is sent once per change, and an AP without one never clears a handheld's.
 */
static void test_time_source_and_zone(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    CHECK(!lg_client_time_from_gps(cl(s, DAD)));   /* no flags from anyone yet */
    CHECK_EQ(lg_client_time_zone(cl(s, DAD))[0], '\0');

    /* Node A's GPS set the time; node C does not know that yet. */
    s->nodes[0].time_flags = LG_TIME_FROM_GPS;
    s->capture_count = 0;
    lg_node_announce_time(&s->nodes[0].node, LG_TIME_AUTHORITATIVE);
    CHECK(sim_pump(s));
    size_t bad_len = 0;
    CHECK(captured_time_syncs(s, &bad_len) > 0);
    CHECK_EQ(bad_len, 0);                          /* APs to APs: 8 bytes, no flags */
    CHECK(lg_client_time_from_gps(cl(s, DAD)));    /* from node A itself */
    CHECK(lg_client_time_from_gps(cl(s, RANGER)));
    CHECK(!lg_client_time_from_gps(cl(s, EMMA)));  /* node C pushed its own view: not by GPS */
    CHECK(!lg_client_time_restricted(cl(s, EMMA)));

    /* Node C learns it (the grid state, in firmware) and says so at its next push. */
    s->nodes[2].time_flags = LG_TIME_FROM_GPS;
    lg_node_send_time(&s->nodes[2].node, LG_PROTO_EMMA, LG_TIME_CARRIED);
    CHECK(sim_pump(s));
    CHECK(lg_client_time_from_gps(cl(s, EMMA)));

    /* Registering gets the flags at once, not only at the next announcement. */
    s->nodes[1].time_flags = 0;
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 1);
    CHECK(cl(s, EMMA)->registered);
    CHECK(!lg_client_time_from_gps(cl(s, EMMA)));
    s->nodes[1].time_flags = LG_TIME_FROM_GPS;
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 1);
    CHECK(lg_client_time_from_gps(cl(s, EMMA)));

    /* A node with no time at all: the flag goes with the time. */
    sim_detach(s, EMMA);
    s->grid_time = 0;
    s->nodes[1].time = 0;
    s->clients[EMMA].clock = 0;
    sim_attach(s, EMMA, 1);
    CHECK(lg_client_time_restricted(cl(s, EMMA)));
    CHECK(!lg_client_time_from_gps(cl(s, EMMA)));
    s->grid_time = T0;

    /* The zone: node A has it, sends it to its handhelds now, and only once. */
    const char *zone = "CST6CDT,M3.2.0,M11.1.0";
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, zone), LG_OK);
    CHECK(sim_pump(s));
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), zone) == 0);
    CHECK(strcmp(lg_client_time_zone(cl(s, RANGER)), zone) == 0);
    CHECK_EQ(s->clients[DAD].tz_events, 1);
    CHECK_EQ(lg_client_time_zone(cl(s, ALEX))[0], '\0');   /* node B has none: nothing sent */
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, zone), LG_OK);   /* the same again */
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].tz_events, 1);

    /* Refused zones leave the one held. */
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, "CST 6"), LG_ERR_ARG);
    char too_long[LG_TZ_MAX + 2];
    memset(too_long, 'A', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, too_long), LG_ERR_ARG);
    CHECK(strcmp(s->nodes[0].node.tz, zone) == 0);

    /* Registration brings it; an AP without one leaves a handheld's zone alone. */
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 0);
    CHECK(strcmp(lg_client_time_zone(cl(s, EMMA)), zone) == 0);
    CHECK_EQ(s->clients[EMMA].tz_events, 1);
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 2);
    CHECK(strcmp(lg_client_time_zone(cl(s, EMMA)), zone) == 0);
    CHECK_EQ(s->clients[EMMA].tz_events, 1);

    /* A change reaches everyone attached; forgetting sends nothing. */
    const char *india = "<+0530>-5:30";
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, india), LG_OK);
    CHECK(sim_pump(s));
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), india) == 0);
    CHECK_EQ(s->clients[DAD].tz_events, 2);
    CHECK_EQ(lg_node_set_time_zone(&s->nodes[0].node, ""), LG_OK);
    CHECK(sim_pump(s));
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), india) == 0);
    CHECK_EQ(s->clients[DAD].tz_events, 2);

    /* The client takes only a well-formed SYSTEM zone. */
    uint8_t frame[LG_ENV_SIZE + LG_TZ_MAX + 1u];
    lg_env_t e = {
        .major = LG_PROTO_MAJOR, .minor = LG_PROTO_MINOR, .type = LG_T_TIME_ZONE, .scope = LG_SCOPE_DIRECT,
        .ttl = 1, .origin_id = LG_NODE_ID_BASE | 0u, .origin_boot = 1, .origin_seq = 900, .target = LG_PROTO_DAD,
    };
    int flen = lg_frame_build(&e, (const uint8_t *)"UTC0", 4, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);   /* not SYSTEM */
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), india) == 0);
    e.scope = LG_SCOPE_SYSTEM;
    e.origin_seq++;
    flen = lg_frame_build(&e, (const uint8_t *)"UTC 0", 5, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);   /* not a POSIX TZ */
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), india) == 0);
    e.origin_seq++;
    flen = lg_frame_build(&e, (const uint8_t *)"UTC0", 4, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);
    CHECK(strcmp(lg_client_time_zone(cl(s, DAD)), "UTC0") == 0);
    CHECK_EQ(s->clients[DAD].tz_events, 3);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);   /* delivered twice: one change */
    CHECK_EQ(s->clients[DAD].tz_events, 3);

    /* A node ignores a zone sent up by a handheld. */
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    e.origin_id = LG_PROTO_DAD;
    e.target = 0;
    flen = lg_frame_build(&e, (const uint8_t *)"UTC0", 4, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)flen);
    CHECK(strcmp(s->nodes[0].node.tz, "") == 0);
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed);
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

/* ---- positions (D65) ------------------------------------------------------ */

#define LAT0  51507400    /* microdegrees */
#define LON0  (-127800)

/* Sends a POSITION frame on a client's session, as a handheld would, with any body and scope. */
static void send_position_frame(sim_t *s, int client, const lg_position_t *p, size_t body_len,
                                uint8_t scope, uint32_t seq)
{
    uint8_t body[LG_POSITION_LEN + 4];
    memset(body, 0, sizeof(body));
    (void)lg_position_enc(p, body);
    lg_env_t e = {
        .type = LG_T_POSITION, .scope = scope, .target = p->subject,
        .origin_id = s->clients[client].client.device, .origin_boot = 1, .origin_seq = seq,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, body_len, frame, sizeof(frame));
    CHECK(len > 0);
    int node = s->clients[client].node;
    lg_node_on_session_frame(&s->nodes[node].node, &s->clients[client].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
}

static bool pos_is(const lg_position_t *p, uint32_t subject, int32_t lat, int32_t lon, uint32_t fix)
{
    return p != NULL && p->subject == subject && p->lat_u == lat && p->lon_u == lon && p->fix_time == fix;
}

/* Wire format: exact length, ranges, a real subject and fix time; the table slot of each subject. */
static void test_position_body(void)
{
    lg_position_t p = {
        .subject = LG_PROTO_EMMA, .lat_u = -33868800, .lon_u = -151209300,
        .fix_time = T0, .sats = 9, .flags = LG_POS_LIVE,
    };
    uint8_t wire[LG_POSITION_LEN + 1];
    lg_position_t back;
    CHECK_EQ(lg_position_enc(&p, wire), LG_POSITION_LEN);
    CHECK(lg_position_dec(wire, LG_POSITION_LEN, &back));
    CHECK(pos_is(&back, LG_PROTO_EMMA, -33868800, -151209300, T0));
    CHECK_EQ(back.sats, 9);
    CHECK_EQ(back.flags, LG_POS_LIVE);
    CHECK_EQ(wire[4], 0x00);   /* -33868800 = 0xFDFB3400, little-endian */
    CHECK_EQ(wire[5], 0x34);
    CHECK_EQ(wire[7], 0xFD);
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN - 1, &back));
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN + 1, &back));

    lg_position_t bad = p;
    bad.lat_u = 90000001;
    (void)lg_position_enc(&bad, wire);
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN, &back));
    bad = p;
    bad.lon_u = -180000001;
    (void)lg_position_enc(&bad, wire);
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN, &back));
    bad = p;
    bad.fix_time = LG_POS_TIME_MIN - 1u;
    (void)lg_position_enc(&bad, wire);
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN, &back));
    bad = p;
    bad.subject = 0;
    (void)lg_position_enc(&bad, wire);
    CHECK(!lg_position_dec(wire, LG_POSITION_LEN, &back));
    bad = p;
    bad.lat_u = -90000000;
    bad.lon_u = 180000000;
    (void)lg_position_enc(&bad, wire);
    CHECK(lg_position_dec(wire, LG_POSITION_LEN, &back));   /* the edges are in range */

    lg_roster_t r;
    lg_roster_init_prototype(&r);
    CHECK_EQ(lg_position_slot(&r, LG_PROTO_DAD), 0);
    CHECK_EQ(lg_position_slot(&r, LG_NODE_ID_BASE | 2u), (int)LG_MAX_DEVICES + 2);
    CHECK_EQ(lg_position_slot(&r, LG_NODE_ID_BASE | LG_MAX_NODES), -1);
    CHECK_EQ(lg_position_slot(&r, 999u), -1);
}

/* D65: a handheld's position reaches every AP and every other handheld; newest fix wins. */
static void test_positions(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }

    /* Arguments are checked before anything is kept. */
    CHECK_EQ(lg_client_send_position(cl(s, DAD), 90000001, 0, T0, 5, 0), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_position(cl(s, DAD), 0, 180000001, T0, 5, 0), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0, LON0, 12345u, 5, 0), LG_ERR_ARG);
    CHECK(lg_client_position(cl(s, DAD), LG_PROTO_DAD) == NULL);

    /* Dad on A; Ranger beside him, Alex on B, Emma on C two hops away, and every AP get it. */
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0, LON0, T0, 8, LG_POS_LIVE), LG_OK);
    CHECK(sim_pump(s));
    CHECK(pos_is(lg_client_position(cl(s, DAD), LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0));
    CHECK_EQ(s->clients[DAD].position_events, 0u);   /* its own: kept, not echoed back */
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_position_t *held = lg_node_position(&s->nodes[i].node, LG_PROTO_DAD);
        CHECK(pos_is(held, LG_PROTO_DAD, LAT0, LON0, T0));
        CHECK(held != NULL && held->sats == 8 && held->flags == 0);   /* a kept copy is no clock */
    }
    for (int i = 1; i < SIM_CLIENTS; i++) {
        CHECK(pos_is(lg_client_position(cl(s, i), LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0));
        CHECK_EQ(s->clients[i].position_events, 1u);
        CHECK_EQ(s->clients[i].last_position, LG_PROTO_DAD);
    }
    lg_position_t list[LG_POS_SLOTS];
    CHECK_EQ(lg_node_positions(&s->nodes[2].node, list, LG_POS_SLOTS), 1u);
    CHECK_EQ(lg_node_positions(&s->nodes[2].node, list, 0), 0u);

    /* An older or equal fix goes nowhere and changes nothing. */
    uint32_t fwd = s->nodes[0].node.stats.forwarded;
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0 + 1000, LON0, T0 - 10u, 8, 0), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0 + 2000, LON0, T0, 8, 0), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].node.stats.forwarded, fwd);
    CHECK(pos_is(lg_node_position(&s->nodes[2].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0));
    CHECK_EQ(s->clients[EMMA].position_events, 1u);

    /* A newer fix replaces it everywhere, once, even with every backbone frame delivered twice. */
    s->duplicate_backbone = true;
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0 + 3000, LON0 - 50, T0 + 30u, 9, 0), LG_OK);
    CHECK(sim_pump(s));
    s->duplicate_backbone = false;
    CHECK(pos_is(lg_client_position(cl(s, EMMA), LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 3000, LON0 - 50, T0 + 30u));
    CHECK(pos_is(lg_node_position(&s->nodes[2].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 3000, LON0 - 50, T0 + 30u));
    CHECK_EQ(s->clients[EMMA].position_events, 2u);
    CHECK_EQ(s->clients[ALEX].position_events, 2u);

    /* Refusals: placing another handheld, a body of the wrong length, a scope other than system. */
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    lg_position_t spoof = { .subject = LG_PROTO_EMMA, .lat_u = 1, .lon_u = 1, .fix_time = T0 + 999u };
    send_position_frame(s, DAD, &spoof, LG_POSITION_LEN, LG_SCOPE_SYSTEM, 9101);
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected + 1);
    CHECK(lg_node_position(&s->nodes[0].node, LG_PROTO_EMMA) == NULL);
    CHECK(lg_client_position(cl(s, RANGER), LG_PROTO_EMMA) == NULL);
    lg_position_t mine = { .subject = LG_PROTO_DAD, .lat_u = 1, .lon_u = 1, .fix_time = T0 + 999u };
    send_position_frame(s, DAD, &mine, LG_POSITION_LEN + 1, LG_SCOPE_SYSTEM, 9102);
    send_position_frame(s, DAD, &mine, LG_POSITION_LEN, LG_SCOPE_DIRECT, 9103);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 2);
    CHECK(pos_is(lg_node_position(&s->nodes[0].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 3000, LON0 - 50, T0 + 30u));

    /* An AP's own fix reaches every handheld and AP; an older one is refused. */
    CHECK_EQ(lg_node_set_own_position(&s->nodes[0].node, 91000000, 0, T0, 7), LG_ERR_ARG);
    CHECK_EQ(lg_node_set_own_position(&s->nodes[0].node, LAT0 - 500, LON0 + 500, T0 + 40u, 7), LG_OK);
    CHECK(sim_pump(s));
    uint32_t ap_a = LG_NODE_ID_BASE | 0u;
    CHECK(pos_is(lg_node_position(&s->nodes[2].node, ap_a), ap_a, LAT0 - 500, LON0 + 500, T0 + 40u));
    for (int i = 0; i < SIM_CLIENTS; i++) {
        CHECK(pos_is(lg_client_position(cl(s, i), ap_a), ap_a, LAT0 - 500, LON0 + 500, T0 + 40u));
    }
    CHECK_EQ(s->clients[EMMA].last_position, ap_a);
    CHECK_EQ(lg_node_set_own_position(&s->nodes[0].node, LAT0, LON0, T0 + 40u, 7), LG_ERR_ID);
    CHECK_EQ(lg_node_positions(&s->nodes[1].node, list, LG_POS_SLOTS), 2u);
    CHECK_EQ(list[1].subject, ap_a);   /* handhelds first, then APs */

    /* A handheld that was away gets every position held when it registers. */
    sim_detach(s, EMMA);
    memset(cl(s, EMMA)->positions, 0, sizeof(cl(s, EMMA)->positions));   /* as if Emma had restarted */
    sim_attach(s, EMMA, 2);
    CHECK(pos_is(lg_client_position(cl(s, EMMA), LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 3000, LON0 - 50, T0 + 30u));
    CHECK(pos_is(lg_client_position(cl(s, EMMA), ap_a), ap_a, LAT0 - 500, LON0 + 500, T0 + 40u));

    /* Not registered: kept as our own, not sent. */
    sim_detach(s, ALEX);
    CHECK_EQ(lg_client_send_position(cl(s, ALEX), LAT0, LON0 + 7, T0 + 50u, 4, 0), LG_ERR_SHORT);
    CHECK(pos_is(lg_client_position(cl(s, ALEX), LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 7, T0 + 50u));
    CHECK(lg_node_position(&s->nodes[1].node, LG_PROTO_ALEX) == NULL);
    sim_attach(s, ALEX, 1);

    /* A split grid: a fix on one side reaches the other when the link comes back. */
    sim_link(s, 1, 2, false);
    CHECK_EQ(lg_client_send_position(cl(s, ALEX), LAT0, LON0 + 9, T0 + 60u, 4, 0), LG_OK);
    CHECK(sim_pump(s));
    CHECK(lg_node_position(&s->nodes[2].node, LG_PROTO_ALEX) == NULL);
    sim_link(s, 1, 2, true);
    CHECK(pos_is(lg_node_position(&s->nodes[2].node, LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 9, T0 + 60u));
    CHECK(pos_is(lg_client_position(cl(s, EMMA), LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 9, T0 + 60u));
    sim_destroy(s);
}

/* D65: a handheld reading GPS now gives time to an AP that has none, and never overrides one set. */
static void test_position_time(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    s->grid_time = 0;
    for (int i = 0; i < SIM_NODES; i++) {
        s->nodes[i].time = 0;
    }
    /* Not live: a stored fix is no clock. */
    CHECK_EQ(lg_client_send_position(cl(s, ALEX), LAT0, LON0, T0 + 5u, 6, 0), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].client_time_count, 0u);
    CHECK_EQ(s->nodes[1].time, 0u);

    /* Live, on an AP with no clock: taken as carried time. */
    CHECK_EQ(lg_client_send_position(cl(s, EMMA), LAT0, LON0, T0 + 7u, 6, LG_POS_LIVE), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[2].client_time_count, 1u);
    CHECK_EQ(s->nodes[2].time, T0 + 7u);
    CHECK_EQ(s->nodes[2].time_stratum, LG_STRATUM_UNKNOWN);
    CHECK_EQ(s->nodes[1].client_time_count, 0u);   /* only the AP it was sent to; copies carry no flag */

    /* Live, on an AP whose clock is set: never overridden. */
    s->nodes[0].time = T0;
    CHECK_EQ(lg_client_send_position(cl(s, DAD), LAT0, LON0, T0 + 100u, 6, LG_POS_LIVE), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].client_time_count, 0u);
    CHECK_EQ(s->nodes[0].time, T0);
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
    lg_client_ping(cl(s, EMMA), LG_BATTERY_UNKNOWN);
    sim_pump(s);
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 5000u);
    CHECK_EQ(cl(s, DAD)->last_pong_ms, 0u);
    CHECK_EQ(cl(s, ALEX)->last_pong_ms, 0u);

    /* A handheld that is not connected sends nothing and records nothing. */
    sim_detach(s, EMMA);
    lg_client_disconnected(cl(s, EMMA));
    s->now_ms = 9000;
    lg_client_ping(cl(s, EMMA), 50);
    sim_pump(s);
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 5000u);
    sim_destroy(s);
}

/* Sends a PING on Dad's session at node A with any body, as a handheld of any age would. */
static void dad_sends_ping(sim_t *s, const uint8_t *body, size_t body_len, uint32_t seq)
{
    lg_env_t e = {
        .type = LG_T_PING, .scope = LG_SCOPE_SYSTEM, .target = 0,
        .origin_id = LG_PROTO_DAD, .origin_boot = 1, .origin_seq = seq,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, body_len, frame, sizeof(frame));
    CHECK(len > 0);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
}

/* D68: a handheld's battery rides on its PING to its own AP; a PING without it still keeps the
 * session alive and reports nothing. */
/*
 * Asking a roster that was never filled in. A handheld that cannot start says so, and saying so
 * asks for a name: on the bench (2026-09-20) that read through a null table and left the board
 * rebooting, so the explanation never reached anyone.
 */
static void test_roster_before_setup(void)
{
    /* Static: a client and a roster are kilobytes, and this suite runs on the main task's stack. */
    static lg_roster_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK_EQ(lg_roster_user_index(&empty, 1), -1);
    CHECK(lg_roster_user(&empty, 1) == NULL);
    CHECK_EQ(lg_roster_user_index(NULL, 1), -1);

    static lg_client_t c;
    memset(&c, 0, sizeof(c));
    CHECK(lg_client_name(&c, 1) == NULL);       /* no roster yet */
    CHECK(lg_client_name(NULL, 1) == NULL);

    /* And the roster the grid actually ships carries every handheld the bench has. */
    static lg_roster_t proto;
    lg_roster_init_prototype(&proto);
    for (uint32_t d = 1; d <= 12; d++) {
        CHECK(lg_roster_user(&proto, d) != NULL);
    }
    CHECK(lg_roster_user(&proto, 13) == NULL);
}

static void test_ping_battery(void)
{
    uint8_t b = 0;
    uint8_t body[2] = { 57, 0 };
    CHECK_EQ(lg_ping_enc(57, body), LG_PING_LEN);
    CHECK(lg_ping_dec(body, 0, &b) && b == LG_BATTERY_UNKNOWN);   /* before D68 */
    CHECK(lg_ping_dec(body, 1, &b) && b == 57);
    body[0] = 100;
    CHECK(lg_ping_dec(body, 1, &b) && b == 100);
    body[0] = LG_BATTERY_UNKNOWN;
    CHECK(lg_ping_dec(body, 1, &b) && b == LG_BATTERY_UNKNOWN);
    body[0] = 101;
    CHECK(!lg_ping_dec(body, 1, &b));
    body[0] = 50;
    CHECK(!lg_ping_dec(body, 2, &b));

    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK_EQ(lg_node_battery(&s->nodes[i].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    }
    /* Emma, on C, reports to C only. */
    s->now_ms = 5000;
    lg_client_ping(cl(s, EMMA), 57);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->last_pong_ms, 5000u);
    CHECK_EQ(lg_node_battery(&s->nodes[2].node, LG_PROTO_EMMA), 57);
    CHECK_EQ(lg_node_battery(&s->nodes[0].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    CHECK_EQ(lg_node_battery(&s->nodes[1].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    /* Out of range from the client API goes as unknown. */
    lg_client_ping(cl(s, EMMA), 200);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_node_battery(&s->nodes[2].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    lg_client_ping(cl(s, EMMA), 0);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_node_battery(&s->nodes[2].node, LG_PROTO_EMMA), 0);

    /* A handheld from before D68: empty PING, answered, battery unknown. */
    uint8_t one = 80;
    dad_sends_ping(s, &one, 1, 900);
    CHECK_EQ(lg_node_battery(&s->nodes[0].node, LG_PROTO_DAD), 80);
    s->now_ms = 6000;
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    dad_sends_ping(s, NULL, 0, 901);
    CHECK_EQ(cl(s, DAD)->last_pong_ms, 6000u);
    CHECK_EQ(lg_node_battery(&s->nodes[0].node, LG_PROTO_DAD), LG_BATTERY_UNKNOWN);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed);

    /* A bad value is counted, forgotten, and still answered: the keepalive never fails on it. */
    dad_sends_ping(s, &one, 1, 902);
    s->now_ms = 7000;
    uint8_t bad[2] = { 150, 0 };
    dad_sends_ping(s, bad, 1, 903);
    CHECK_EQ(cl(s, DAD)->last_pong_ms, 7000u);
    CHECK_EQ(lg_node_battery(&s->nodes[0].node, LG_PROTO_DAD), LG_BATTERY_UNKNOWN);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 1u);
    s->now_ms = 8000;
    dad_sends_ping(s, bad, 2, 904);
    CHECK_EQ(cl(s, DAD)->last_pong_ms, 8000u);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 2u);

    /* Moving to another AP starts afresh there: nothing is reported until its first PING. */
    lg_client_ping(cl(s, EMMA), 33);
    CHECK(sim_pump(s));
    sim_detach(s, EMMA);
    lg_client_disconnected(cl(s, EMMA));
    sim_attach(s, EMMA, 1);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_node_battery(&s->nodes[1].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    lg_client_ping(cl(s, EMMA), 34);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_node_battery(&s->nodes[1].node, LG_PROTO_EMMA), 34);
    /* ... and registering again on the same AP forgets the old reading too. */
    sim_detach(s, EMMA);
    lg_client_disconnected(cl(s, EMMA));
    sim_attach(s, EMMA, 1);
    CHECK(sim_pump(s));
    CHECK_EQ(lg_node_battery(&s->nodes[1].node, LG_PROTO_EMMA), LG_BATTERY_UNKNOWN);
    CHECK_EQ(lg_node_battery(&s->nodes[1].node, 99), LG_BATTERY_UNKNOWN);   /* not in the roster */
    sim_destroy(s);
}

/* D68: every AP keeps the newest urgent broadcast it carried, whether it stands an alert down,
 * and how many read reports for it passed through. */
static void test_urgent_record(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_urgent_info_t u;
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(!lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.author, 0u);
        CHECK(!u.active);
    }
    /* An ordinary announcement is not an alert. */
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, 0, TXT("Dinner at six")) >= 0);
    CHECK(sim_pump(s));
    CHECK(!lg_node_urgent(&s->nodes[1].node, &u));

    /* Dad on A raises an alert: every AP carried it, from its handheld or the backbone. */
    s->now_ms += LG_BROADCAST_INTERVAL_MS + 1;
    int sos = lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS from Dad"));
    CHECK(sos >= 0);
    CHECK(sim_pump(s));
    s->now_ms += 3000;
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.author, LG_PROTO_DAD);
        CHECK(u.active);
        CHECK(!u.all_clear);
        CHECK_EQ(u.reads, 0);
        CHECK_EQ(u.age_ms, 3000u);
    }

    /* Emma on C reads it: her report travels C -> B -> A, and each AP on the way counts it once. */
    const lg_in_msg_t *in = lg_client_inbox(cl(s, EMMA), 0);
    CHECK(in != NULL && in->author == LG_PROTO_DAD);
    uint32_t boot = in->boot, seq = in->seq;
    CHECK(lg_client_mark_read(cl(s, EMMA), LG_PROTO_DAD, boot, seq));
    CHECK(sim_pump(s));
    CHECK(lg_client_mark_read(cl(s, EMMA), LG_PROTO_DAD, boot, seq));   /* reported twice */
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.reads, 1);
    }
    /* Alex on B reads it too: B and A see his report, C does not. */
    CHECK(lg_client_mark_read(cl(s, ALEX), LG_PROTO_DAD, boot, seq));
    CHECK(sim_pump(s));
    CHECK(lg_node_urgent(&s->nodes[0].node, &u) && u.reads == 2);
    CHECK(lg_node_urgent(&s->nodes[1].node, &u) && u.reads == 2);
    CHECK(lg_node_urgent(&s->nodes[2].node, &u) && u.reads == 1);
    /* A read report for another message is not counted. */
    CHECK(lg_client_mark_read(cl(s, RANGER), LG_PROTO_DAD, boot, seq + 100u));
    CHECK(sim_pump(s));
    CHECK(lg_node_urgent(&s->nodes[0].node, &u) && u.reads == 2);

    /* Ranger raises another: the newest replaces it, its reads start at zero. */
    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("Bear at the lake")) >= 0);
    CHECK(sim_pump(s));
    CHECK(lg_node_urgent(&s->nodes[2].node, &u));
    CHECK_EQ(u.author, LG_PROTO_RANGER);
    CHECK_EQ(u.reads, 0);
    CHECK(u.active);

    /* Dad stands his down. The newest is an all clear, but Ranger's alert is still active. */
    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR,
                              TXT("Dad is safe")) >= 0);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.author, LG_PROTO_DAD);
        CHECK(u.all_clear);
        CHECK(u.active);
    }
    /* Ranger's all clear leaves nothing active. */
    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, RANGER), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR,
                              TXT("Bear gone")) >= 0);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.author, LG_PROTO_RANGER);
        CHECK(u.all_clear);
        CHECK(!u.active);
    }

    /* An alert that nobody stands down stops being active after LG_URGENT_ACTIVE_MS. */
    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("Lost on the trail")) >= 0);
    CHECK(sim_pump(s));
    CHECK(lg_node_urgent(&s->nodes[0].node, &u) && u.active && u.author == LG_PROTO_EMMA);
    s->now_ms += LG_URGENT_ACTIVE_MS - 1u;
    CHECK(lg_node_urgent(&s->nodes[0].node, &u) && u.active);
    s->now_ms += 1u;
    CHECK(lg_node_urgent(&s->nodes[0].node, &u) && !u.active);
    CHECK_EQ(u.age_ms, LG_URGENT_ACTIVE_MS);

    /* Duplicated backbone frames neither renew the alert nor count a read twice. */
    s->duplicate_backbone = true;
    s->now_ms += LG_URGENT_INTERVAL_MS + 1;
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS again")) >= 0);
    CHECK(sim_pump(s));
    in = lg_client_inbox(cl(s, EMMA), 0);
    CHECK(in != NULL && in->author == LG_PROTO_DAD);
    CHECK(lg_client_mark_read(cl(s, EMMA), LG_PROTO_DAD, in->boot, in->seq));
    CHECK(sim_pump(s));
    s->now_ms += 1000;
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(lg_node_urgent(&s->nodes[i].node, &u));
        CHECK_EQ(u.author, LG_PROTO_DAD);
        CHECK_EQ(u.reads, 1);
        CHECK_EQ(u.age_ms, 1000u);
    }
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

/* ---- live voice (D61) ---------------------------------------------------- */

/* A voice payload: header for (talk, frame) and data_len bytes of data, all `fill` or from text. */
static size_t voice_payload(uint8_t *out, uint16_t talk, uint16_t frame, uint8_t flags,
                            const char *text, size_t data_len, uint8_t fill)
{
    lg_voice_hdr_t h = {
        .codec = LG_VOICE_CODEC_IMA_8K, .flags = flags, .talk = talk, .frame = frame,
        .predictor = -1234, .step_index = 40,
    };
    size_t n = lg_voice_hdr_enc(&h, out);
    if (text != NULL) {
        memcpy(out + n, text, data_len);
    } else {
        memset(out + n, fill, data_len);
    }
    return n + data_len;
}

/* Builds a VOICE frame by hand, as a handheld or a neighbour would, with any contents. */
static int voice_frame(uint8_t *frame, uint32_t author, uint32_t boot, uint32_t seq, uint8_t scope,
                       uint32_t target, uint16_t flags, const uint8_t *body, size_t blen)
{
    lg_env_t e = {
        .type = LG_T_VOICE, .scope = scope, .flags = flags, .target = target, .ttl = LG_TTL_DEFAULT,
        .origin_id = author, .origin_boot = boot, .origin_seq = seq, .grid_time = T0,
    };
    return lg_frame_build(&e, body, blen, frame, LG_FRAME_MAX);
}

static bool outbox_empty(const lg_client_t *c)
{
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        if (c->outbox[i].state != LG_OUT_EMPTY) {
            return false;
        }
    }
    return true;
}

static void test_voice_body(void)
{
    uint8_t b[LG_VOICE_PAYLOAD_MAX + 2];
    memset(b, 0, sizeof(b));
    lg_voice_hdr_t h = {
        .codec = LG_VOICE_CODEC_IMA_8K, .flags = LG_VOICE_END, .talk = 0xBEEF, .frame = 3,
        .predictor = -1234, .step_index = 88,
    };
    CHECK_EQ(lg_voice_hdr_enc(&h, b), LG_VOICE_HDR_LEN);
    lg_voice_hdr_t d;
    CHECK(lg_voice_hdr_dec(b, LG_VOICE_HDR_LEN, &d));           /* an END frame may carry no data */
    CHECK_EQ(d.codec, LG_VOICE_CODEC_IMA_8K);
    CHECK_EQ(d.flags, LG_VOICE_END);
    CHECK_EQ(d.talk, 0xBEEF);
    CHECK_EQ(d.frame, 3);
    CHECK_EQ(d.predictor, -1234);
    CHECK_EQ(d.step_index, 88);
    CHECK(lg_voice_hdr_dec(b, LG_VOICE_PAYLOAD_MAX, NULL));     /* 100 ms of ADPCM fits */
    CHECK(!lg_voice_hdr_dec(b, LG_VOICE_HDR_LEN - 1u, NULL));
    CHECK(!lg_voice_hdr_dec(b, LG_VOICE_PAYLOAD_MAX + 1u, NULL));
    b[0] = 2;
    CHECK(!lg_voice_hdr_dec(b, LG_VOICE_HDR_LEN, NULL));        /* unknown codec */
    b[0] = LG_VOICE_CODEC_IMA_8K;
    b[8] = 89;
    CHECK(!lg_voice_hdr_dec(b, LG_VOICE_HDR_LEN, NULL));        /* step index past the table */
    b[8] = 0;
    b[9] = 1;
    CHECK(!lg_voice_hdr_dec(b, LG_VOICE_HDR_LEN, NULL));        /* reserved byte */
    CHECK_EQ(LG_VOICE_SAMPLES / 2u, LG_VOICE_DATA_MAX);
    /* The largest sealed frame fits one ESP-NOW v2 frame. */
    CHECK(LG_ENV_SIZE + LG_VOICE_PAYLOAD_MAX + LG_AEAD_TAG_LEN <= LG_FRAME_MAX);
}

static void test_voice_group(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    uint32_t text_seq = cl(s, DAD)->seq;
    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    size_t n = voice_payload(p, 1, 0, 0, NULL, 40, 0xA5);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 1);              /* two hops away */
    CHECK_EQ(s->clients[ALEX].voice_frames, 1);
    CHECK_EQ(s->clients[RANGER].voice_frames, 0);            /* not a member */
    CHECK_EQ(s->clients[DAD].voice_frames, 0);               /* never the author's own */
    CHECK_EQ(s->clients[EMMA].voice_author, LG_PROTO_DAD);
    CHECK_EQ(s->clients[EMMA].voice_scope, LG_SCOPE_GROUP);
    CHECK_EQ(s->clients[EMMA].voice_len, n);
    CHECK(memcmp(s->clients[EMMA].voice_last, p, n) == 0);
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK_EQ(s->nodes[i].node.stats.voice, 1);
    }
    /* Live only: nothing stored, nothing queued, no ack, and text sequences untouched. */
    for (int i = 0; i < SIM_CLIENTS; i++) {
        CHECK_EQ(cl(s, i)->inbox_count, 0);
    }
    CHECK(outbox_empty(cl(s, DAD)));
    CHECK_EQ(s->clients[DAD].voice_refusals, 0);
    CHECK_EQ(cl(s, DAD)->seq, text_seq);

    /* The release frame carries no data and still arrives. */
    n = voice_payload(p, 1, 1, LG_VOICE_END, NULL, 0, 0);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    CHECK_EQ(s->clients[ALEX].voice_frames, 2);
    CHECK_EQ(s->clients[EMMA].voice_len, LG_VOICE_HDR_LEN);
    CHECK_EQ(s->clients[EMMA].voice_last[1], LG_VOICE_END);

    /* KIDS: Emma to Alex, and Dad hears nothing. */
    n = voice_payload(p, 9, 0, 0, NULL, 20, 0x11);
    CHECK_EQ(lg_client_send_voice(cl(s, EMMA), LG_SCOPE_GROUP, LG_PROTO_KIDS, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[ALEX].voice_frames, 3);
    CHECK_EQ(s->clients[ALEX].voice_author, LG_PROTO_EMMA);
    CHECK_EQ(s->clients[DAD].voice_frames, 0);
    CHECK_EQ(s->clients[RANGER].voice_frames, 0);
    sim_destroy(s);
}

static void test_voice_direct(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    static const char secret[] = "VOICE-SECRET-VOICE-SECRET-VOICE-SECRET";
    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    size_t n = voice_payload(p, 2, 0, 0, secret, sizeof(secret) - 1u, 0);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 1);
    CHECK_EQ(s->clients[EMMA].voice_scope, LG_SCOPE_DIRECT);
    CHECK_EQ(s->clients[EMMA].voice_len, n);
    CHECK(memcmp(s->clients[EMMA].voice_last, p, n) == 0);   /* decrypted at the target */
    CHECK_EQ(s->clients[ALEX].voice_frames, 0);
    CHECK_EQ(s->clients[RANGER].voice_frames, 0);
    CHECK(!sim_captured_contains(s, "VOICE-SECRET"));        /* sealed on the backbone */

    /* Same AP. */
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_RANGER, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[RANGER].voice_frames, 1);

    /* Arguments the handheld refuses before anything leaves. */
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_BROADCAST, LG_TARGET_ALL, p, n), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, 99, p, n), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_DAD, p, n), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, 77, p, n), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, LG_VOICE_HDR_LEN - 1u), LG_ERR_ARG);
    lg_peer_t *emma = NULL;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (cl(s, DAD)->peers[i].in_use && cl(s, DAD)->peers[i].device == LG_PROTO_EMMA) {
            emma = &cl(s, DAD)->peers[i];
        }
    }
    CHECK(emma != NULL);
    if (emma != NULL) {
        emma->has_key = 0;
        CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_ERR_ARG);
        emma->has_key = 1;
    }

    /* A frame that does not open is dropped and counted, never played. */
    lg_peer_t *dad = NULL;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (cl(s, EMMA)->peers[i].in_use && cl(s, EMMA)->peers[i].device == LG_PROTO_DAD) {
            dad = &cl(s, EMMA)->peers[i];
        }
    }
    CHECK(dad != NULL);
    if (dad != NULL) {
        uint32_t fails = cl(s, EMMA)->decrypt_failures;
        dad->pubkey[0] ^= 1u;
        CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
        CHECK(sim_pump(s));
        CHECK_EQ(cl(s, EMMA)->decrypt_failures, fails + 1u);
        CHECK_EQ(s->clients[EMMA].voice_frames, 1);
        dad->pubkey[0] ^= 1u;
        /* The failed frame did not advance the newest-seen mark: the next one plays. */
        CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
        CHECK(sim_pump(s));
        CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    }
    sim_destroy(s);
}

static void test_voice_duplicates_and_order(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_link(s, 0, 2, true);           /* full mesh: every flood reaches nodes twice */
    s->duplicate_backbone = true;      /* and every backbone frame is delivered twice */

    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    size_t n = voice_payload(p, 3, 0, 0, NULL, 40, 0x33);
    uint32_t dups = s->nodes[2].node.stats.duplicates;
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 1);
    CHECK_EQ(s->clients[ALEX].voice_frames, 1);
    CHECK(s->nodes[2].node.stats.duplicates > dups);
    CHECK_EQ(s->nodes[2].node.stats.voice, 1);

    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);

    /* Dad's session offers his first voice frame again: older than the newest, so dropped. */
    uint8_t frame[LG_FRAME_MAX];
    int len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 1u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    CHECK(len > 0);
    uint32_t voice0 = s->nodes[0].node.stats.voice;
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].node.stats.voice, voice0);
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    CHECK_EQ(s->clients[DAD].voice_refusals, 0);   /* a stale frame is not refused, just dropped */

    /* The handheld keeps its own newest-only mark per author, and a new boot starts it again. */
    lg_client_t *emma = cl(s, EMMA);
    len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 1u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 100u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 3);
    lg_client_on_frame(emma, frame, (size_t)len);                /* the same frame twice */
    CHECK_EQ(s->clients[EMMA].voice_frames, 3);
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 1u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 4);
    len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 200u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);                /* from the boot before */
    CHECK_EQ(s->clients[EMMA].voice_frames, 4);
    /* A voice frame without the voice bit, or a bad header, is not voice. */
    len = voice_frame(frame, LG_PROTO_DAD, 2, 500u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 4);
    p[9] = 1;
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 5u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_client_on_frame(emma, frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 4);

    /* A neighbour's malformed voice frame is counted, never delivered. */
    uint32_t bad = s->nodes[1].node.stats.malformed;
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 9u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_node_on_backbone_frame(&s->nodes[1].node, 0, frame, (size_t)len);
    p[9] = 0;
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 9u, LG_SCOPE_DIRECT, LG_PROTO_ALEX, 0, p, n);
    lg_node_on_backbone_frame(&s->nodes[1].node, 0, frame, (size_t)len);   /* 1:1 must be sealed */
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 9u, LG_SCOPE_BROADCAST, LG_TARGET_ALL, 0, p, n);
    lg_node_on_backbone_frame(&s->nodes[1].node, 0, frame, (size_t)len);   /* never broadcast */
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].node.stats.malformed, bad + 3u);
    CHECK_EQ(s->clients[ALEX].voice_frames, 1);
    sim_destroy(s);
}

static void test_voice_refusals(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    size_t n = voice_payload(p, 4, 0, 0, NULL, 40, 0x44);

    /* Ranger is not in FAMILY: one refusal, then quiet for a second however many frames follow. */
    CHECK_EQ(lg_client_send_voice(cl(s, RANGER), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[RANGER].voice_refusals, 1);
    CHECK_EQ(s->clients[RANGER].last_voice_refusal, LG_ACK_REJ_NOT_MEMBER);
    for (int i = 0; i < 5; i++) {
        s->now_ms += 100;
        CHECK_EQ(lg_client_send_voice(cl(s, RANGER), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
        CHECK(sim_pump(s));
    }
    CHECK_EQ(s->clients[RANGER].voice_refusals, 1);
    s->now_ms += LG_VOICE_REFUSE_MS;
    CHECK_EQ(lg_client_send_voice(cl(s, RANGER), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[RANGER].voice_refusals, 2);
    CHECK_EQ(s->clients[EMMA].voice_frames, 0);
    CHECK_EQ(s->clients[DAD].voice_frames, 0);
    CHECK(outbox_empty(cl(s, RANGER)));

    /* 1:1 to a handheld that is offline. */
    sim_detach(s, EMMA);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].voice_refusals, 1);
    CHECK_EQ(s->clients[DAD].last_voice_refusal, LG_ACK_REJ_OFFLINE);

    /* A clock outside the tolerance is refused by the AP, as for text. */
    s->now_ms += LG_VOICE_REFUSE_MS;
    s->clients[DAD].clock = T0 - (LG_TIME_TOLERANCE_S + 60);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].voice_refusals, 2);
    CHECK_EQ(s->clients[DAD].last_voice_refusal, LG_ACK_REJ_TIME);
    CHECK_EQ(s->clients[ALEX].voice_frames, 0);

    /* No grid time: the handheld refuses before sending. Not registered: nothing to send on. */
    s->clients[DAD].clock = 0;
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_ERR_TIME);
    s->clients[DAD].clock = T0;
    sim_detach(s, RANGER);
    CHECK_EQ(lg_client_send_voice(cl(s, RANGER), LG_SCOPE_GROUP, LG_PROTO_LEADERS, p, n), LG_ERR_SHORT);

    /* A voice frame without the voice sequence bit, or sent to everyone, is invalid. The first
     * one's refusal names a text sequence, so the handheld cannot take it for a voice refusal. */
    uint8_t frame[LG_FRAME_MAX];
    uint32_t rejected = s->nodes[1].node.stats.rejected;
    uint32_t voice1 = s->nodes[1].node.stats.voice;
    int len = voice_frame(frame, LG_PROTO_ALEX, 1, 7000u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_node_on_session_frame(&s->nodes[1].node, &s->clients[ALEX].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].node.stats.rejected, rejected + 1u);
    CHECK_EQ(s->nodes[1].node.stats.voice, voice1);
    CHECK_EQ(s->clients[ALEX].voice_refusals, 0);
    CHECK(outbox_empty(cl(s, ALEX)));
    s->now_ms += LG_VOICE_REFUSE_MS;
    len = voice_frame(frame, LG_PROTO_ALEX, 1, LG_VOICE_SEQ_BIT | 1u, LG_SCOPE_BROADCAST, LG_TARGET_ALL, 0, p, n);
    lg_node_on_session_frame(&s->nodes[1].node, &s->clients[ALEX].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[ALEX].voice_refusals, 1);
    CHECK_EQ(s->clients[ALEX].last_voice_refusal, LG_ACK_REJ_INVALID);
    /* A sealed 1:1 body longer than the largest frame is invalid (checked by length only). */
    s->now_ms += LG_VOICE_REFUSE_MS;
    static uint8_t big[LG_VOICE_PAYLOAD_MAX + LG_AEAD_TAG_LEN + 1u];
    len = voice_frame(frame, LG_PROTO_ALEX, 1, LG_VOICE_SEQ_BIT | 2u, LG_SCOPE_DIRECT, LG_PROTO_DAD,
                      LG_FLAG_E2E_PAYLOAD, big, sizeof(big));
    lg_node_on_session_frame(&s->nodes[1].node, &s->clients[ALEX].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[ALEX].voice_refusals, 2);
    CHECK_EQ(s->clients[ALEX].last_voice_refusal, LG_ACK_REJ_INVALID);
    CHECK_EQ(s->clients[DAD].voice_frames, 0);
    sim_destroy(s);
}

/* Voice sequences carry their own bit, so voice never pushes the text dedup window forward. */
static void test_voice_keeps_text_fresh(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    int d = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Before the talk"));
    CHECK(d >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);

    uint32_t text_seq = cl(s, DAD)->seq;
    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    size_t n = voice_payload(p, 5, 0, 0, NULL, 40, 0x55);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
        CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
        CHECK(sim_pump(s));
    }
    CHECK_EQ(s->clients[EMMA].voice_frames, 10);
    CHECK_EQ(cl(s, DAD)->seq, text_seq);

    /* The text's acknowledgement was lost: its retransmission is a duplicate, not stale. */
    uint32_t dups = s->nodes[0].node.stats.duplicates;
    cl(s, DAD)->outbox[d].state = LG_OUT_PENDING;
    s->now_ms += LG_RESEND_MS + 1;
    lg_client_tick(cl(s, DAD));
    CHECK(sim_pump(s));
    CHECK(s->nodes[0].node.stats.duplicates > dups);
    CHECK_EQ(cl(s, DAD)->outbox[d].state, LG_OUT_ACCEPTED);
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);

    /* New text after the talk is delivered on every AP and handheld. */
    int g = lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("After the talk"));
    CHECK(g >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "After the talk"));
    CHECK(newest_is(s, ALEX, "After the talk"));
    CHECK_EQ(cl(s, DAD)->outbox[g].delivered_count, 2);
    sim_destroy(s);
}

/*
 * D70: the traffic counters classify what passes and never hold any of it. A 1:1 text counts as
 * one 1:1 in on the AP the sender is on, one relayed on the AP in the middle, and one out on the
 * AP that delivers it; a group text counts as group; and no class ever counts a body.
 */
static void test_traffic_counters(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_node_traffic_t before[SIM_NODES];
    for (int i = 0; i < SIM_NODES; i++) {
        before[i] = *lg_node_traffic(&s->nodes[i].node);
        CHECK(before[i].in[LG_TC_PRESENCE] > 0);   /* registration already counted */
    }
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Where are you?")) >= 0);
    CHECK(sim_pump(s));

    const lg_node_traffic_t *a = lg_node_traffic(&s->nodes[0].node);
    const lg_node_traffic_t *b = lg_node_traffic(&s->nodes[1].node);
    const lg_node_traffic_t *c = lg_node_traffic(&s->nodes[2].node);
    CHECK_EQ(a->in[LG_TC_DIRECT] - before[0].in[LG_TC_DIRECT], 1u);       /* taken from Dad */
    CHECK(b->relayed[LG_TC_DIRECT] > before[1].relayed[LG_TC_DIRECT]);     /* passed along the chain */
    CHECK(c->out[LG_TC_DIRECT] > before[2].out[LG_TC_DIRECT]);             /* delivered to Emma */
    CHECK_EQ(a->in[LG_TC_GROUP], before[0].in[LG_TC_GROUP]);               /* not a group message */
    CHECK(a->bytes_in > before[0].bytes_in);
    CHECK_EQ(a->in[LG_TC_VOICE], 0u);

    /* The ack for it comes back as an ack, not as text. */
    CHECK(c->in[LG_TC_ACK] + a->out[LG_TC_ACK] > 0u);

    lg_node_traffic_t mid = *a;
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Dinner at seven.")) >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(a->in[LG_TC_GROUP] - mid.in[LG_TC_GROUP], 1u);
    CHECK_EQ(a->in[LG_TC_DIRECT], mid.in[LG_TC_DIRECT]);
    sim_destroy(s);
}

void test_messaging(void)
{
    test_diag_echo();
    test_grid_state();
    test_time_announce();
    test_time_source_and_zone();
    test_registration_and_presence();
    test_direct_two_hops_encrypted();
    test_direct_same_node();
    test_group();
    test_group_edits();
    test_broadcast_and_rate_limit();
    test_announce_permission();
    test_duplicates_and_retransmit();
    test_offline_and_roam();
    test_presence_announced_again();
    test_time_rule();
    test_time_from_handheld();
    test_node_refuses_unsafe_frames();
    test_key_pinning();
    test_ping_pong();
    test_traffic_counters();
    test_roster_before_setup();
    test_ping_battery();
    test_urgent_record();
    test_read_receipt();
    test_names();
    test_position_body();
    test_positions();
    test_position_time();
    test_voice_body();
    test_voice_group();
    test_voice_direct();
    test_voice_duplicates_and_order();
    test_voice_refusals();
    test_voice_keeps_text_fresh();
}
