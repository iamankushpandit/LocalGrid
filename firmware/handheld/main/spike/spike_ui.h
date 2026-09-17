/*
 * spike_ui.h - the no-LVGL spike: a launcher and a live Status screen drawn with lg_draw.
 * Built only with CONFIG_LG_HH_UI_SPIKE, to measure RAM and redraw cost against LVGL.
 */
#pragma once

#include "esp_err.h"
#include "lg_board.h"

#include <stdbool.h>

esp_err_t spike_ui_start(const lg_board_t *board);

/* From any task: open the Status screen (true) or the launcher (false) on the spike's task. */
void spike_ui_request(bool status);
