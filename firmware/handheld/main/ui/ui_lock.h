/*
 * ui_lock.h - the top bar's battery badge and padlock, the lock screen, and the brand mark they
 * share with the launcher (D62). UI task only.
 *
 * The bar: every screen but the launcher has a house at the left of its header that goes home in
 * one tap, and every screen has a small cluster at the right of its header row, left of any back
 * icon of its own: the battery badge (the charge as digits in a battery shell, with a thin gauge
 * under them) and the padlock. Tapping the padlock locks. Left of the cluster a gap is kept free
 * for a later indicator (someone is talking); titles end before it.
 *
 * The lock: an accidental-touch guard, not security. The lock screen owns the panel and every
 * touch; the only way out is to hold the button for 0.9 s. Alerts still take the screen over it
 * (ui_overlay.c), and after 12 s untouched it hands the panel to the screen saver; waking that
 * comes back here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lg_draw.h"

/* ---- the brand mark, 26 px square ---- */

#define UI_LOGO_SIZE 26

void ui_paint_logo(const lg_canvas_t *c, int16_t x, int16_t y);

/* ---- the top bar's cluster ---- */

/* Puts the cluster in the row y..y+h with its right edge at `right`. Returns the x a title must
 * end at: the cluster's left, less the gap kept for a later indicator. */
int16_t ui_bar_place(int16_t right, int16_t y, int16_t h);

/* Puts a house at the left of the header row y..y+h; tapping it goes to the launcher. Call after
 * ui_bar_place (which clears it). Returns the x a title must start at. */
int16_t ui_bar_home(int16_t y, int16_t h);

/* The free slot left of the badge, kept for a small indicator (someone is talking): 22 px wide,
 * the header row's height, on the current screen. The bar never paints it, but a screen's own
 * header paint clears it, so an indicator there must be repainted after the header is. w is 0
 * before any screen placed the bar. */
lg_rect_t ui_bar_spare_rect(void);

/* Paints the cluster inside a caller's region painter (a header painted in one pass). */
void ui_bar_paint(const lg_canvas_t *c);

/* Draws the cluster on its own, over whatever the screen painted there. */
void ui_bar_draw(void);

/* Repaints the badge only if the charge shown has changed. True when it drew. */
bool ui_bar_refresh(void);

/*
 * One touch sample for the padlock, before the screen sees it. True when the sample belongs to a
 * press that started on the padlock (the screen must not see it). A release on the padlock locks
 * and draws the lock screen.
 */
bool ui_bar_touch(int16_t x, int16_t y, bool down);

/* ---- the lock ---- */

void ui_lock_start(uint16_t w, uint16_t h);

bool ui_lock_active(void);

/* Locks and paints the lock screen. */
void ui_lock_engage(void);

/* Paints the whole lock screen again (after an alert or the saver covered it). */
void ui_lock_draw(void);

/* Call every loop while locked and nothing covers the panel, with the touch as it is now (down
 * false when the overlay took the sample). Runs the hold, its progress bar, the battery, and the
 * hand-over to the saver. True when the hold completed and the handheld is unlocked: the caller
 * then redraws the screen underneath. */
bool ui_lock_tick(uint32_t now_ms, bool down, int16_t x, int16_t y);
