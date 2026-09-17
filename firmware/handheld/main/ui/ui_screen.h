/*
 * ui_screen.h - screens that exist only while they are shown.
 *
 * A screen built at boot and kept forever holds every one of its widgets in RAM whether
 * anybody looks at it or not: Status and Settings cost about 7 KB each on the Hosyond, and a
 * chat with its keyboard 12 KB more. Screens are built when opened and freed when another
 * screen replaces them, so only the one on the panel (and the always-kept launcher) costs RAM.
 *
 * ui_screen_free_on_leave() arranges that: when the screen is unloaded, `forget` runs first so
 * the module drops every pointer into the screen (timers must check for NULL afterwards),
 * then the screen is deleted on the next LVGL pass, never inside its own event.
 * Drawing task, or with the display lock held.
 */
#pragma once

#include "lvgl.h"

void ui_screen_free_on_leave(lv_obj_t *screen, void (*forget)(void));
