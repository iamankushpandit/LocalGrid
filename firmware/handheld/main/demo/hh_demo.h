/*
 * hh_demo.h - a self-running demonstration of what this grid can do.
 *
 * Start it and stand back: the handheld works through a script of ordinary use - messages to a
 * person and to a group, a voice talk, an announcement, an urgent call and its all clear - and
 * shows on its own screen what it is doing and what came back (D23). One person starting it is
 * the only interaction it needs.
 *
 * Nothing here is written for the boards on one bench. The script is built at the moment it runs,
 * from who the grid says is present and what their devices report they can do (LG_CAP_* bits,
 * shared in presence since design review answer 19 was finally implemented). A board added next
 * year takes part with no change here, and a demo run with two handhelds in a room is the same
 * code as one run with ten in a field.
 *
 * Two runs, because a demo in front of people and a demo of resilience are different things:
 *
 *   HH_DEMO_SHOWCASE    nothing is taken down and nothing is broken. Safe to start at any moment,
 *                       including in front of an audience or while the grid is in real use.
 *   HH_DEMO_RESILIENCE  the showcase, and then it asks the grid to show it recovering: this
 *                       handheld leaves its AP and rejoins, so failover and the messages that
 *                       survive it are visible. It degrades only itself, never an AP, and never
 *                       anybody else's handheld.
 *
 * Layering (D27): this module decides what to do and asks hh_service and hh_voice to do it. It
 * draws nothing; it publishes its progress and the screens read it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hh_service.h"

typedef enum {
    HH_DEMO_SHOWCASE = 0,
    HH_DEMO_RESILIENCE = 1,
} hh_demo_mode_t;

/* What the run is doing now. The order is the order they happen in. */
typedef enum {
    HH_DEMO_IDLE = 0,
    HH_DEMO_LOOKING,        /* reading who is present and what they can do */
    HH_DEMO_DIRECT,         /* a 1:1 message to someone, and waiting for it to be delivered */
    HH_DEMO_GROUP,          /* a message to a group */
    HH_DEMO_VOICE,          /* a talk, generated in firmware so no one need speak */
    HH_DEMO_ANNOUNCE,       /* a broadcast everyone sees */
    HH_DEMO_URGENT,         /* an urgent broadcast, then its all clear */
    HH_DEMO_POSITION,       /* showing where the devices with a GPS are */
    HH_DEMO_ROAM,           /* resilience only: leave the AP and come back */
    HH_DEMO_DONE,
} hh_demo_step_t;

#define HH_DEMO_NOTE_MAX 64

typedef struct {
    uint32_t       version;      /* changes whenever anything below changes */
    bool           running;
    hh_demo_mode_t mode;
    hh_demo_step_t step;
    uint8_t        step_n;       /* 1-based, for "step 3 of 7" */
    uint8_t        steps;        /* how many this run has, which depends on what the grid can do */
    uint8_t        done;         /* steps finished */
    uint8_t        skipped;      /* steps the grid could not show, with a reason in note */
    char           note[HH_DEMO_NOTE_MAX];   /* a sentence about what is happening or why it was skipped */
} hh_demo_state_t;

/* Starts the demo task. Call once, after hh_voice_start. */
esp_err_t hh_demo_start(void);

/*
 * Begins a run. ESP_ERR_INVALID_STATE if one is already running, ESP_ERR_NOT_FOUND when the grid
 * has nobody else in it: a demonstration of a network needs somebody on the other end, and saying
 * so is more use than a run that quietly does nothing.
 */
esp_err_t hh_demo_run(hh_demo_mode_t mode);

/* Stops a run at the next step boundary. Safe to call when nothing is running. */
void hh_demo_stop(void);

void hh_demo_state(hh_demo_state_t *out);
