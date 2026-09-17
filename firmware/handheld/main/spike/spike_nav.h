/*
 * spike_nav.h - moving between the no-LVGL screens. A screen asks to go somewhere with
 * spike_go(); the spike task switches after the current touch or refresh has finished, so no
 * screen is torn down while its own code is still running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hh_service.h"

typedef enum {
    NAV_HOME,
    NAV_STATUS,
    NAV_CONVERSATIONS,
    NAV_CHAT,          /* scope, target, title */
    NAV_GROUPS,
    NAV_GROUP_EDIT,    /* target: group id, 0 for a new group */
    NAV_SETTINGS,      /* target: the tab */
    NAV_WHICH_AP,
    NAV_RENAME,
    NAV_CALIBRATE,
} spike_nav_t;

void spike_go(spike_nav_t to, uint8_t scope, uint32_t target, const char *title);

/* Screen entry points, each drawing its whole screen. w and h are the panel's size. */
void spike_convs_open(uint16_t w, uint16_t h);
void spike_convs_touch(int16_t x, int16_t y, bool down);
void spike_convs_refresh(void);

void spike_groups_open(uint16_t w, uint16_t h);
void spike_groups_touch(int16_t x, int16_t y, bool down);
void spike_groups_refresh(void);

void spike_group_edit_open(uint16_t w, uint16_t h, uint16_t id);
void spike_group_edit_touch(int16_t x, int16_t y, bool down);
void spike_group_edit_refresh(uint32_t now_ms);

void spike_settings_open(uint16_t w, uint16_t h, uint8_t tab);
void spike_settings_touch(int16_t x, int16_t y, bool down);
void spike_settings_refresh(void);

void spike_which_ap_open(uint16_t w, uint16_t h);
void spike_which_ap_touch(int16_t x, int16_t y, bool down);
void spike_which_ap_refresh(void);

void spike_rename_open(uint16_t w, uint16_t h);
void spike_rename_touch(int16_t x, int16_t y, bool down);

/* Blocks the spike task for up to 30 s a point while the three targets are pressed. */
void spike_calibrate_run(uint16_t w, uint16_t h);

/* Redraws whatever screen is current, after the banner, an alert, or the saver covered it. */
void spike_redraw_current(void);

/* True when that conversation's chat is the screen showing. */
bool spike_chat_showing(uint8_t scope, uint32_t target);

void spike_group_edit_redraw(void);
void spike_rename_redraw(void);

/* The service's status, refreshed on each call; spike task only. */
const hh_status_t *spike_status(void);
