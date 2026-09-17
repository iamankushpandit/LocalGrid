/*
 * ui_bar.h - the top bar every screen but the launcher carries.
 *
 * Home on the left, the screen's title, and a status cluster on the right, so the owner
 * always knows where they are, how the handheld is connected, and what time the grid says.
 * Structure follows Braino's shared top bar; the code is LocalGrid's own.
 */
#pragma once

#include "hh_service.h"
#include "lvgl.h"

/* Adds the bar to a screen. on_home is called when Home is tapped. */
lv_obj_t *ui_bar_create(lv_obj_t *screen, const char *title, lv_event_cb_t on_home);

/* Refreshes the right-hand cluster: link state, signal, and grid time. */
void ui_bar_update(lv_obj_t *bar, const hh_status_t *status);
