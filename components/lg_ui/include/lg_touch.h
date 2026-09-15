/*
 * lg_touch.h - touch controllers and the LVGL pointer device.
 *
 * Capacitive controllers (FT6336U) report pixels and are mapped by the board
 * profile. Resistive controllers (XPT2046) report ADC counts and are mapped by a
 * three-point affine calibration kept in NVS (namespace "lgui"), so the default
 * NVS partition must be initialised before lg_touch_start.
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
    bool     down;
    int16_t  x;          /* controller units */
    int16_t  y;
    uint16_t pressure;
} lg_touch_raw_t;

/* Starts the board's touch controller. XPT2046 joins the display's SPI bus, so the
 * bus must already be initialised. Called by lg_display_start. */
esp_err_t lg_touch_start(const lg_board_t *board, int spi_host);

/* Registers the LVGL pointer device. Called by lg_display_start. */
void lg_touch_attach(lv_display_t *display);

bool lg_touch_present(void);
bool lg_touch_can_calibrate(void);
bool lg_touch_needs_calibration(void);

/* One raw reading; safe from any task. */
bool lg_touch_read_raw(lg_touch_raw_t *out);

/* Waits for a steady press, averages it, and waits for release. timeout_ms 0 waits forever. */
bool lg_touch_wait_press(int16_t *raw_x, int16_t *raw_y, uint32_t timeout_ms);

/* Fits and saves a calibration from three raw readings and their screen points. */
esp_err_t lg_touch_set_calibration(const int16_t raw[3][2], const int16_t screen[3][2]);

/* While suspended the pointer device reports no touch. */
void lg_touch_suspend(bool suspended);

/* Full-screen calibration wizard (lg_calibrate.c). Blocks; call from an application
 * task, never from an LVGL event callback. Returns true when a calibration was saved,
 * or at once on boards that need none. */
bool lg_touch_calibrate(void);

#ifdef __cplusplus
}
#endif
