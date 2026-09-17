/*
 * ui_settings.h - everything the owner can change on the handheld, in one place.
 *
 * Until now the node choice lived on the home screen and calibration, the self test and
 * restart were only reachable over the serial console.
 */
#pragma once

#include "lg_identity.h"

/* Builds the settings screens. Call once, from the launcher. */
void ui_settings_build(const lg_identity_t *identity);

void ui_settings_open(void);

/* Runs the self test on the application task, because it must not block drawing. */
void ui_settings_run_selftest(void);

/* Work the drawing task must not do itself: calibration waits for presses, and the self
 * test runs for tens of milliseconds. The application task takes these and performs them. */
typedef enum {
    UI_JOB_NONE = 0,
    UI_JOB_CALIBRATE,
    UI_JOB_SELFTEST,
} ui_job_t;

ui_job_t ui_settings_take_job(void);
