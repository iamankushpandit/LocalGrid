/*
 * lg_bsp_display.h - display panel driver (board support layer, decision D27).
 *
 * Brings up a board's panel from its profile and moves pixels. It knows nothing
 * about LVGL or screens; lg_ui builds on it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from interrupt context when a draw transfer has finished. */
typedef void (*lg_bsp_flush_done_t)(void *ctx);

/* Initialises the SPI bus, any touch controller that shares it, and the panel; clears
 * the panel and turns the backlight on. flush_done fires after each lg_bsp_display_draw. */
esp_err_t lg_bsp_display_start(const lg_board_t *board, lg_bsp_flush_done_t flush_done, void *ctx);

/* The driver's DMA-capable pixel buffer; valid after lg_bsp_display_start. */
uint8_t *lg_bsp_display_buffer(size_t *out_bytes);

/* Sends big-endian RGB565 pixels for the inclusive rectangle x1..x2, y1..y2. */
esp_err_t lg_bsp_display_draw(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const uint8_t *pixels);

void lg_bsp_backlight(bool on);

/* Hardware vertical scroll: rows top_fixed and bottom_fixed stay put, the scroll_rows between
 * them scroll. The three must add up to the panel height. Wait for any pending draw first. */
esp_err_t lg_bsp_display_scroll_area(uint16_t top_fixed, uint16_t scroll_rows, uint16_t bottom_fixed);

/* The panel memory row shown at the top of the scrolling area (top_fixed .. top_fixed+scroll_rows-1). */
esp_err_t lg_bsp_display_scroll_to(uint16_t first_row);

#ifdef __cplusplus
}
#endif
