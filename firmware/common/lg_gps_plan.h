/*
 * lg_gps_plan.h - when a GPS is read, and when its UART is given back (D73).
 *
 * Wi-Fi, BLE and LoRa are first-class for RAM; the GPS gives way. A module that is only ever
 * asked where a grid is, and a grid that does not move, does not need a resident reader: the admin
 * picks an interval, and between readings the UART driver and its buffers are released. "Always
 * on" is still a choice, and on that setting nothing below ever closes anything, so an owner who
 * wants continuous GPS loses nothing.
 *
 * This file is portable C11 with no ESP-IDF in it, shared by the AP (firmware/node/main/gps.c) and
 * the handheld (firmware/handheld/main/service/hh_gps.c) and tested on a board without one fitted
 * (tests/target/main/test_gps_plan.c). It decides only; opening and closing is the caller's.
 *
 * Grid time keeps its own discipline either way (D63, D65): a reading corrects a clock, it never
 * replaces the grid's agreement about it, and nothing here touches time.
 *
 * Stored form (D49, D48): one little-endian u16 in the grid settings, replicated between APs and
 * pushed to handhelds, so the words ("every 5 minutes") are the admin page's job, not a device's.
 *     0        the default, LG_GPS_PLAN_DEFAULT_S: a settings record from before D73 reads this way
 *     0xFFFF   always on, exactly as before D73
 *     other    seconds between the starts of two readings, LG_GPS_PLAN_MIN_S..LG_GPS_PLAN_MAX_S
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LG_GPS_PLAN_LEN         2u        /* the stored and sent form: one u16, little-endian */
#define LG_GPS_PLAN_DEFAULT     0u        /* stored: "whatever the firmware thinks sensible" */
#define LG_GPS_PLAN_ALWAYS      0xFFFFu   /* stored: never released */
#define LG_GPS_PLAN_DEFAULT_S   300u      /* a grid that stays put: five minutes */
#define LG_GPS_PLAN_MIN_S       30u
#define LG_GPS_PLAN_MAX_S       3600u

/* How long one reading may stay open waiting for a fix, and how long it stays open after the first
 * fix so GGA and GSV catch up with RMC. A powered module keeps its almanac, so a warm fix is
 * seconds; the window is for the cold case and is bounded either way. */
#define LG_GPS_PLAN_WINDOW_MS   120000u
#define LG_GPS_PLAN_SETTLE_MS   2000u

/* Seconds between readings, clamped; 0 means always on. A stored value out of range is clamped
 * rather than refused, so a newer AP's choice never stops an older one from reading at all. */
uint16_t lg_gps_plan_seconds(uint16_t stored);

/* True for a stored value a device would keep unchanged: the default, always on, or in range. */
bool lg_gps_plan_valid(uint16_t stored);

/* The stored value a device holds after being told `stored`: out-of-range becomes the clamped
 * seconds, so every AP and handheld in the grid ends up holding the same number. */
uint16_t lg_gps_plan_canon(uint16_t stored);

typedef enum {
    LG_GPS_PHASE_READING = 0,   /* the module is powered and being read */
    LG_GPS_PHASE_IDLE    = 1,   /* released: no UART driver, no buffers, waiting for the next reading */
} lg_gps_phase_t;

typedef enum {
    LG_GPS_ACT_NONE  = 0,
    LG_GPS_ACT_OPEN  = 1,   /* install the UART driver and start reading */
    LG_GPS_ACT_CLOSE = 2,   /* this reading is done: delete the driver and free its buffers */
} lg_gps_act_t;

typedef struct {
    uint16_t stored;        /* as held in the settings */
    uint32_t window_ms;     /* longest one reading waits for a fix */
    uint32_t settle_ms;     /* stays open this long after a reading's first fix */
    uint8_t  phase;         /* lg_gps_phase_t */
    uint32_t started_ms;    /* when the current (or last) reading began */
    uint32_t next_ms;       /* when the next reading is due, while idle */
    bool     fix;           /* a fix arrived during the current reading */
    uint32_t fix_ms;        /* when it did */
    uint32_t readings;      /* readings begun since boot */
    uint32_t fixes;         /* of those, ones that reached a fix */
    uint32_t last_ttf_ms;   /* time to a fix in the last reading that got one; UINT32_MAX never */
    uint32_t last_open_ms;  /* how long the last finished reading stayed open; 0 never */
} lg_gps_plan_t;

/* Starts in a reading, now: a device wants a fix as soon as it boots whatever the interval is. */
void lg_gps_plan_init(lg_gps_plan_t *p, uint16_t stored, uint32_t now_ms);

/* A new interval from the grid. Returns true when it differs from the one held. Switching to
 * always on opens at the next tick; switching away lets the reading in progress finish. */
bool lg_gps_plan_set(lg_gps_plan_t *p, uint16_t stored, uint32_t now_ms);

/* A fixed sentence arrived. Only the first one of a reading moves the plan. */
void lg_gps_plan_on_fix(lg_gps_plan_t *p, uint32_t now_ms);

/* Bring the next reading forward to now (the console's `gps raw`, or a screen asking to locate). */
void lg_gps_plan_wake(lg_gps_plan_t *p, uint32_t now_ms);

/* Call about every 100-200 ms. Moves the plan on and says what the caller must do to the UART. */
lg_gps_act_t lg_gps_plan_tick(lg_gps_plan_t *p, uint32_t now_ms);

/* True while the module should be open. */
bool lg_gps_plan_reading(const lg_gps_plan_t *p);

/* True when the plan never closes anything. */
bool lg_gps_plan_always(const lg_gps_plan_t *p);

/* Seconds until the next reading; 0 while one is running or when always on. */
uint32_t lg_gps_plan_next_in_s(const lg_gps_plan_t *p, uint32_t now_ms);

/*
 * How long a fix taken in a reading stays the answer, in milliseconds: the interval, plus a
 * reading's window, plus a little. Grid time and a position both go on using the last reading
 * while this holds, which is what makes a sampled GPS behave as a resident one between
 * corrections (D63) and lets a position simply age (D65). UINT32_MAX when always on, where the
 * caller's own freshness rule (5 s) is the only one that applies.
 */
uint32_t lg_gps_plan_hold_ms(const lg_gps_plan_t *p);
