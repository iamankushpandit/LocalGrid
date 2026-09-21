#include "lg_gps_plan.h"

/* Wrap-safe: true once `now` has reached `due`, whatever the 32-bit millisecond counter has done. */
static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static uint32_t since(uint32_t now, uint32_t then)
{
    return now - then;
}

uint16_t lg_gps_plan_seconds(uint16_t stored)
{
    if (stored == LG_GPS_PLAN_ALWAYS) {
        return 0u;
    }
    if (stored == LG_GPS_PLAN_DEFAULT) {
        return LG_GPS_PLAN_DEFAULT_S;
    }
    if (stored < LG_GPS_PLAN_MIN_S) {
        return LG_GPS_PLAN_MIN_S;
    }
    if (stored > LG_GPS_PLAN_MAX_S) {
        return LG_GPS_PLAN_MAX_S;
    }
    return stored;
}

bool lg_gps_plan_valid(uint16_t stored)
{
    return stored == LG_GPS_PLAN_DEFAULT || stored == LG_GPS_PLAN_ALWAYS ||
           (stored >= LG_GPS_PLAN_MIN_S && stored <= LG_GPS_PLAN_MAX_S);
}

uint16_t lg_gps_plan_canon(uint16_t stored)
{
    return lg_gps_plan_valid(stored) ? stored : lg_gps_plan_seconds(stored);
}

static void begin_reading(lg_gps_plan_t *p, uint32_t now_ms)
{
    p->phase = LG_GPS_PHASE_READING;
    p->started_ms = now_ms;
    p->fix = false;
    p->fix_ms = 0;
    p->readings++;
}

void lg_gps_plan_init(lg_gps_plan_t *p, uint16_t stored, uint32_t now_ms)
{
    p->stored = lg_gps_plan_canon(stored);
    p->window_ms = LG_GPS_PLAN_WINDOW_MS;
    p->settle_ms = LG_GPS_PLAN_SETTLE_MS;
    p->readings = 0;
    p->fixes = 0;
    p->last_ttf_ms = UINT32_MAX;
    p->last_open_ms = 0;
    p->next_ms = now_ms;
    begin_reading(p, now_ms);
}

bool lg_gps_plan_set(lg_gps_plan_t *p, uint16_t stored, uint32_t now_ms)
{
    uint16_t next = lg_gps_plan_canon(stored);
    if (next == p->stored) {
        return false;
    }
    p->stored = next;
    if (p->phase == LG_GPS_PHASE_IDLE) {
        /* Re-time the wait against the reading it follows, so a shorter interval takes effect at
         * once and a longer one does not start a reading it has already outlived. */
        uint32_t ms = (uint32_t)lg_gps_plan_seconds(next) * 1000u;
        p->next_ms = p->started_ms + ms;
        if (lg_gps_plan_always(p) || due(now_ms, p->next_ms)) {
            p->next_ms = now_ms;
        }
    }
    return true;
}

void lg_gps_plan_on_fix(lg_gps_plan_t *p, uint32_t now_ms)
{
    if (p->phase != LG_GPS_PHASE_READING || p->fix) {
        return;
    }
    p->fix = true;
    p->fix_ms = now_ms;
    p->fixes++;
    p->last_ttf_ms = since(now_ms, p->started_ms);
}

void lg_gps_plan_wake(lg_gps_plan_t *p, uint32_t now_ms)
{
    if (p->phase == LG_GPS_PHASE_IDLE) {
        p->next_ms = now_ms;
    }
}

lg_gps_act_t lg_gps_plan_tick(lg_gps_plan_t *p, uint32_t now_ms)
{
    if (lg_gps_plan_always(p)) {
        if (p->phase != LG_GPS_PHASE_READING) {
            begin_reading(p, now_ms);
            return LG_GPS_ACT_OPEN;
        }
        return LG_GPS_ACT_NONE;   /* nothing is ever closed: exactly the behaviour before D73 */
    }
    if (p->phase == LG_GPS_PHASE_READING) {
        bool settled = p->fix && since(now_ms, p->fix_ms) >= p->settle_ms;
        if (!settled && since(now_ms, p->started_ms) < p->window_ms) {
            return LG_GPS_ACT_NONE;
        }
        p->last_open_ms = since(now_ms, p->started_ms);
        p->phase = LG_GPS_PHASE_IDLE;
        p->next_ms = p->started_ms + (uint32_t)lg_gps_plan_seconds(p->stored) * 1000u;
        if (due(now_ms, p->next_ms)) {
            p->next_ms = now_ms;   /* the reading outran its own interval: the next one follows it */
        }
        return LG_GPS_ACT_CLOSE;
    }
    if (due(now_ms, p->next_ms)) {
        begin_reading(p, now_ms);
        return LG_GPS_ACT_OPEN;
    }
    return LG_GPS_ACT_NONE;
}

bool lg_gps_plan_reading(const lg_gps_plan_t *p)
{
    return p->phase == LG_GPS_PHASE_READING;
}

bool lg_gps_plan_always(const lg_gps_plan_t *p)
{
    return p->stored == LG_GPS_PLAN_ALWAYS;
}

uint32_t lg_gps_plan_next_in_s(const lg_gps_plan_t *p, uint32_t now_ms)
{
    if (p->phase == LG_GPS_PHASE_READING || lg_gps_plan_always(p)) {
        return 0u;
    }
    if (due(now_ms, p->next_ms)) {
        return 0u;
    }
    return (p->next_ms - now_ms + 999u) / 1000u;
}

uint32_t lg_gps_plan_hold_ms(const lg_gps_plan_t *p)
{
    if (lg_gps_plan_always(p)) {
        return UINT32_MAX;
    }
    return (uint32_t)lg_gps_plan_seconds(p->stored) * 1000u + p->window_ms + 5000u;
}
