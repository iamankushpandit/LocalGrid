/*
 * grid_state.h - what every AP knows about the grid and keeps sharing (D45, D48).
 *
 * There is no master. Every AP keeps a full copy of the admin settings in NVS and floods them
 * over the backbone (LG_T_GRID_STATE) when a link comes up and every GRID_STATE_ANNOUNCE_MS. A
 * copy replaces another only when its (seq, author) pair is higher, so the newest change wins
 * everywhere, including after a split grid rejoins.
 *
 * The same broadcast carries each AP's own health: uptime, time quality and stratum, its last
 * time sync, its boot counter, and why its current run started. From those every AP keeps a
 * table of the others, notices when one becomes unreachable, and records why (an incident).
 *
 * Sticky records (D48) are kept as packed bytes, never text, and survive restarts: in RTC memory
 * for a crash, watchdog, software restart, or brownout, and in NVS for a power-on reset. They are
 *   - where and when grid time was last set (generation, author, grid time),
 *   - the incident log (GRID_INCIDENTS entries of 16 bytes),
 *   - availability: two bits a minute for GRID_AVAIL_MINUTES minutes per AP.
 * The log and the availability history are also flooded to neighbours, which merge what they
 * lack, so an AP that restarted recovers what happened while it was away.
 *
 * Threading: frames, announcements, and the once-a-second bookkeeping run on the core task.
 * Settings are also read and committed by the web task, so they sit behind a mutex.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_types.h"
#include "settings.h"

/*
 * The healing repeat, for a device that missed a change (D48). A change is announced at once and
 * on link up, so this only sets how quickly a missed one heals. Measured on the bench 2026-09-20
 * with nobody using the grid: three APs at 30 s produced about 35 arrivals a minute each, half of
 * them duplicates of the other APs' relays, and it was the largest single source of traffic and of
 * MAIN's full send queue. Two minutes costs a device that missed something up to 90 s more to
 * heal, and nothing else (owner agreed).
 */
#define GRID_STATE_ANNOUNCE_MS  120000u
#define GRID_AP_NAME_MAX        15u
#define GRID_AVAIL_MINUTES      120u                              /* two hours */
#define GRID_AVAIL_BYTES        ((GRID_AVAIL_MINUTES + 3u) / 4u)  /* two bits a minute, oldest first */
#define GRID_INCIDENTS          16u
#define GRID_NO_AP              0xFFFFu
#define GRID_NEVER              0xFFFFFFFFu

enum {
    GRID_AVAIL_UNKNOWN = 0,   /* no record: before this AP started, or an AP nobody saw */
    GRID_AVAIL_UP      = 1,   /* reachable every second of that minute */
    GRID_AVAIL_PARTIAL = 2,   /* reachable some of it */
    GRID_AVAIL_DOWN    = 3,   /* not reachable at all */
};

/* Minute m (0 oldest) of a packed availability history. */
static inline uint8_t grid_avail_get(const uint8_t *packed, size_t m)
{
    return (uint8_t)((packed[m / 4u] >> ((m % 4u) * 2u)) & 3u);
}

/* What one AP last said about itself, aged to now. A working view, not stored. */
typedef struct {
    bool     valid;
    bool     self;
    uint16_t ap;
    char     name[GRID_AP_NAME_MAX + 1];
    uint32_t uptime_s;
    uint8_t  time_quality;    /* lg_time_quality_t */
    uint8_t  stratum;         /* distance from where the time was set; 255 unknown */
    uint16_t sync_from;       /* GRID_NO_AP never */
    uint32_t sync_age_s;      /* GRID_NEVER never */
    int16_t  sync_drift_ms;   /* clock correction at that sync */
    uint32_t heard_age_s;     /* since its last grid state arrived; 0 for this AP */
    uint32_t boot;
    uint8_t  reset_reason;    /* why its current run started (esp_reset_reason_t) */
    uint32_t prev_run_s;      /* how long its run before that lasted, UINT32_MAX unknown */
} grid_ap_info_t;

typedef enum {
    GRID_INCIDENT_DOWN = 0,       /* unreachable now, or back and waiting for its reason */
    GRID_INCIDENT_RESTARTED,      /* came back with a new boot: the reset reason says why */
    GRID_INCIDENT_LINK,           /* came back without restarting: the link dropped */
    GRID_INCIDENT_UNEXPLAINED,    /* came back but never said why */
} grid_incident_kind_t;

/*
 * One outage of an AP, as some AP saw it. Exactly 16 bytes, stored and sent as is.
 * kind_reset: kind in bits 0-1, esp_reset_reason_t in bits 2-7.
 */
typedef struct {
    uint32_t down_grid_time;  /* Unix seconds when it went unreachable; 0 if grid time was unset */
    uint16_t duration_s;      /* unreachable for, saturating at 65535; so far while still down */
    uint16_t prev_run_min;    /* for RESTARTED: how long it had run, minutes; 0xFFFF unknown */
    uint16_t boot;            /* low 16 bits of its boot counter when this was recorded */
    uint8_t  ap;
    uint8_t  kind_reset;
    uint8_t  seen_by;         /* the AP that watched it; others learn it from that AP */
    uint8_t  reserved[3];
} grid_incident_t;

_Static_assert(sizeof(grid_incident_t) == 16, "grid_incident_t is a stored and transmitted record");

static inline uint8_t grid_incident_kind(const grid_incident_t *in)
{
    return (uint8_t)(in->kind_reset & 3u);
}

static inline uint8_t grid_incident_reset(const grid_incident_t *in)
{
    return (uint8_t)(in->kind_reset >> 2);
}

void grid_state_init(uint16_t self);

/* Copies the current settings. Any task. */
void grid_state_settings(node_settings_t *out);

/* Copies only the grid's POSIX time zone (D67), "" when none. Any task. */
void grid_state_posix_tz(char *out, size_t cap);

/* The grid's GPS reading plan (D73) in its stored form, as lg_gps_plan.h describes it. Any task. */
uint16_t grid_state_gps_plan(void);

/* Stores new settings as the newest version, authored by this AP, and asks the core task to
 * announce them. Any task. */
esp_err_t grid_state_commit(const node_settings_t *in);

/* True once this AP may accept first-time setup: it has no working link, or it has heard another
 * AP's grid state since boot, so it knows whether the grid is set up already. */
bool grid_state_setup_allowed(void);

/* The admin set time on this AP: start a new time generation. Core task. */
/* Time set on this AP: by an admin (by_gps false) or by its GPS (D63). */
void grid_state_time_set_here(uint32_t unix_s, bool by_gps);

/* This AP took or confirmed grid time from another AP; correction_ms is how far off it was. */
void grid_state_note_sync(uint16_t from_ap, int32_t correction_ms);

/* The AP this AP last took or confirmed grid time from (itself when set here), GRID_NO_AP never. */
uint16_t grid_state_sync_source(void);

/* One AP's announced state (this AP's own when ap is self). False when never heard. */
bool grid_state_ap(uint16_t ap, grid_ap_info_t *out);

/* When and where grid time was last set by an admin. set_on_ap is GRID_NO_AP when unknown. */
/* by_gps: the time was set by that AP's GPS (D63), not by an admin. */
void grid_state_time_info(uint32_t *set_unix, uint16_t *set_on_ap, uint32_t *generation, bool *by_gps);

/* Core task, once a second: which APs are reachable now (this AP counts as up). */
void grid_state_avail_second(const bool up[LG_MAX_NODES]);

/* Floods this AP's availability history and incident log so others fill their gaps. Core task. */
void grid_state_avail_announce(void);

/* Copies one AP's packed availability history (GRID_AVAIL_BYTES). */
void grid_state_avail(uint16_t ap, uint8_t out[GRID_AVAIL_BYTES]);

/* Grid-time minute of the newest availability entry, 0 while not anchored to grid time. */
uint32_t grid_state_avail_newest_minute(void);

/* Copies up to max incidents, newest first. */
size_t grid_state_incidents(grid_incident_t *out, size_t max);

/* Text for an esp_reset_reason_t value; used only when printing. */
const char *grid_state_reset_text(uint8_t reason);

/* A GRID_STATE body from another AP. Core task. */
void grid_state_on_frame(uint16_t origin_node, const uint8_t *body, size_t len);

/* Floods this AP's grid state. Core task. */
void grid_state_announce(void);

/* Status lines for the console. */
void grid_state_print(void);
