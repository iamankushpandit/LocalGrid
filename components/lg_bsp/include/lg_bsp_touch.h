/*
 * lg_bsp_touch.h - touch controller drivers (board support layer, decision D27).
 *
 * Capacitive controllers (FT6336U) report pixels and are mapped by the board
 * profile. Resistive controllers (XPT2046) report ADC counts and are mapped by a
 * three-point affine calibration kept in NVS (namespace "lgui"), so the default NVS
 * partition must be initialised before the display starts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     down;
    int16_t  x;          /* controller units */
    int16_t  y;
    uint16_t pressure;
} lg_bsp_touch_raw_t;

/* Starts the board's touch controller. An XPT2046 joins the display's SPI bus (spi_host), so
 * lg_bsp_display_start calls this once the bus exists; one whose profile gives it pins of its
 * own gets a second SPI host instead. */
esp_err_t lg_bsp_touch_start(const lg_board_t *board, int spi_host);

bool lg_bsp_touch_present(void);
bool lg_bsp_touch_can_calibrate(void);
bool lg_bsp_touch_needs_calibration(void);

/* One raw reading; safe from any task. */
bool lg_bsp_touch_read_raw(lg_bsp_touch_raw_t *out);

/* Maps a raw reading to screen pixels. False if the panel is not calibrated. */
bool lg_bsp_touch_map(const lg_bsp_touch_raw_t *raw, int16_t *x, int16_t *y);

/* Waits for a steady press, averages it, and waits for release. timeout_ms 0 waits forever. */
bool lg_bsp_touch_wait_press(int16_t *raw_x, int16_t *raw_y, uint32_t timeout_ms);

/* Fits and saves a calibration from three raw readings and their screen points. */
esp_err_t lg_bsp_touch_set_calibration(const int16_t raw[3][2], const int16_t screen[3][2]);

#ifdef __cplusplus
}
#endif
