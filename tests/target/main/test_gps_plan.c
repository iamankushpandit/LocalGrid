/*
 * D73: the GPS is sampled, not resident. Everything here runs on a bench board with no GPS
 * fitted, because none of it touches a UART: it is the stored form the grid replicates, the
 * scheduler's decisions, and the promise that "always on" is exactly what it was before D73.
 *
 * What still needs a module on a board: how long a reading actually takes to a fix, and how many
 * bytes the UART driver gives back. Those are measured on MAIN and a handheld, not here.
 */
#include <string.h>

#include "lg_body.h"
#include "lg_client.h"
#include "lg_envelope.h"
#include "lg_gps_plan.h"
#include "lg_node.h"
#include "lg_test.h"
#include "lg_types.h"
#include "sim.h"

/* ---- the stored form (D49): one u16 that every AP and handheld reads the same way ---- */

static void test_stored_form(void)
{
    /* A settings record written before D73 has no such field and reads as zero: the default. */
    CHECK_EQ(lg_gps_plan_seconds(LG_GPS_PLAN_DEFAULT), LG_GPS_PLAN_DEFAULT_S);
    CHECK_EQ(lg_gps_plan_seconds(0), 300);
    CHECK(lg_gps_plan_valid(LG_GPS_PLAN_DEFAULT));

    /* Always on is its own value, and means "no interval", not "an interval of zero". */
    CHECK_EQ(lg_gps_plan_seconds(LG_GPS_PLAN_ALWAYS), 0);
    CHECK(lg_gps_plan_valid(LG_GPS_PLAN_ALWAYS));

    /* The choices the admin page offers. */
    const uint16_t offered[] = { 60, 300, 600, 900, 1800 };
    for (size_t i = 0; i < sizeof(offered) / sizeof(offered[0]); i++) {
        CHECK(lg_gps_plan_valid(offered[i]));
        CHECK_EQ(lg_gps_plan_seconds(offered[i]), offered[i]);
        CHECK_EQ(lg_gps_plan_canon(offered[i]), offered[i]);
    }

    /* Out of range is clamped, never refused: a number from an AP on newer firmware still leaves
     * this one reading on a sane schedule, and canon makes every device hold the same value. */
    CHECK(!lg_gps_plan_valid(5));
    CHECK_EQ(lg_gps_plan_seconds(5), LG_GPS_PLAN_MIN_S);
    CHECK_EQ(lg_gps_plan_canon(5), LG_GPS_PLAN_MIN_S);
    CHECK(!lg_gps_plan_valid(9000));
    CHECK_EQ(lg_gps_plan_seconds(9000), LG_GPS_PLAN_MAX_S);
    CHECK_EQ(lg_gps_plan_canon(9000), LG_GPS_PLAN_MAX_S);
    CHECK_EQ(lg_gps_plan_canon(LG_GPS_PLAN_ALWAYS), LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(lg_gps_plan_canon(LG_GPS_PLAN_DEFAULT), LG_GPS_PLAN_DEFAULT);

    /* Two bytes on the wire and in the settings, little-endian, whatever the value. */
    uint8_t body[LG_GPS_PLAN_BODY_LEN];
    CHECK_EQ(sizeof(body), LG_GPS_PLAN_LEN);
    lg_wr16(body, 1800);
    CHECK_EQ(body[0], 1800u & 0xFFu);
    CHECK_EQ(body[1], 1800u >> 8);
    CHECK_EQ(lg_rd16(body), 1800);
    lg_wr16(body, LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(lg_rd16(body), LG_GPS_PLAN_ALWAYS);
}

/* ---- "always on" behaves exactly as before D73 ---- */

static void test_always_on(void)
{
    lg_gps_plan_t p;
    uint32_t t = 100000u;
    lg_gps_plan_init(&p, LG_GPS_PLAN_ALWAYS, t);
    CHECK(lg_gps_plan_always(&p));
    CHECK(lg_gps_plan_reading(&p));
    CHECK_EQ(p.readings, 1);

    /* A fix, a settle, an hour: nothing is ever closed, so no UART is ever released. */
    lg_gps_plan_on_fix(&p, t + 500u);
    for (uint32_t i = 0; i < 3600u; i++) {
        CHECK_EQ(lg_gps_plan_tick(&p, t + i * 1000u), LG_GPS_ACT_NONE);
    }
    CHECK(lg_gps_plan_reading(&p));
    CHECK_EQ(p.readings, 1);   /* one reading, never ended */
    CHECK_EQ(lg_gps_plan_next_in_s(&p, t + 3600000u), 0);
    CHECK_EQ(lg_gps_plan_hold_ms(&p), UINT32_MAX);   /* the caller's own 5 s freshness rule applies */
}

/* ---- a reading, then the port goes back, then the next reading ---- */

static void test_schedule(void)
{
    lg_gps_plan_t p;
    uint32_t t = 0;
    lg_gps_plan_init(&p, 300, t);
    CHECK(!lg_gps_plan_always(&p));
    CHECK(lg_gps_plan_reading(&p));   /* a device wants a fix as soon as it boots */

    /* Nothing closes while the reading is still looking. */
    CHECK_EQ(lg_gps_plan_tick(&p, t + 1000u), LG_GPS_ACT_NONE);

    /* A fix at 4 s: the reading stays open a moment for GGA and GSV, then the port goes back. */
    lg_gps_plan_on_fix(&p, t + 4000u);
    CHECK_EQ(p.fixes, 1);
    CHECK_EQ(p.last_ttf_ms, 4000);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 4500u), LG_GPS_ACT_NONE);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 6000u), LG_GPS_ACT_CLOSE);
    CHECK(!lg_gps_plan_reading(&p));
    CHECK_EQ(p.last_open_ms, 6000);

    /* The interval runs from the start of the reading, not from its end, so readings do not drift. */
    CHECK_EQ(lg_gps_plan_next_in_s(&p, t + 6000u), 294);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 299000u), LG_GPS_ACT_NONE);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 300000u), LG_GPS_ACT_OPEN);
    CHECK(lg_gps_plan_reading(&p));
    CHECK_EQ(p.readings, 2);
    CHECK(!p.fix);   /* the new reading has not found one yet */
    CHECK_EQ(lg_gps_plan_next_in_s(&p, t + 300000u), 0);

    /* A reading that finds nothing gives up at the window and waits for its turn again. */
    CHECK_EQ(lg_gps_plan_tick(&p, t + 300000u + LG_GPS_PLAN_WINDOW_MS - 1u), LG_GPS_ACT_NONE);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 300000u + LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_CLOSE);
    CHECK_EQ(p.fixes, 1);   /* still just the one */
    CHECK_EQ(lg_gps_plan_tick(&p, t + 600000u), LG_GPS_ACT_OPEN);
    CHECK_EQ(p.readings, 3);

    /* A fix from a reading stays the answer until the next reading is overdue (D63, D65). */
    CHECK(lg_gps_plan_hold_ms(&p) > 300000u);
    CHECK_EQ(lg_gps_plan_hold_ms(&p), 300000u + LG_GPS_PLAN_WINDOW_MS + 5000u);
}

/* A reading that outruns its own interval is followed straight away rather than skipped. */
static void test_short_interval(void)
{
    lg_gps_plan_t p;
    lg_gps_plan_init(&p, 30, 0);
    CHECK_EQ(lg_gps_plan_tick(&p, LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_CLOSE);
    CHECK_EQ(lg_gps_plan_next_in_s(&p, LG_GPS_PLAN_WINDOW_MS), 0);
    CHECK_EQ(lg_gps_plan_tick(&p, LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_OPEN);
    CHECK_EQ(p.readings, 2);
}

/* ---- the grid changing the interval, and the console asking for a reading now ---- */

static void test_interval_change(void)
{
    lg_gps_plan_t p;
    lg_gps_plan_init(&p, 1800, 0);
    lg_gps_plan_on_fix(&p, 3000u);
    CHECK_EQ(lg_gps_plan_tick(&p, 5000u), LG_GPS_ACT_CLOSE);
    CHECK_EQ(lg_gps_plan_next_in_s(&p, 5000u), 1795);

    /* A shorter interval takes effect against the reading it follows, not from when it arrived. */
    CHECK(lg_gps_plan_set(&p, 60, 5000u));
    CHECK_EQ(lg_gps_plan_next_in_s(&p, 5000u), 55);
    CHECK(!lg_gps_plan_set(&p, 60, 5000u));   /* the same number changes nothing */

    /* An interval already outlived opens at once rather than never. */
    CHECK(lg_gps_plan_set(&p, 1800, 6000u));
    CHECK(lg_gps_plan_set(&p, 30, 100000u));
    CHECK_EQ(lg_gps_plan_next_in_s(&p, 100000u), 0);
    CHECK_EQ(lg_gps_plan_tick(&p, 100000u), LG_GPS_ACT_OPEN);

    /* Switching to always on opens and never closes again. */
    CHECK_EQ(lg_gps_plan_tick(&p, 100000u + LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_CLOSE);
    CHECK(lg_gps_plan_set(&p, LG_GPS_PLAN_ALWAYS, 100001u + LG_GPS_PLAN_WINDOW_MS));
    CHECK_EQ(lg_gps_plan_tick(&p, 100001u + LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_OPEN);
    CHECK_EQ(lg_gps_plan_tick(&p, 900000u), LG_GPS_ACT_NONE);
    CHECK(lg_gps_plan_reading(&p));

    /* `gps raw`, or a screen that wants a position now, brings the next reading forward. */
    CHECK(lg_gps_plan_set(&p, 1800, 900000u));
    CHECK_EQ(lg_gps_plan_tick(&p, 900000u + LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_CLOSE);
    CHECK(lg_gps_plan_next_in_s(&p, 900000u + LG_GPS_PLAN_WINDOW_MS) > 0);
    lg_gps_plan_wake(&p, 900000u + LG_GPS_PLAN_WINDOW_MS);
    CHECK_EQ(lg_gps_plan_tick(&p, 900000u + LG_GPS_PLAN_WINDOW_MS), LG_GPS_ACT_OPEN);
}

/* The millisecond counter wrapping must not park a device between readings for 49 days. */
static void test_wrap(void)
{
    lg_gps_plan_t p;
    uint32_t t = UINT32_MAX - 2000u;   /* the first reading straddles the wrap */
    lg_gps_plan_init(&p, 60, t);
    lg_gps_plan_on_fix(&p, t + 1000u);
    CHECK_EQ(p.last_ttf_ms, 1000);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 3000u), LG_GPS_ACT_CLOSE);   /* t + 3000 has wrapped */
    CHECK_EQ(lg_gps_plan_next_in_s(&p, t + 3000u), 57);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 59000u), LG_GPS_ACT_NONE);
    CHECK_EQ(lg_gps_plan_tick(&p, t + 60000u), LG_GPS_ACT_OPEN);
}

/* ---- the AP telling its handhelds, on the simulated grid (D73's GPS_PLAN body) ---- */

#define DAD     0
#define EMMA    1
#define ALEX    2
#define RANGER  3

static lg_client_t *cl(sim_t *s, int i)
{
    return &s->clients[i].client;
}

static uint16_t held_plan(sim_t *s, int i)
{
    uint16_t plan = 0xEEEEu;
    return lg_client_gps_plan(cl(s, i), &plan) ? plan : 0xEEEEu;
}

static void test_plan_reaches_handhelds(void)
{
    sim_t *s = sim_create();
    CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    /* DAD and RANGER on AP 0, ALEX on AP 1, as the zone test arranges them. */
    sim_attach(s, DAD, 0);
    sim_attach(s, RANGER, 0);
    sim_attach(s, ALEX, 1);
    CHECK(sim_pump(s));

    /* Until an AP says, a handheld keeps whatever it saved rather than falling back. */
    CHECK_EQ(held_plan(s, DAD), 0xEEEEu);

    /* AP 0 learns the grid's setting and tells the handhelds on it, once. */
    CHECK_EQ(lg_node_set_gps_plan(&s->nodes[0].node, 600), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(held_plan(s, DAD), 600);
    CHECK_EQ(held_plan(s, RANGER), 600);
    CHECK_EQ(s->clients[DAD].gps_plan_events, 1);
    CHECK_EQ(held_plan(s, ALEX), 0xEEEEu);   /* AP 1 has not been told: nothing sent */

    /* The same setting again costs a handheld nothing. */
    CHECK_EQ(lg_node_set_gps_plan(&s->nodes[0].node, 600), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(s->clients[DAD].gps_plan_events, 1);

    /* Registration brings it, so a handheld that roams to an AP learns the grid's choice (D48). */
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 0);
    CHECK(sim_pump(s));
    CHECK_EQ(held_plan(s, EMMA), 600);
    CHECK_EQ(s->clients[EMMA].gps_plan_events, 1);

    /* An AP that has not heard the settings clears nothing a handheld already holds. */
    sim_detach(s, EMMA);
    sim_attach(s, EMMA, 2);
    CHECK(sim_pump(s));
    CHECK_EQ(held_plan(s, EMMA), 600);
    CHECK_EQ(s->clients[EMMA].gps_plan_events, 1);

    /* A change reaches everyone attached; always on travels as its own value. */
    CHECK_EQ(lg_node_set_gps_plan(&s->nodes[0].node, LG_GPS_PLAN_ALWAYS), LG_OK);
    CHECK(sim_pump(s));
    CHECK_EQ(held_plan(s, DAD), LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(held_plan(s, RANGER), LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(s->clients[DAD].gps_plan_events, 2);

    /* The client takes only a well-formed SYSTEM body of exactly two bytes. */
    uint8_t frame[LG_ENV_SIZE + 8u];
    uint8_t body[4] = { 0 };
    lg_env_t e = {
        .major = LG_PROTO_MAJOR, .minor = LG_PROTO_MINOR, .type = LG_T_GPS_PLAN, .scope = LG_SCOPE_DIRECT,
        .ttl = 1, .origin_id = LG_NODE_ID_BASE | 0u, .origin_boot = 1, .origin_seq = 900, .target = LG_PROTO_DAD,
    };
    lg_wr16(body, 300);
    int flen = lg_frame_build(&e, body, LG_GPS_PLAN_BODY_LEN, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);   /* not SYSTEM */
    CHECK_EQ(held_plan(s, DAD), LG_GPS_PLAN_ALWAYS);
    e.scope = LG_SCOPE_SYSTEM;
    e.origin_seq++;
    flen = lg_frame_build(&e, body, 1u, frame, sizeof(frame));   /* one byte */
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);
    CHECK_EQ(held_plan(s, DAD), LG_GPS_PLAN_ALWAYS);
    e.origin_seq++;
    flen = lg_frame_build(&e, body, 4u, frame, sizeof(frame));   /* four bytes */
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);
    CHECK_EQ(held_plan(s, DAD), LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(s->clients[DAD].gps_plan_events, 2);
    e.origin_seq++;
    flen = lg_frame_build(&e, body, LG_GPS_PLAN_BODY_LEN, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);
    CHECK_EQ(held_plan(s, DAD), 300);
    CHECK_EQ(s->clients[DAD].gps_plan_events, 3);
    lg_client_on_frame(cl(s, DAD), frame, (size_t)flen);   /* delivered twice: one change */
    CHECK_EQ(s->clients[DAD].gps_plan_events, 3);

    /* An AP ignores a plan sent up by a handheld: this is admin policy, not a handheld's to set. */
    uint32_t rejected = s->nodes[0].node.stats.rejected;
    e.origin_id = LG_PROTO_DAD;
    e.target = 0;
    e.origin_seq++;
    flen = lg_frame_build(&e, body, LG_GPS_PLAN_BODY_LEN, frame, sizeof(frame));
    CHECK(flen > 0);
    lg_node_on_session_frame(&s->nodes[0].node, &s->clients[DAD].session_device, frame, (size_t)flen);
    CHECK_EQ(s->nodes[0].node.gps_plan, LG_GPS_PLAN_ALWAYS);
    CHECK_EQ(s->nodes[0].node.stats.rejected, rejected);
    sim_destroy(s);
}

void test_gps_plan(void)
{
    test_stored_form();
    test_always_on();
    test_schedule();
    test_short_interval();
    test_interval_change();
    test_wrap();
    test_plan_reaches_handhelds();
}
