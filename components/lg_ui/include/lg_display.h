/*
 * lg_display.h - bring up a board's panel and run LVGL on it.
 *
 * The UI never assumes a resolution: it asks LVGL for the display's width and
 * height, which come from the board profile after orientation is applied.
 * All LVGL calls from tasks other than the display task must hold the lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_display_t *display;
    uint16_t      width;
    uint16_t      height;
    uint16_t      px_per_10mm;
} lg_display_t;

/* Initialises the panel, clears it, turns the backlight on, and starts the LVGL task. */
esp_err_t lg_display_start(const lg_board_t *board, lg_display_t *out);

bool lg_display_lock(uint32_t timeout_ms);
void lg_display_unlock(void);

/*
 * Bytes still unused on the drawing task's stack, at its worst point so far, or 0 before the
 * display has started. Screens are built and refreshed on that task, and LVGL's layout
 * recurses, so a screen that nests deeply can overflow it: a reading here says how close the
 * boards that survive actually are, which a panic on the one that does not cannot.
 */
uint32_t lg_display_stack_headroom(void);

#ifdef __cplusplus
}
#endif
