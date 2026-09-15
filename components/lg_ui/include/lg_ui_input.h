/*
 * lg_ui_input.h - LVGL pointer device and touch calibration screen.
 * Readings come from lg_bsp_touch; nothing here talks to hardware.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the pointer device when the board has touch. Called by lg_display_start. */
void lg_ui_input_attach(lv_display_t *display);

/* While suspended the pointer device reports no touch. */
void lg_ui_input_suspend(bool suspended);

/* Full-screen calibration for resistive panels. Blocks; call from an application task,
 * never from an LVGL event callback. True when a calibration is in use, or at once on
 * boards that need none. */
bool lg_ui_calibrate(void);

#ifdef __cplusplus
}
#endif
