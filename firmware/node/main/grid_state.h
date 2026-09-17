/*
 * grid_state.h - the admin settings and the grid time generation, held by every AP (D45).
 *
 * There is no master. Every AP keeps a full copy of the admin settings in NVS and floods
 * them over the backbone (LG_T_GRID_STATE) when a link comes up and every GRID_STATE_ANNOUNCE_MS.
 * A copy replaces another only when its (seq, author) pair is higher, so the newest change
 * wins everywhere, including after a split grid rejoins; author is the AP index that made it.
 *
 * Grid time works the same way without being stored: whoever the admin last set time on holds
 * AUTHORITATIVE, and a higher (time_gen, time_author) heard from another AP demotes this one to
 * CARRIED so it adopts the newer time instead of defending the old one.
 *
 * Threading: frames, announcements, and time generation run on the core task. Settings are
 * also read and committed by the web server task, so they sit behind a mutex; a commit posts
 * NODE_CMD_GRID_ANNOUNCE so the flood itself happens on the core task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

#define GRID_STATE_ANNOUNCE_MS  30000u

void grid_state_init(uint16_t self);

/* Copies the current settings. Any task. */
void grid_state_settings(node_settings_t *out);

/* Stores new settings as the newest version, authored by this AP, and asks the core task to
 * announce them. Any task. */
esp_err_t grid_state_commit(const node_settings_t *in);

/* True once this AP may accept first-time setup: it has no working link (it is alone), or it
 * has heard another AP's grid state since boot, so it knows whether the grid is set up already. */
bool grid_state_setup_allowed(void);

/* The admin set time on this AP: start a new time generation. Core task. */
void grid_state_time_set_here(void);

/* A GRID_STATE body from another AP. Core task. */
void grid_state_on_frame(uint16_t origin_node, const uint8_t *body, size_t len);

/* Floods this AP's grid state. Core task. */
void grid_state_announce(void);

/* One status line for the console. Core task. */
void grid_state_print(void);
