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

/* ---- trails (D99) ---- */

/* Sends a frame of a SYSTEM-family type on a client's session, as a handheld would, with any body. */
static void send_session_frame(sim_t *s, int client, uint8_t type, const uint8_t *body, size_t body_len,
                               uint8_t scope, uint32_t target, uint32_t seq)
{
    lg_env_t e = {
        .type = type, .scope = scope, .target = target,
        .origin_id = s->clients[client].client.device, .origin_boot = 1, .origin_seq = seq,
    };
    uint8_t frame[LG_FRAME_MAX];
    int len = lg_frame_build(&e, body, body_len, frame, sizeof(frame));
    CHECK(len > 0);
    int node = s->clients[client].node;
    lg_node_on_session_frame(&s->nodes[node].node, &s->clients[client].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
}

/* A trail as another AP would flood it, handed to `node` as if from its neighbour `from`. */
static void backbone_trail(sim_t *s, int node, uint16_t from, const lg_trail_t *t, uint32_t seq)
{
    uint8_t body[LG_TRAIL_LEN_MAX];
    size_t blen = lg_trail_enc(t, body);
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.major = LG_PROTO_MAJOR;
    e.minor = LG_PROTO_MINOR;
    e.type = LG_T_TRAIL;
    e.scope = LG_SCOPE_SYSTEM;
    e.target = t->subject;
    e.ttl = LG_TTL_DEFAULT;
    e.origin_id = LG_NODE_ID_BASE | from;
    e.origin_node = from;
    e.origin_boot = 1;
    e.origin_seq = seq;
    uint8_t frame[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_node_on_backbone_frame(&s->nodes[node].node, from, frame, (size_t)flen);
    CHECK(sim_pump(s));
}

static bool trail_is(const lg_trail_t *t, uint32_t subject, int32_t lat, int32_t lon, uint32_t fix, uint8_t count)
{
    return t != NULL && t->subject == subject && t->lat_u == lat && t->lon_u == lon && t->fix_time == fix &&
           t->count == count;
}

static void test_trail_body(void)
{
    lg_trail_t t = {
        .subject = LG_PROTO_EMMA, .lat_u = -33868800, .lon_u = -151209300, .fix_time = T0, .count = 3,
        .older = { { -9, 12, 7 }, { -20, 30, 15 }, { 32767, -32768, 65535 } },
    };
    uint8_t wire[LG_TRAIL_LEN_MAX + 8];
    lg_trail_t back;
    CHECK_EQ(LG_TRAIL_LEN_MAX, 107u);
    CHECK_EQ(lg_trail_enc(&t, wire), 17u + 18u);
    CHECK(lg_trail_dec(wire, 35, &back));
    CHECK(trail_is(&back, LG_PROTO_EMMA, -33868800, -151209300, T0, 3));
    CHECK(back.older[0].dlat == -9 && back.older[0].dlon == 12 && back.older[0].age_s == 7);
    CHECK(back.older[2].dlat == 32767 && back.older[2].dlon == -32768 && back.older[2].age_s == 65535);
    CHECK_EQ(wire[16], 3);
    CHECK_EQ(wire[17], 0xF7);   /* -9, little-endian */
    CHECK_EQ(wire[18], 0xFF);
    CHECK_EQ(wire[21], 7);
    CHECK(!lg_trail_dec(wire, 34, &back));
    CHECK(!lg_trail_dec(wire, 36, &back));
    CHECK(!lg_trail_dec(wire, 16, &back));

    /* Just the newest point is a trail too; so is a full one, and one more than full is not. */
    lg_trail_t one = t;
    one.count = 0;
    CHECK_EQ(lg_trail_enc(&one, wire), 17u);
    CHECK(lg_trail_dec(wire, 17, &back));
    CHECK_EQ(back.count, 0);
    lg_trail_t full = t;
    full.count = LG_TRAIL_POINTS - 1u;
    for (unsigned i = 0; i < LG_TRAIL_POINTS - 1u; i++) {
        full.older[i] = (lg_trail_point_t){ (int16_t)i, (int16_t)-i, (uint16_t)(i * 9u) };
    }
    CHECK_EQ(lg_trail_enc(&full, wire), LG_TRAIL_LEN_MAX);
    CHECK(lg_trail_dec(wire, LG_TRAIL_LEN_MAX, &back));
    CHECK_EQ(back.older[14].age_s, 126);
    full.count = 99;   /* the encoder never writes past what a trail holds */
    CHECK_EQ(lg_trail_enc(&full, wire), LG_TRAIL_LEN_MAX);
    memset(wire + LG_TRAIL_LEN_MAX, 0, 6);
    wire[16] = LG_TRAIL_POINTS;
    CHECK(!lg_trail_dec(wire, LG_TRAIL_LEN_MAX + 6u, &back));

    lg_trail_t bad = t;
    bad.subject = 0;
    (void)lg_trail_enc(&bad, wire);
    CHECK(!lg_trail_dec(wire, 35, &back));
    bad = t;
    bad.subject = LG_NODE_ID_BASE | 1u;   /* an AP has a position and no trail */
    (void)lg_trail_enc(&bad, wire);
    CHECK(!lg_trail_dec(wire, 35, &back));
    bad = t;
    bad.lat_u = 90000001;
    (void)lg_trail_enc(&bad, wire);
    CHECK(!lg_trail_dec(wire, 35, &back));
    bad = t;
    bad.fix_time = LG_POS_TIME_MIN - 1u;
    (void)lg_trail_enc(&bad, wire);
    CHECK(!lg_trail_dec(wire, 35, &back));
    bad = t;
    bad.older[1].age_s = 6;   /* younger than the point before it */
    (void)lg_trail_enc(&bad, wire);
    CHECK(!lg_trail_dec(wire, 35, &back));
}

/* D99: a handheld's last steps are held by every AP, outlive the handheld, and are fetched on request. */
static void test_trails(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_trail_t walk = {
        .lat_u = LAT0, .lon_u = LON0, .fix_time = T0, .count = 2, .older = { { -9, 0, 8 }, { -18, 3, 17 } },
    };

    /* Arguments are checked before anything is sent. */
    lg_trail_t bad = walk;
    bad.lat_u = 90000001;
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &bad), LG_ERR_ARG);
    bad = walk;
    bad.count = LG_TRAIL_POINTS;
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &bad), LG_ERR_ARG);
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), NULL), LG_ERR_ARG);
    CHECK_EQ(lg_client_request_trail(cl(s, DAD), 0), LG_ERR_ARG);

    /* Dad walks on A. Every AP holds his trail; no handheld is sent it unasked. */
    walk.subject = 77;   /* whatever the caller wrote: the handheld signs its own */
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &walk), LG_OK);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_trail_t *held = lg_node_trail(&s->nodes[i].node, LG_PROTO_DAD);
        CHECK(trail_is(held, LG_PROTO_DAD, LAT0, LON0, T0, 2));
        CHECK(held != NULL && held->older[1].dlat == -18 && held->older[1].dlon == 3 && held->older[1].age_s == 17);
        CHECK_EQ(lg_node_trail_count(&s->nodes[i].node), 1u);
        /* ... and its newest point is where he is: the way a handheld on LoRa alone is placed */
        CHECK(pos_is(lg_node_position(&s->nodes[i].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0));
    }
    for (int i = 0; i < SIM_CLIENTS; i++) {
        CHECK_EQ(s->clients[i].trail_events, 0u);
        CHECK(lg_client_trail(cl(s, i), LG_PROTO_DAD) == NULL);
    }
    CHECK_EQ(s->clients[EMMA].position_events, 1u);
    CHECK_EQ(s->clients[RANGER].position_events, 1u);
    CHECK_EQ(s->clients[DAD].position_events, 0u);   /* not echoed to the one who walked it */
    CHECK(pos_is(lg_client_position(cl(s, EMMA), LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0));

    /* Emma, two hops away, asks for it. */
    CHECK_EQ(lg_client_request_trail(cl(s, EMMA), LG_PROTO_DAD), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].trail_events, 1u);
    CHECK_EQ(s->clients[EMMA].last_trail, LG_PROTO_DAD);
    const lg_trail_t *got = lg_client_trail(cl(s, EMMA), LG_PROTO_DAD);
    CHECK(trail_is(got, LG_PROTO_DAD, LAT0, LON0, T0, 2));
    CHECK(got != NULL && got->older[0].dlat == -9 && got->older[0].age_s == 8);
    CHECK(lg_client_trail(cl(s, EMMA), LG_PROTO_ALEX) == NULL);
    CHECK_EQ(s->clients[ALEX].trail_events, 0u);   /* only the one who asked */

    /* Asking again with nothing new says nothing; asking for someone with no trail gets no answer. */
    CHECK_EQ(lg_client_request_trail(cl(s, EMMA), LG_PROTO_DAD), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].trail_events, 1u);
    CHECK_EQ(lg_client_request_trail(cl(s, RANGER), LG_PROTO_ALEX), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[RANGER].trail_events, 0u);

    /* An older or equal trail goes nowhere and changes nothing. */
    uint32_t fwd = s->nodes[0].node.stats.forwarded;
    lg_trail_t stale = walk;
    stale.lat_u = LAT0 + 500;
    stale.fix_time = T0 - 5u;
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &stale), LG_OK);
    CHECK(sim_pump(s));
    stale.fix_time = T0;
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &stale), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].node.stats.forwarded, fwd);
    CHECK(trail_is(lg_node_trail(&s->nodes[2].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0, LON0, T0, 2));

    /* A newer one replaces it on every AP, once, even with every backbone frame delivered twice. */
    lg_trail_t next = {
        .lat_u = LAT0 + 90, .lon_u = LON0, .fix_time = T0 + 9u, .count = 3,
        .older = { { -9, 0, 9 }, { -18, 0, 17 }, { -27, 3, 26 } },
    };
    s->duplicate_backbone = true;
    CHECK_EQ(lg_client_send_trail(cl(s, DAD), &next), LG_OK);
    CHECK(sim_pump(s));
    s->duplicate_backbone = false;
    for (int i = 0; i < SIM_NODES; i++) {
        CHECK(trail_is(lg_node_trail(&s->nodes[i].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));
        CHECK_EQ(lg_node_trail_count(&s->nodes[i].node), 1u);
    }
    CHECK_EQ(s->clients[EMMA].trail_events, 1u);   /* still not pushed */
    CHECK_EQ(s->clients[EMMA].position_events, 2u);   /* but where he is now, is */
    CHECK_EQ(lg_client_request_trail(cl(s, EMMA), LG_PROTO_DAD), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].trail_events, 2u);
    CHECK(trail_is(lg_client_trail(cl(s, EMMA), LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));

    /* Asking for someone else forgets the one held. */
    CHECK_EQ(lg_client_request_trail(cl(s, EMMA), LG_PROTO_ALEX), LG_OK);
    CHECK(lg_client_trail(cl(s, EMMA), LG_PROTO_DAD) == NULL);
    CHECK(sim_pump(s));

    /* Refusals: drawing someone else's trail, a body the wrong length, a scope other than system. */
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    uint32_t malformed = s->nodes[0].node.stats.malformed;
    uint8_t body[LG_TRAIL_LEN_MAX + 4];
    lg_trail_t spoof = { .subject = LG_PROTO_EMMA, .lat_u = 1, .lon_u = 1, .fix_time = T0 + 999u };
    size_t blen = lg_trail_enc(&spoof, body);
    send_session_frame(s, DAD, LG_T_TRAIL, body, blen, LG_SCOPE_SYSTEM, LG_PROTO_EMMA, 9201);
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected + 1);
    CHECK(lg_node_trail(&s->nodes[0].node, LG_PROTO_EMMA) == NULL);
    CHECK(lg_node_position(&s->nodes[0].node, LG_PROTO_EMMA) == NULL);
    lg_trail_t mine = { .subject = LG_PROTO_DAD, .lat_u = 1, .lon_u = 1, .fix_time = T0 + 999u, .count = 1 };
    blen = lg_trail_enc(&mine, body);
    send_session_frame(s, DAD, LG_T_TRAIL, body, blen - 1u, LG_SCOPE_SYSTEM, LG_PROTO_DAD, 9202);
    send_session_frame(s, DAD, LG_T_TRAIL, body, blen, LG_SCOPE_DIRECT, LG_PROTO_DAD, 9203);
    send_session_frame(s, DAD, LG_T_TRAIL_REQ, body, LG_TRAIL_REQ_LEN + 1u, LG_SCOPE_SYSTEM, LG_PROTO_DAD, 9204);
    send_session_frame(s, DAD, LG_T_TRAIL_REQ, body, LG_TRAIL_REQ_LEN, LG_SCOPE_DIRECT, LG_PROTO_DAD, 9205);
    CHECK_EQ(s->nodes[0].node.stats.malformed, malformed + 4);
    CHECK(trail_is(lg_node_trail(&s->nodes[0].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));

    /* A handheld with no session still offers its trail to the radio; "no session took it" is the answer. */
    sim_detach(s, ALEX);
    lg_trail_t alex = { .lat_u = LAT0, .lon_u = LON0 + 40, .fix_time = T0 + 20u };
    CHECK_EQ(lg_client_send_trail(cl(s, ALEX), &alex), LG_ERR_SHORT);
    CHECK(lg_node_trail(&s->nodes[1].node, LG_PROTO_ALEX) == NULL);
    sim_attach(s, ALEX, 1);

    /* A split grid: a trail walked on one side reaches the other when the link comes back. */
    sim_link(s, 1, 2, false);
    CHECK_EQ(lg_client_send_trail(cl(s, ALEX), &alex), LG_OK);
    CHECK(sim_pump(s));
    CHECK(trail_is(lg_node_trail(&s->nodes[0].node, LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 40, T0 + 20u, 0));
    CHECK(lg_node_trail(&s->nodes[2].node, LG_PROTO_ALEX) == NULL);
    sim_link(s, 1, 2, true);
    CHECK(trail_is(lg_node_trail(&s->nodes[2].node, LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 40, T0 + 20u, 0));
    CHECK(pos_is(lg_node_position(&s->nodes[2].node, LG_PROTO_ALEX), LG_PROTO_ALEX, LAT0, LON0 + 40, T0 + 20u));

    /* A handheld that restarted asks for its own trail and has its steps back (D53). */
    sim_detach(s, DAD);
    sim_attach(s, DAD, 0);
    CHECK(lg_client_trail(cl(s, DAD), LG_PROTO_DAD) == NULL);
    CHECK_EQ(lg_client_request_trail(cl(s, DAD), LG_PROTO_DAD), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].trail_events, 1u);
    CHECK(trail_is(lg_client_trail(cl(s, DAD), LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));

    /* The handheld is gone; what it walked is still on the AP it was never attached to. */
    sim_detach(s, DAD);
    CHECK(trail_is(lg_node_trail(&s->nodes[2].node, LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));
    CHECK_EQ(lg_client_request_trail(cl(s, EMMA), LG_PROTO_DAD), LG_OK);
    CHECK(sim_pump(s));
    CHECK(trail_is(lg_client_trail(cl(s, EMMA), LG_PROTO_DAD), LG_PROTO_DAD, LAT0 + 90, LON0, T0 + 9u, 3));

    /* A full table: the stalest trail gives way to a fresher one, and to nothing staler than itself. */
    lg_node_t *c = &s->nodes[2].node;
    lg_trail_t other = { .subject = 11, .lat_u = LAT0, .lon_u = LON0, .fix_time = T0 + 30u };
    backbone_trail(s, 2, 1, &other, 9301);
    other.subject = 12;
    other.fix_time = T0 + 31u;
    backbone_trail(s, 2, 1, &other, 9302);
    CHECK_EQ(lg_node_trail_count(c), LG_TRAIL_SLOTS);   /* Dad, Alex, 11, 12 */
    CHECK_EQ(c->trail_evictions, 0u);
    other.subject = 13;
    other.fix_time = T0 + 9u;   /* no fresher than the stalest held, Dad's */
    backbone_trail(s, 2, 1, &other, 9303);
    CHECK(lg_node_trail(c, 13) == NULL);
    CHECK_EQ(c->trail_evictions, 0u);
    other.fix_time = T0 + 40u;
    backbone_trail(s, 2, 1, &other, 9304);
    CHECK(trail_is(lg_node_trail(c, 13), 13, LAT0, LON0, T0 + 40u, 0));
    CHECK(lg_node_trail(c, LG_PROTO_DAD) == NULL);   /* the stalest went */
    CHECK(lg_node_trail(c, LG_PROTO_ALEX) != NULL);
    CHECK_EQ(lg_node_trail_count(c), LG_TRAIL_SLOTS);
    CHECK_EQ(c->trail_evictions, 1u);
    sim_destroy(s);
}

/* D101: update frames go one hop between APs, are never passed on, and never come from a handheld. */
static void test_fw_frames(void)
{
    sim_t *s = make_chain();   /* A - B - C */
    if (s == NULL) {
        return;
    }
    static uint8_t body[LG_FW_BODY_MAX + 1u];
    memset(body, 0x5A, sizeof(body));
    lg_node_t *a = &s->nodes[0].node;
    lg_node_t *b = &s->nodes[1].node;

    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_TEXT, body, 12), LG_ERR_ARG);
    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_FW_GET, body, 0), LG_ERR_ARG);
    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_FW_DATA, body, LG_FW_BODY_MAX + 1u), LG_ERR_ARG);
    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_FW_GET, NULL, 12), LG_ERR_ARG);

    /* To one neighbour: it arrives there, whole, and goes no further. */
    body[0] = 0x11;
    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_FW_GET, body, 14), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].fw_count, 1u);
    CHECK_EQ(s->nodes[1].fw_from, 0);
    CHECK_EQ(s->nodes[1].fw_type, LG_T_FW_GET);
    CHECK_EQ(s->nodes[1].fw_len, 14u);
    CHECK_EQ(s->nodes[1].fw_first, 0x11);
    CHECK_EQ(s->nodes[2].fw_count, 0u);
    CHECK_EQ(s->nodes[0].fw_count, 0u);

    /* To every neighbour: B has two, and each hears it once; A, two hops from C, hears nothing of C's. */
    body[0] = 0x22;
    CHECK_EQ(lg_node_send_fw(b, LG_NODE_NONE, LG_T_FW_OFFER, body, 12), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].fw_count, 1u);
    CHECK_EQ(s->nodes[2].fw_count, 1u);
    CHECK_EQ(s->nodes[2].fw_from, 1);
    body[0] = 0x33;
    CHECK_EQ(lg_node_send_fw(&s->nodes[2].node, LG_NODE_NONE, LG_T_FW_OFFER, body, 12), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[1].fw_count, 2u);
    CHECK_EQ(s->nodes[1].fw_first, 0x33);
    CHECK_EQ(s->nodes[0].fw_count, 1u);   /* not passed on */

    /* A duplicate on the air is taken once. */
    s->duplicate_backbone = true;
    body[0] = 0x44;
    CHECK_EQ(lg_node_send_fw(a, 1, LG_T_FW_DATA, body, 200), LG_OK);
    CHECK(sim_pump(s));
    s->duplicate_backbone = false;
    CHECK_EQ(s->nodes[1].fw_count, 3u);
    CHECK_EQ(s->nodes[1].fw_len, 200u);

    /* The largest body is taken (handed over directly: the simulated radio carries smaller frames),
     * and one byte more is not a frame the core will build at all. */
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.major = LG_PROTO_MAJOR;
    e.minor = LG_PROTO_MINOR;
    e.type = LG_T_FW_DATA;
    e.scope = LG_SCOPE_SYSTEM;
    e.ttl = 1;
    e.origin_id = LG_NODE_ID_BASE | 0u;
    e.origin_node = 0;
    e.origin_boot = 1;
    e.origin_seq = 9400;
    static uint8_t big_frame[LG_FRAME_MAX];
    int big_len = lg_frame_build(&e, body, LG_FW_BODY_MAX, big_frame, sizeof(big_frame));
    CHECK(big_len > 0);
    lg_node_on_backbone_frame(b, 0, big_frame, (size_t)big_len);
    CHECK_EQ(s->nodes[1].fw_count, 4u);
    CHECK_EQ(s->nodes[1].fw_len, (size_t)LG_FW_BODY_MAX);

    /* One that claims another author than the neighbour it came from is not believed. */
    memset(&e, 0, sizeof(e));
    e.major = LG_PROTO_MAJOR;
    e.minor = LG_PROTO_MINOR;
    e.type = LG_T_FW_DATA;
    e.scope = LG_SCOPE_SYSTEM;
    e.ttl = 1;
    e.origin_id = LG_NODE_ID_BASE | 2u;
    e.origin_node = 2;
    e.origin_boot = 1;
    e.origin_seq = 9401;
    uint8_t frame[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, 20, frame, sizeof(frame));
    CHECK(flen > 0);
    uint32_t malformed = a->stats.malformed;
    lg_node_on_backbone_frame(a, 1, frame, (size_t)flen);   /* C's frame, arriving from B */
    CHECK_EQ(a->stats.malformed, malformed + 1u);
    CHECK_EQ(s->nodes[0].fw_count, 1u);
    e.origin_id = LG_NODE_ID_BASE | 1u;
    e.origin_node = 1;
    e.origin_seq = 9402;
    e.scope = LG_SCOPE_BROADCAST;
    flen = lg_frame_build(&e, body, 20, frame, sizeof(frame));
    lg_node_on_backbone_frame(a, 1, frame, (size_t)flen);   /* the wrong scope */
    CHECK_EQ(a->stats.malformed, malformed + 2u);
    CHECK_EQ(s->nodes[0].fw_count, 1u);

    /* A handheld cannot put one in: on a session these types mean nothing. */
    uint32_t forwarded = a->stats.forwarded;
    send_session_frame(s, DAD, LG_T_FW_DATA, body, 20, LG_SCOPE_SYSTEM, 0, 9403);
    send_session_frame(s, DAD, LG_T_FW_OFFER, body, 12, LG_SCOPE_SYSTEM, 0, 9404);
    CHECK_EQ(s->nodes[0].fw_count, 1u);
    CHECK_EQ(s->nodes[1].fw_count, 4u);
    CHECK_EQ(a->stats.forwarded, forwarded);
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

/* A compact voice payload (D95): the Codec2 header for (talk, packet) and `frames` six-byte codec frames. */
static size_t compact_payload(uint8_t *out, uint16_t talk, uint16_t packet, uint8_t flags, size_t frames,
                              uint8_t fill)
{
    lg_voice_hdr_t h = { .codec = LG_VOICE_CODEC_C2_1200, .flags = flags, .talk = talk, .frame = packet };
    size_t n = lg_voice_hdr_enc(&h, out);
    memset(out + n, fill, frames * LG_VOICE_C2_FRAME_LEN);
    return n + frames * LG_VOICE_C2_FRAME_LEN;
}

/* The compact codec (D95): the one kind of voice that may cross LoRa. */
static void test_voice_compact(void)
{
    uint8_t p[LG_VOICE_PAYLOAD_MAX];
    /* The body: whole codec frames only, no more than a packet's worth, and the unused fields zero. */
    size_t n = compact_payload(p, 3, 0, 0, LG_VOICE_C2_FRAMES, 0x5A);
    CHECK_EQ(n, LG_VOICE_C2_PAYLOAD_MAX);
    lg_voice_hdr_t d;
    CHECK(lg_voice_hdr_dec(p, n, &d));
    CHECK_EQ(d.codec, LG_VOICE_CODEC_C2_1200);
    CHECK_EQ(d.talk, 3);
    CHECK(lg_voice_hdr_dec(p, LG_VOICE_HDR_LEN, NULL));                               /* an empty last packet */
    CHECK(!lg_voice_hdr_dec(p, LG_VOICE_HDR_LEN + LG_VOICE_C2_FRAME_LEN + 1u, NULL)); /* a codec frame and a bit */
    CHECK(!lg_voice_hdr_dec(p, n + LG_VOICE_C2_FRAME_LEN, NULL));                     /* one frame too many */
    p[8] = 1;
    CHECK(!lg_voice_hdr_dec(p, n, NULL));                                             /* a live codec's field in use */
    p[8] = 0;
    /* What bounds its airtime: envelope, payload and both seals inside two LoRa parts of 130 bytes. */
    CHECK(LG_ENV_SIZE + LG_VOICE_C2_PAYLOAD_MAX + LG_AEAD_TAG_LEN + 12u + LG_AEAD_TAG_LEN <= 260u);

    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* A group: sent by the handheld with the envelope marked, delivered to members across two hops. */
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 1);
    CHECK_EQ(s->clients[ALEX].voice_frames, 1);
    CHECK_EQ(s->clients[RANGER].voice_frames, 0);            /* not a member */
    CHECK_EQ(s->clients[EMMA].voice_len, n);
    CHECK(memcmp(s->clients[EMMA].voice_last, p, sizeof(s->clients[EMMA].voice_last)) == 0);
    CHECK_EQ(s->clients[EMMA].voice_last[0], LG_VOICE_CODEC_C2_1200);

    /* 1:1: sealed end to end like live talk, and still marked on the outside where an AP can read it. */
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    CHECK_EQ(s->clients[EMMA].voice_scope, LG_SCOPE_DIRECT);
    CHECK(memcmp(s->clients[EMMA].voice_last, p, sizeof(s->clients[EMMA].voice_last)) == 0);

    /*
     * The marker and the codec must agree wherever the codec can be read. A live frame wearing the
     * marker is what would put 400 bytes of ADPCM on a LoRa channel, so the AP refuses it; and a
     * compact frame without it is refused too, so the two never drift apart.
     */
    uint8_t live[LG_VOICE_PAYLOAD_MAX];
    size_t ln = voice_payload(live, 4, 0, 0, NULL, 40, 0x33);
    uint8_t frame[LG_FRAME_MAX];
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    int len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 700u, LG_SCOPE_GROUP, LG_PROTO_FAMILY,
                          LG_FLAG_COMPACT, live, ln);
    CHECK(len > 0);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected + 1u);
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 701u, LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, p, n);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)len);
    CHECK(sim_pump(s));
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected + 2u);
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);
    /* And a handheld drops a frame whose marker and payload disagree, whoever let it through. */
    len = voice_frame(frame, LG_PROTO_DAD, 1, LG_VOICE_SEQ_BIT | 702u, LG_SCOPE_GROUP, LG_PROTO_FAMILY,
                      LG_FLAG_COMPACT, live, ln);
    lg_client_on_frame(cl(s, EMMA), frame, (size_t)len);
    CHECK_EQ(s->clients[EMMA].voice_frames, 2);

    /*
     * Without a session a handheld still sends the compact codec - the radio may carry it (D80) - and
     * still refuses to send live talk, which nothing else can carry.
     */
    lg_client_disconnected(cl(s, DAD));
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, live, ln), LG_ERR_SHORT);
    int rc = lg_client_send_voice(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, p, n);
    CHECK(rc == LG_OK || rc == LG_ERR_FULL);   /* offered to the link; the simulated one may have nowhere to put it */
    sim_destroy(s);
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

    /*
     * A spoken announcement (D88). This used to assert LG_ERR_ARG, because voice could not be aimed at
     * everyone; it now has to reach every other handheld, and it is held to the announce permission of
     * D56 rather than a rule of its own. Checked in both directions, because the permission is the only
     * thing standing between one handheld and every speaker on the grid.
     */
    uint32_t keep_announcers = s->clients[DAD].roster.groups.announcers;
    s->clients[DAD].roster.groups.announcers = LG_ANNOUNCE_EVERYONE;
    for (size_t i = 0; i < 3u; i++) {
        s->nodes[i].roster.groups.announcers = LG_ANNOUNCE_EVERYONE;
    }
    uint16_t emma_before = s->clients[EMMA].voice_frames;
    uint16_t ranger_before = s->clients[RANGER].voice_frames;
    uint16_t alex_before = s->clients[ALEX].voice_frames;
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_BROADCAST, LG_TARGET_ALL, p, n), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, (uint16_t)(emma_before + 1u));
    CHECK_EQ(s->clients[RANGER].voice_frames, (uint16_t)(ranger_before + 1u));
    CHECK_EQ(s->clients[ALEX].voice_frames, (uint16_t)(alex_before + 1u));
    CHECK_EQ(s->clients[EMMA].voice_scope, LG_SCOPE_BROADCAST);
    /* It crosses APs, so a handheld on another one hears it too, and it is not end-to-end sealed:
     * there is no single recipient to seal it to, so the AP routes it the way it routes a group. */
    CHECK_EQ(s->clients[ALEX].voice_scope, LG_SCOPE_BROADCAST);

    /* And refused at the handheld when this device may not announce, before anything leaves. */
    s->clients[DAD].roster.groups.announcers = 1u << RANGER;
    CHECK(!lg_roster_may_announce(cl(s, DAD)->roster, cl(s, DAD)->device));
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_BROADCAST, LG_TARGET_ALL, p, n), LG_ERR_ARG);
    s->clients[DAD].roster.groups.announcers = keep_announcers;
    /* Put the listeners' counters back, so the checks further down still read absolute numbers. */
    s->clients[EMMA].voice_frames = emma_before;
    s->clients[RANGER].voice_frames = ranger_before;
    s->clients[ALEX].voice_frames = alex_before;
    s->clients[EMMA].voice_scope = LG_SCOPE_DIRECT;

    /* Arguments the handheld refuses before anything leaves. */
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
    /*
     * An announcement is legal since D88, so the malformed case is a *sealed* one: there is no single
     * recipient to seal a broadcast to, so LG_FLAG_E2E_PAYLOAD on a broadcast cannot be honest and an AP
     * that routed it would be passing on a body nobody can open.
     */
    len = voice_frame(frame, LG_PROTO_DAD, 2, LG_VOICE_SEQ_BIT | 9u, LG_SCOPE_BROADCAST, LG_TARGET_ALL,
                      LG_FLAG_E2E_PAYLOAD, p, n);
    lg_node_on_backbone_frame(&s->nodes[1].node, 0, frame, (size_t)len);
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
    /* A sealed announcement: legal scope since D88, impossible body, so still invalid. */
    len = voice_frame(frame, LG_PROTO_ALEX, 1, LG_VOICE_SEQ_BIT | 1u, LG_SCOPE_BROADCAST, LG_TARGET_ALL,
                      LG_FLAG_E2E_PAYLOAD, p, n);
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


/* ---- D80: a handheld reachable only over LoRa ---- */

/*
 * The state that says "alive, reachable, and only over LoRa" (D80). What these scenarios pin down is
 * the core's decision, not the radio: the core decides *that* a device is reachable here, and the
 * firmware glue decides which transport (io.to_client is the one door either way). So each scenario
 * leaves the simulated session in place as the stand-in for whatever transport the glue picks, and
 * changes only the presence state the core reads. Every one of these would have failed before D80,
 * because delivery demanded LG_PRES_ONLINE.
 */

/* Marks `device` reachable-only-over-LoRa on every node, as an AP that heard it would (D77). */
static void set_lora_presence(sim_t *s, uint32_t device, uint16_t at_node)
{
    for (int i = 0; i < SIM_NODES; i++) {
        for (size_t k = 0; k < LG_MAX_DEVICES; k++) {
            lg_presence_entry_t *p = &s->nodes[i].node.presence[k];
            if (p->in_use && p->device == device) {
                p->state = LG_PRES_LORA;
                p->node  = at_node;
            }
        }
    }
}

static uint8_t presence_state_of(sim_t *s, int node, uint32_t device)
{
    for (size_t k = 0; k < LG_MAX_DEVICES; k++) {
        const lg_presence_entry_t *p = &s->nodes[node].node.presence[k];
        if (p->in_use && p->device == device) {
            return p->state;
        }
    }
    return 0xFFu;
}

/* The wire format: the record is the same length, state 2 is read, and anything above is refused. */
static void test_presence_lora_body(void)
{
    uint8_t buf[LG_PRESENCE_LEN];
    lg_presence_t in = { .device = LG_PROTO_EMMA, .node = 2, .epoch = 7, .state = LG_PRES_LORA };
    memset(in.pubkey, 0xA5, sizeof(in.pubkey));

    CHECK_EQ(lg_presence_enc(&in, buf), LG_PRESENCE_LEN);   /* D80 did not change the length */
    CHECK_EQ(buf[10], LG_PRES_LORA);

    lg_presence_t out;
    CHECK(lg_presence_dec(buf, sizeof(buf), &out));
    CHECK_EQ(out.state, LG_PRES_LORA);
    CHECK_EQ(out.device, (uint32_t)LG_PROTO_EMMA);
    CHECK_EQ(out.node, 2);
    CHECK_EQ(out.epoch, 7u);

    /* The two states that came before still decode. */
    buf[10] = LG_PRES_OFFLINE;
    CHECK(lg_presence_dec(buf, sizeof(buf), &out));
    CHECK_EQ(out.state, LG_PRES_OFFLINE);
    buf[10] = LG_PRES_ONLINE;
    CHECK(lg_presence_dec(buf, sizeof(buf), &out));
    CHECK_EQ(out.state, LG_PRES_ONLINE);

    /*
     * A state above the highest we know is still refused, exactly as state 2 was refused before D80.
     * That is why D80 is a breaking change: an older device rejects the whole record rather than
     * reading an unknown state as "not online", so every board in one grid must run the same build.
     */
    buf[10] = LG_PRES_LORA + 1u;
    CHECK(!lg_presence_dec(buf, sizeof(buf), &out));
    buf[10] = 0xFFu;
    CHECK(!lg_presence_dec(buf, sizeof(buf), &out));

    /* A wrong length is still refused, whatever the state says. */
    buf[10] = LG_PRES_LORA;
    CHECK(!lg_presence_dec(buf, sizeof(buf) - 1u, &out));
}

/* A 1:1 message reaches a handheld the far AP can only reach over LoRa, two hops away. */
static void test_presence_lora_direct(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Emma is on node 2, and every AP now knows her only as reachable over LoRa. */
    set_lora_presence(s, LG_PROTO_EMMA, 2);
    CHECK_EQ(presence_state_of(s, 0, LG_PROTO_EMMA), LG_PRES_LORA);
    CHECK_EQ(presence_state_of(s, 2, LG_PROTO_EMMA), LG_PRES_LORA);

    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Are you hurt?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    /* Node 0 routed it to node 2 on the strength of the new state, and node 2 handed it over. */
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);
    CHECK(newest_is(s, EMMA, "Are you hurt?"));
    /* And the acknowledgement came back, which is the whole reason the state exists rather than
     * delivering blind: the sender learns it landed. */
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);

    /* Nobody else was given it. */
    CHECK_EQ(cl(s, ALEX)->inbox_count, 0);
    CHECK_EQ(cl(s, RANGER)->inbox_count, 0);
    /* The 1:1 plaintext still never crossed the backbone: a shared LoRa key cannot read it (D77). */
    CHECK(!sim_captured_contains(s, "Are you hurt?"));
    sim_destroy(s);
}

/*
 * An urgent broadcast reaches a handheld that is only reachable over LoRa. This is the case the
 * second radio exists for, and it goes through deliver_fanout rather than deliver_direct - a
 * different branch, which is why it gets its own scenario.
 */
static void test_presence_lora_broadcast(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    set_lora_presence(s, LG_PROTO_EMMA, 2);

    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_BROADCAST, LG_TARGET_ALL, LG_FLAG_URGENT,
                              TXT("SOS from the ridge")) >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);
    CHECK(newest_is(s, EMMA, "SOS from the ridge"));
    /* Everyone still online got it too: the new state widened delivery, it did not replace it. */
    CHECK_EQ(cl(s, ALEX)->inbox_count, 1);
    CHECK_EQ(cl(s, RANGER)->inbox_count, 1);

    /* A group message takes the same branch, so a LoRa-reachable member is included. */
    CHECK(lg_client_send_text(cl(s, DAD), LG_SCOPE_GROUP, LG_PROTO_FAMILY, 0, TXT("Heading down")) >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 2);
    CHECK(newest_is(s, EMMA, "Heading down"));
    sim_destroy(s);
}

/* A duplicate is still a duplicate, and a re-sent message is still acknowledged. */
static void test_presence_lora_duplicate(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    set_lora_presence(s, LG_PROTO_EMMA, 2);
    s->duplicate_backbone = true;   /* every backbone frame arrives twice */

    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Twice over"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, EMMA)->inbox_count, 1);   /* delivered once, not twice */
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);
    CHECK(s->nodes[2].node.stats.duplicates > 0);
    sim_destroy(s);
}

/*
 * Voice is refused to a LoRa-reachable handheld, deliberately: one 100 ms frame is seconds of
 * airtime, so D71 never offers voice to LoRa and validate_voice still demands LG_PRES_ONLINE. If
 * someone ever "fixes" that by widening it to is_local_reachable, this fails.
 */
/*
 * D80: the announcement a handheld makes over LoRa when it has no Wi-Fi session.
 *
 * What this can and cannot prove. The simulator has no LoRa transport, so this does not exercise the
 * radio, the sealing, or the AP glue that marks presence -- test_lora_hh.c covers the sealing and the
 * allow-list, and the glue between them stays untested until a module is fitted. What it does prove
 * is the part that is pure protocol and easy to get wrong: a client that believes it has no session
 * still builds a well-formed PING, the ordinary keepalive still refuses to, and an AP receiving one
 * answers it. Without the first, a handheld out of Wi-Fi range would never announce itself at all;
 * without the second, every handheld would spend a sequence number every few seconds on a socket
 * that is not there.
 */
static void test_lora_announce(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_node_t *a = &s->nodes[0].node;
    CHECK(sim_pump(s));

    /* The ordinary keepalive, with the session gone: nothing is built, so the AP sees nothing. */
    lg_client_disconnected(cl(s, DAD));
    uint32_t rx_before = a->stats.rx_client;
    lg_client_ping(cl(s, DAD), 80);
    CHECK(sim_pump(s));
    CHECK_EQ(a->stats.rx_client, rx_before);

    /*
     * The announcement, with the session still gone. On a handheld this frame would go to the radio;
     * here it reaches the AP the same way, which is enough to show it is a frame an AP accepts and
     * answers rather than one it rejects as malformed or unauthorised.
     */
    uint32_t rejected = a->stats.rejected, malformed = a->stats.malformed;
    CHECK(lg_client_lora_announce(cl(s, DAD), 80));
    CHECK(sim_pump(s));
    CHECK_EQ(a->stats.rx_client, rx_before + 1);
    CHECK_EQ(a->stats.rejected, rejected);       /* not author spoofing: it is that handheld's own */
    CHECK_EQ(a->stats.malformed, malformed);     /* and a well-formed envelope */

    /* The battery rode along, which is what the AP's status beacon shows (D68). */
    int bi = lg_roster_user_index(&s->nodes[0].roster, LG_PROTO_DAD);
    CHECK(bi >= 0);
    CHECK_EQ(a->battery[bi], 80);

    /* An impossible battery is sent as unknown rather than refused: a bad reading must not cost the
     * announcement, which is the only thing telling the grid this handheld is alive. */
    CHECK(lg_client_lora_announce(cl(s, DAD), 200));
    CHECK(sim_pump(s));
    CHECK_EQ(a->battery[bi], LG_BATTERY_UNKNOWN);

    /* And it is still refused when there is nowhere at all to send it. */
    sim_detach(s, DAD);
    CHECK(sim_pump(s));
    sim_destroy(s);
}

static void test_presence_lora_voice_refused(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* A real voice payload: a raw buffer is refused by the client before it ever reaches a node. */
    uint8_t voice[LG_VOICE_PAYLOAD_MAX];
    size_t vn = voice_payload(voice, 3, 0, 0, NULL, 64, 0x5A);

    /* While Emma is properly online, a talk frame reaches her: the baseline this compares against. */
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, voice, vn), LG_OK);
    CHECK(sim_pump(s));
    uint32_t heard = s->clients[EMMA].voice_frames;
    CHECK(heard > 0);

    /* Now she is reachable only over LoRa, and live voice stops: not one more frame arrives. It is
     * not refused either (D95), because a refusal stops the talk at the sender, and the compact frames
     * of that same talk are what does reach her. */
    set_lora_presence(s, LG_PROTO_EMMA, 2);
    uint32_t refusals = s->clients[DAD].voice_refusals;
    (void)lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, voice, vn);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, heard);
    CHECK_EQ(s->clients[DAD].voice_refusals, refusals);
    uint8_t compact[LG_VOICE_C2_PAYLOAD_MAX];
    size_t cn = compact_payload(compact, 3, 0, 0, 10, 0x6B);
    CHECK_EQ(lg_client_send_voice(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, compact, cn), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[EMMA].voice_frames, heard + 1u);
    CHECK_EQ(s->clients[EMMA].voice_last[0], LG_VOICE_CODEC_C2_1200);

    /*
     * One AP carries a handheld that is on LoRa alone. Her frames are broadcast on the radio, so the AP
     * next door hears them too - and it must not claim her while the AP that has her is still up, or
     * both would transmit everything addressed to her at once and the copies would collide.
     */
    lg_node_mark_lora_reachable(&s->nodes[1].node, LG_PROTO_EMMA);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[i].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_LORA && pe->node == 2);
    }
    /* An AP that cannot reach hers does claim her, and the lower index is then agreed everywhere. */
    lg_node_mark_lora_reachable(&s->nodes[0].node, LG_PROTO_EMMA);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[i].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_LORA && pe->node == 0);
    }
    sim_destroy(s);
}

/*
 * A LoRa frame heard from a handheld that has a session is not a reason to take it (D80).
 *
 * The bench found this on 2026-10-05. A handheld moving between APs is without Wi-Fi for half a second
 * and announces itself over LoRa; the frame is a second on the air and arrives after it has registered
 * again. The AP that heard it claimed the handheld, no other AP agreed, and nothing ever settled it:
 * for the rest of a 90 minute run that AP sent every direct message for the handheld out over LoRa
 * alone, and one did not arrive.
 */
static void test_lora_frame_does_not_take_a_session(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Emma has a session on node 2. Node 1 is its neighbour and hears her late announcement. */
    lg_node_mark_lora_reachable(&s->nodes[1].node, LG_PROTO_EMMA);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[i].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_ONLINE && pe->node == 2);
    }
    /*
     * Node 0 cannot see node 2, so it cannot know that AP is up and does claim her. The AP with her
     * session answers at once and every AP, the claimant included, is back to the truth without
     * waiting for a periodic announcement.
     */
    lg_node_mark_lora_reachable(&s->nodes[0].node, LG_PROTO_EMMA);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[i].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_ONLINE && pe->node == 2);
    }
    /* And a direct message from node 0 goes to her AP and is acknowledged. */
    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Still there?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Still there?"));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);

    /*
     * A split left by an older build, or by a refutation that was lost: one AP holds "LoRa, through
     * me" at the same epoch as the session. The periodic announcement from her AP settles it.
     */
    for (size_t k = 0; k < LG_MAX_DEVICES; k++) {
        lg_presence_entry_t *p = &s->nodes[1].node.presence[k];
        if (p->in_use && p->device == LG_PROTO_EMMA) {
            p->state = LG_PRES_LORA;
            p->node  = 1;
        }
    }
    lg_node_announce_presence(&s->nodes[2].node);
    CHECK(sim_pump(s));
    {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[1].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_ONLINE && pe->node == 2);
    }

    /*
     * Her session ends, and her AP says so. Now a frame heard over LoRa is the best anyone has: the AP
     * that hears it takes her, at the epoch of the session that ended, and every AP agrees.
     */
    sim_detach(s, EMMA);
    CHECK(sim_pump(s));
    CHECK_EQ(presence_state_of(s, 0, LG_PROTO_EMMA), LG_PRES_OFFLINE);
    lg_node_mark_lora_reachable(&s->nodes[1].node, LG_PROTO_EMMA);
    CHECK(sim_pump(s));
    for (int i = 0; i < SIM_NODES; i++) {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[i].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_LORA && pe->node == 1);
    }
    /* An AP that missed the claim learns it from the periodic announcement (D48). */
    for (size_t k = 0; k < LG_MAX_DEVICES; k++) {
        lg_presence_entry_t *p = &s->nodes[0].node.presence[k];
        if (p->in_use && p->device == LG_PROTO_EMMA) {
            p->state = LG_PRES_OFFLINE;
            p->node  = 2;
        }
    }
    lg_node_announce_presence(&s->nodes[1].node);
    CHECK(sim_pump(s));
    {
        const lg_presence_entry_t *pe = lg_node_presence(&s->nodes[0].node, LG_PROTO_EMMA);
        CHECK(pe != NULL && pe->state == LG_PRES_LORA && pe->node == 1);
    }
    sim_destroy(s);
}

/*
 * D77, D80: a handheld reachable only over LoRa can originate, not only receive.
 *
 * The bench found this on 2026-09-30. With a handheld's Wi-Fi switched off (D78) a message sent to it
 * arrived over LoRa and was shown, but its own reply never reached the air: one frame went out in
 * 150 s and that one was the announcement. Three gates in this file were the cause - send_frame
 * refused unless `connected`, the first attempt happened only `if (registered)`, and lg_client_tick
 * returned early unless registered - and all three are right for a socket. On the second radio there
 * is no session to have, so each of them meant the one device that most needed to speak could not.
 *
 * These scenarios fail if any of the three is put back.
 */
static void test_lora_only_can_originate(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* Emma loses Wi-Fi at node C. Node C still hears her over LoRa; nothing else does. */
    sim_lora(s, EMMA, 2);
    sim_detach(s, EMMA);
    CHECK(!cl(s, EMMA)->connected);
    CHECK(!cl(s, EMMA)->registered);

    uint32_t offered = s->clients[EMMA].offers_detached;
    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT,
                                  TXT("SOS from the ridge"));
    CHECK(slot >= 0);
    /* The gate that was shut: with no session the frame was never offered to a radio at all. */
    CHECK(s->clients[EMMA].offers_detached > offered);
    /* Not refused for the one reason that is normal here. */
    CHECK_EQ(cl(s, EMMA)->outbox[slot].state, LG_OUT_PENDING);
    CHECK(sim_pump(s));

    /* It crossed the grid from a handheld with no session, two hops to Dad on node A. */
    CHECK(newest_is(s, DAD, "SOS from the ridge"));
    CHECK(newest_is(s, ALEX, "SOS from the ridge"));
    CHECK(newest_is(s, RANGER, "SOS from the ridge"));
    /* And the AP that heard it says how she is reachable, so a reply knows which path to take. */
    CHECK_EQ(presence_state_of(s, 2, LG_PROTO_EMMA), LG_PRES_LORA);

    /*
     * Wi-Fi comes back and the slot is still PENDING, so it is sent again - deliberately, because one
     * LoRa attempt is not a delivery. It carries the same (boot, seq, grid time), so it is the same
     * message and the grid drops it by identity. One copy, not two.
     */
    uint32_t had = cl(s, DAD)->inbox_count;
    sim_attach(s, EMMA, 2);
    CHECK(sim_pump(s));
    lg_client_tick(cl(s, EMMA));
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, DAD)->inbox_count, had);
    sim_destroy(s);
}

/*
 * The acknowledgement goes back over LoRa, which is the point of D80 rather than delivering blind:
 * the sender learns their message landed. Before the gates opened, a LoRa-only handheld received the
 * message and the sender waited for ever.
 */
/*
 * D77, D80: with no session, retries are paced by the air rather than by the outbox.
 *
 * The bench measured why this matters on 2026-09-30: a handheld on LoRa alone delivered 13 of 15
 * messages at a median 1.3 s, so the radio was not the constraint - but the two it dropped had no
 * second attempt for 30 s, which for an SOS is the wrong trade. The interval came down to 10 s, and the
 * rate is bounded by messages per tick instead, so a full outbox offers no more than one message at a
 * time and the oldest goes first (D74: what someone is waiting for goes first).
 */
/*
 * D86: an alert too old for the clock rule is sent again under a new id; an ordinary message is not.
 *
 * The bench found the conflict on 2026-09-30. An access point refuses a message whose grid time is more
 * than LG_TIME_TOLERANCE_S from its own (D6), and a retransmission otherwise reuses its stamp, so a
 * message delayed past that window - which is what an outage does - could never be delivered by any
 * path. Six messages hit it in the first chaos run of the handheld LoRa leg.
 *
 * The owner chose arriving possibly twice over not arriving, for urgent broadcasts, an SOS and an all
 * clear only. These scenarios hold both halves of that: the alert gets a new id, and the chat message
 * does not.
 */
/*
 * The outbox must not fill with messages nothing can accept, because a full outbox is a handheld that
 * cannot speak.
 *
 * Found by a chaos night on 2026-10-01 and the worst thing found so far. Jolly went **permanently mute**:
 * `[MSG] Not sent (outbox full)` twenty-seven times, still refusing twenty minutes after it had rejoined
 * Wi-Fi, while its screen showed it online with a good signal. A message nothing acknowledges stayed
 * PENDING for ever; D6 means one delayed past LG_TIME_TOLERANCE_S can never be accepted, because a
 * retransmission keeps its stamp; so each one held a slot for ever, and after LG_OUTBOX_SIZE of them the
 * handheld could send nothing at all - including an SOS.
 *
 * This scenario fails if a stale message is ever kept, or if a fresh one is ever thrown away.
 */
/*
 * A 1:1 message the grid accepted, but whose recipient never confirms, must not hold its outbox slot for
 * ever - and an alert must never be refused because such messages fill it.
 *
 * Found by chaos on 2026-10-01, and it is the muteness bug reached by a second road. The first fix ages
 * out messages stuck in LG_OUT_PENDING; this is LG_OUT_ACCEPTED, which pick_slot held for a 1:1 message
 * until the recipient's device confirmed delivery. When the recipient cannot confirm - in the run its
 * Wi-Fi was off for a LoRa round - every message to it keeps a slot. Three handhelds filled their
 * outboxes that way and refused everything with "outbox full" for the rest of the night: 176 refusals,
 * among them an announcement, an urgent broadcast and an **all clear**, and clean delivery fell from
 * 99.5% to 66.5%.
 *
 * The outbox is put into that state directly rather than raced into it. The simulator delivers or
 * rejects a 1:1 message within one pump, so there is no window in it where a node has accepted a message
 * that the recipient will never confirm - the very gap that made this bug hard to see. What is under
 * test is the slot policy, so the policy is what this sets up and checks.
 */
static void test_unconfirmed_direct_does_not_gag_the_handheld(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    lg_client_t *c = cl(s, DAD);
    /* Every slot holding a 1:1 message a node took and nobody has confirmed. */
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        lg_out_msg_t *m = &c->outbox[i];
        memset(m, 0, sizeof(*m));
        m->state = LG_OUT_ACCEPTED;
        m->scope = LG_SCOPE_DIRECT;
        m->target = LG_PROTO_ALEX;
        m->seq = (uint32_t)(i + 1u);
        m->grid_time = s->grid_time;   /* just sent, so still inside the hold */
        m->len = 4;
        memcpy(m->text, "hi", 2);
    }

    /* While they are fresh their slots are held: a delivery tick is worth keeping. */
    CHECK_EQ(lg_client_send_text(c, LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("one more")), LG_ERR_FULL);

    /* An alert takes one anyway, because raising an alarm outranks a receipt. */
    CHECK(lg_client_send_text(c, LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS")) >= 0);
    /* And so does the all clear that stands it down (D66, D79) - the message this run saw refused. */
    CHECK(lg_client_send_text(c, LG_SCOPE_BROADCAST, 0, LG_FLAG_ALL_CLEAR, TXT("I am safe")) >= 0);

    /* Past the hold an ordinary message may take one too, so the handheld does not depend on alerts to
     * say anything at all. Both clocks move: the hold is measured against the handheld's own. */
    uint32_t waited = LG_ACCEPTED_HOLD_S + 5u;
    s->grid_time += waited;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        s->clients[i].clock += waited;
    }
    CHECK(lg_client_send_text(c, LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("still here")) >= 0);
    sim_destroy(s);
}

static void test_outbox_does_not_fill_with_undeliverable(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /* No access point in earshot, so nothing is acknowledged and every message stays pending. */
    sim_lora(s, EMMA, -1);
    sim_detach(s, EMMA);

    size_t queued = 0;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        if (lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, 0, TXT("waiting")) >= 0) {
            queued++;
        }
    }
    CHECK_EQ(queued, LG_OUTBOX_SIZE);
    /* Full: the next one has nowhere to go, which is the state the handheld was stuck in for ever. */
    CHECK_EQ(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, 0, TXT("one more")), LG_ERR_FULL);

    /* A tick while they are still fresh keeps every one of them: nothing is thrown away early. */
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));
    size_t pending = 0;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        pending += cl(s, EMMA)->outbox[i].state == LG_OUT_PENDING;
    }
    CHECK_EQ(pending, LG_OUTBOX_SIZE);

    /* Now let the clock run past the point where no access point would take them. */
    uint32_t waited = LG_TIME_TOLERANCE_S + LG_TIME_RESTAMP_MARGIN_S + 5u;
    s->grid_time += waited;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        s->clients[i].clock += waited;
    }
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));

    /* Each is failed with the reason the sender would have been given, and its slot is free. */
    size_t refused = 0;
    pending = 0;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &cl(s, EMMA)->outbox[i];
        if (m->state == LG_OUT_REJECTED && m->reject_reason == LG_ACK_REJ_TIME) {
            refused++;
        }
        pending += m->state == LG_OUT_PENDING;
    }
    CHECK_EQ(refused, LG_OUTBOX_SIZE);
    CHECK_EQ(pending, 0u);

    /* And the handheld can speak again, which is the whole point. */
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS")) >= 0);
    sim_destroy(s);
}

/* An alert is not aged out while it still has a re-stamp left (D86): it is about to get a new id. */
static void test_alert_is_not_aged_out_while_it_can_be_restamped(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_lora(s, EMMA, -1);
    sim_detach(s, EMMA);
    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS"));
    CHECK(slot >= 0);

    uint32_t step = LG_TIME_TOLERANCE_S + LG_TIME_RESTAMP_MARGIN_S + 5u;
    for (unsigned round = 0; round < LG_RESTAMP_MAX; round++) {
        s->grid_time += step;
        for (int i = 0; i < SIM_CLIENTS; i++) {
            s->clients[i].clock += step;
        }
        s->now_ms += LG_RESEND_LORA_MS + 1;
        lg_client_tick(cl(s, EMMA));
        /* Still trying, with a new identity each time, rather than aged out. */
        CHECK_EQ(cl(s, EMMA)->outbox[slot].state, LG_OUT_PENDING);
    }
    CHECK_EQ(cl(s, EMMA)->outbox[slot].restamps, LG_RESTAMP_MAX);

    /* Once the re-stamps are spent it ages out like anything else, instead of holding a slot for ever. */
    s->grid_time += step;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        s->clients[i].clock += step;
    }
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));
    CHECK_EQ(cl(s, EMMA)->outbox[slot].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].reject_reason, LG_ACK_REJ_TIME);
    sim_destroy(s);
}

static void test_old_alert_is_restamped(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_lora(s, EMMA, 2);
    sim_detach(s, EMMA);

    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT,
                                   TXT("SOS from the ridge"));
    CHECK(slot >= 0);
    uint32_t first_seq = cl(s, EMMA)->outbox[slot].seq;
    uint32_t first_stamp = cl(s, EMMA)->outbox[slot].grid_time;
    CHECK_EQ(cl(s, EMMA)->outbox[slot].restamps, 0);

    /* Still fresh: a retry keeps the same id, because identity is what the grid deduplicates on. */
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));
    CHECK_EQ(cl(s, EMMA)->outbox[slot].seq, first_seq);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].grid_time, first_stamp);

    /*
     * Now let the clock run past the point where an access point would refuse it. The simulator's grid
     * time and the handhelds' clocks both move, so this is an old message and not a wrong clock.
     */
    uint32_t waited = LG_TIME_TOLERANCE_S - LG_TIME_RESTAMP_MARGIN_S + 5u;
    s->grid_time += waited;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        s->clients[i].clock += waited;
    }
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));

    /* A new id and a new stamp: that is what lets the grid accept it at all. */
    CHECK(cl(s, EMMA)->outbox[slot].seq != first_seq);
    CHECK(cl(s, EMMA)->outbox[slot].grid_time > first_stamp);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].restamps, 1);
    /* The words are untouched: it is the same alert, said again. */
    CHECK_EQ(cl(s, EMMA)->outbox[slot].len, strlen("SOS from the ridge"));

    /* And it now crosses, which it could not have done with its old stamp. */
    CHECK(sim_pump(s));
    CHECK(newest_is(s, DAD, "SOS from the ridge"));
    sim_destroy(s);
}

static void test_old_chat_message_keeps_its_id(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_lora(s, EMMA, 2);
    sim_detach(s, EMMA);

    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_DIRECT, LG_PROTO_DAD, 0, TXT("see you at six"));
    CHECK(slot >= 0);
    uint32_t seq = cl(s, EMMA)->outbox[slot].seq;
    uint32_t stamp = cl(s, EMMA)->outbox[slot].grid_time;

    uint32_t waited = LG_TIME_TOLERANCE_S + 60u;
    s->grid_time += waited;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        s->clients[i].clock += waited;
    }
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));

    /* Unchanged, however old it is: nobody may see a chat message twice (D86 covers alerts only). */
    CHECK_EQ(cl(s, EMMA)->outbox[slot].seq, seq);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].grid_time, stamp);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].restamps, 0);
    sim_destroy(s);
}

/* And it stops: an alert nothing can deliver must not re-stamp for ever. */
static void test_restamping_is_bounded(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_lora(s, EMMA, -1);     /* a module with no access point in earshot: nothing can land */
    sim_detach(s, EMMA);
    int slot = lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("SOS"));
    CHECK(slot >= 0);

    uint32_t step = LG_TIME_TOLERANCE_S - LG_TIME_RESTAMP_MARGIN_S + 5u;
    for (unsigned round = 0; round < LG_RESTAMP_MAX + 3u; round++) {
        s->grid_time += step;
        for (int i = 0; i < SIM_CLIENTS; i++) {
            s->clients[i].clock += step;
        }
        s->now_ms += LG_RESEND_LORA_MS + 1;
        lg_client_tick(cl(s, EMMA));
    }
    CHECK_EQ(cl(s, EMMA)->outbox[slot].restamps, LG_RESTAMP_MAX);
    /*
     * And then it is given up on, rather than held for ever. This expectation was the other way round
     * when it was first written, which was the leak: a chaos night on 2026-10-01 filled a handheld's
     * outbox with messages nothing could accept and left it **permanently unable to send**, an SOS
     * included. A message that cannot be delivered is failed with its reason so the slot comes back.
     */
    CHECK_EQ(cl(s, EMMA)->outbox[slot].state, LG_OUT_REJECTED);
    CHECK_EQ(cl(s, EMMA)->outbox[slot].reject_reason, LG_ACK_REJ_TIME);
    sim_destroy(s);
}

static void test_lora_only_retry_is_paced(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    /*
     * A module with no access point in earshot (-1), so the three messages stay PENDING and there is
     * something to re-offer. The first version of this scenario gave her a working LoRa path and then
     * pumped the simulator, which delivered all three - so nothing was pending, the tick correctly
     * offered nothing, and the test failed for the one reason that was not the behaviour under test.
     */
    sim_lora(s, EMMA, -1);
    sim_detach(s, EMMA);

    /* Three waiting at once, queued in this order. */
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("first")) >= 0);
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("second")) >= 0);
    CHECK(lg_client_send_text(cl(s, EMMA), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT, TXT("third")) >= 0);
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &cl(s, EMMA)->outbox[i];
        CHECK(m->state == LG_OUT_EMPTY || m->state == LG_OUT_PENDING);
    }

    /* Nothing is due yet, so a tick offers nothing however many are waiting. */
    uint32_t offered = s->clients[EMMA].offers_detached;
    lg_client_tick(cl(s, EMMA));
    CHECK_EQ(s->clients[EMMA].offers_detached, offered);

    /* One tick after the interval offers exactly one, not three. */
    s->now_ms += LG_RESEND_LORA_MS + 1;
    lg_client_tick(cl(s, EMMA));
    CHECK_EQ(s->clients[EMMA].offers_detached, offered + LG_RESEND_LORA_MAX_PER_TICK);

    /* And it is the oldest of the three: the one with the smallest sequence number. */
    uint32_t lowest = 0xFFFFFFFFu, resent = 0;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &cl(s, EMMA)->outbox[i];
        if (m->state == LG_OUT_PENDING && m->seq < lowest) {
            lowest = m->seq;
        }
    }
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &cl(s, EMMA)->outbox[i];
        if (m->state == LG_OUT_PENDING && m->last_tx_ms == s->now_ms && m->seq == lowest) {
            resent++;
        }
    }
    CHECK_EQ(resent, 1u);

    /* A registered handheld is unchanged: the socket's own interval, every message that is due. */
    sim_attach(s, EMMA, 2);
    CHECK(sim_pump(s));
    CHECK(cl(s, EMMA)->registered);
    uint32_t before_wifi = s->clients[EMMA].offers_detached;
    s->now_ms += LG_RESEND_MS + 1;
    lg_client_tick(cl(s, EMMA));
    /* With a session, nothing goes through the detached path at all. */
    CHECK_EQ(s->clients[EMMA].offers_detached, before_wifi);
    sim_destroy(s);
}

static void test_lora_only_acknowledges(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_lora(s, EMMA, 2);
    sim_detach(s, EMMA);

    /*
     * She announces first, which is the whole reason that frame exists (D80): until an AP has heard
     * something from her, the grid knows her as offline and a message to her is refused rather than
     * sent down a path nobody knows about. This is the real sequence - the handheld says "I am here
     * on this radio", and only then can anything come back.
     */
    CHECK(lg_client_lora_announce(cl(s, EMMA), 55));
    CHECK(sim_pump(s));
    CHECK_EQ(presence_state_of(s, 2, LG_PROTO_EMMA), LG_PRES_LORA);
    CHECK_EQ(presence_state_of(s, 0, LG_PROTO_EMMA), LG_PRES_LORA);   /* and it reached Dad's AP */

    int slot = lg_client_send_text(cl(s, DAD), LG_SCOPE_DIRECT, LG_PROTO_EMMA, 0, TXT("Are you hurt?"));
    CHECK(slot >= 0);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, EMMA, "Are you hurt?"));
    CHECK_EQ(cl(s, DAD)->outbox[slot].state, LG_OUT_DELIVERED);
    /* Still sealed end to end: the shared LoRa key never sees the words (D77). */
    CHECK(!sim_captured_contains(s, "Are you hurt?"));
    sim_destroy(s);
}

/*
 * A handheld with no LoRa module behaves as it always did. The offer is made - the core cannot know
 * whether a radio is there, and must not guess - but nothing carries it, the message waits instead of
 * failing, and it is delivered once when a session returns.
 */
static void test_no_second_radio_unchanged(void)
{
    sim_t *s = make_chain();
    if (s == NULL) {
        return;
    }
    sim_detach(s, ALEX);               /* no sim_lora: this board has no module */
    uint32_t offered = s->clients[ALEX].offers_detached;
    int slot = lg_client_send_text(cl(s, ALEX), LG_SCOPE_BROADCAST, 0, LG_FLAG_URGENT,
                                   TXT("Lost the path"));
    CHECK(slot >= 0);
    CHECK(s->clients[ALEX].offers_detached > offered);
    CHECK(sim_pump(s));
    CHECK_EQ(cl(s, ALEX)->outbox[slot].state, LG_OUT_PENDING);   /* waiting, not rejected */
    CHECK_EQ(cl(s, DAD)->inbox_count, 0);                        /* and nothing arrived */

    sim_attach(s, ALEX, 1);
    CHECK(sim_pump(s));
    CHECK(newest_is(s, DAD, "Lost the path"));
    CHECK_EQ(cl(s, DAD)->inbox_count, 1);
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
    test_presence_lora_body();
    test_presence_lora_direct();
    test_presence_lora_broadcast();
    test_presence_lora_duplicate();
    test_presence_lora_voice_refused();
    test_lora_announce();
    test_lora_only_can_originate();
    test_lora_frame_does_not_take_a_session();
    test_lora_only_acknowledges();
    test_lora_only_retry_is_paced();
    test_old_alert_is_restamped();
    test_old_chat_message_keeps_its_id();
    test_restamping_is_bounded();
    test_outbox_does_not_fill_with_undeliverable();
    test_unconfirmed_direct_does_not_gag_the_handheld();
    test_alert_is_not_aged_out_while_it_can_be_restamped();
    test_no_second_radio_unchanged();
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
    test_trail_body();
    test_trails();
    test_fw_frames();
    test_voice_body();
    test_voice_compact();
    test_voice_group();
    test_voice_direct();
    test_voice_duplicates_and_order();
    test_voice_refusals();
    test_voice_keeps_text_fresh();
}
