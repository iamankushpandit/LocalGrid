/*
 * lg_ui_screensaver.h - green rain over the panel once it has been left alone.
 *
 * Lives on LVGL's top layer, so the screen underneath is untouched and comes back as it was.
 * The first touch dismisses it and does nothing else.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts watching for an untouched panel. After idle_ms without a touch the rain covers the
 * screen until the next touch. Call once, after the theme and the display are up, with the
 * display lock held or before the drawing task starts.
 */
void lg_ui_screensaver_start(uint32_t idle_ms);

/*
 * Switched off, the panel simply stays as it is. Kept in NVS beside the touch calibration, so
 * a handheld somebody turned it off on does not start flashing green again after a reboot.
 */
bool lg_ui_screensaver_enabled(void);
void lg_ui_screensaver_set_enabled(bool enabled);

/*
 * Takes the rain down now. An alert draws on the same top layer, so a saver left running would
 * cover the one screen that must not be covered.
 */
void lg_ui_screensaver_dismiss(void);

/*
 * True while the rain is covering the panel.
 *
 * Needed because the saver draws on LVGL's top layer, so the screen underneath is still the
 * active one: a screen cannot tell from LVGL alone whether anybody can actually see it. A
 * read receipt must not be sent for a message whose reader was looking at a screen saver.
 */
bool lg_ui_screensaver_showing(void);

#ifdef __cplusplus
}
#endif
