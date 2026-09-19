/*
 * ui_unit.h - the alert and distress unit's screens (D66). UI task only.
 *
 * A board whose profile says LG_ROLE_ALERT_UNIT runs these instead of the launcher: an idle screen
 * that is a watch face (the LocalGrid mark, name, battery, local time and date with a GPS mark, D67;
 * the AP), the SOS countdown, and the SOS screen with "Seen by" and
 * "I'm safe". Alerts from others still take the panel over (ui_overlay.c). Every action is
 * reachable from the board's buttons as well as from touch, as the profile maps them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lg_board.h"

/* Draws the idle screen. touch: the board has a working touch panel. */
void ui_unit_start(const lg_board_t *board, uint16_t w, uint16_t h, bool touch);

/* Paints the current unit screen again (after an alert covered it). */
void ui_unit_redraw(void);

/* Every loop: runs the SOS (sending, repeats, retries, who has read it), and refreshes the screen
 * unless covered (an alert owns the panel). */
void ui_unit_tick(uint32_t now_ms, bool covered);

/* A touch sample, as the UI task polls it. */
void ui_unit_touch(int16_t x, int16_t y, bool down, uint32_t now_ms);

/* A button from the profile: a short press (hold false) or a hold. The action comes from the
 * profile's bits and what is on screen; an alert on screen is read by a press. */
void ui_unit_button(uint8_t id, bool hold, uint32_t now_ms);

/* A button went down or up: the hold's progress bar follows it. */
void ui_unit_button_down(uint8_t id, bool down, uint32_t now_ms);

/* One console line on the SOS state (ui log). */
void ui_unit_log(void);
