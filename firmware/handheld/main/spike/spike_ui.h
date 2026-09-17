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

typedef enum { SPIKE_HOME, SPIKE_STATUS, SPIKE_CHAT, SPIKE_SCROLL, SPIKE_KEYBOARD, SPIKE_TYPE, SPIKE_LOG, SPIKE_PAGE, SPIKE_GO, SPIKE_TAP } spike_cmd_t;

typedef struct {
    spike_cmd_t cmd;
    int         arg;
    char        text[48];
} spike_req_t;

/* From any task: a console-driven step for measurement, done on the spike's task. */
void spike_ui_request_cmd(spike_cmd_t cmd, int arg, const char *text);
