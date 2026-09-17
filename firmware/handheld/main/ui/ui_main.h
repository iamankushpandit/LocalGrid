/*
 * ui_main.h - the handheld UI (D55): starts the UI task, which owns lg_draw and every screen.
 * Other tasks reach it only through the request queue below.
 */
#pragma once

#include "esp_err.h"
#include "lg_board.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t ui_start(const lg_board_t *board);

/* From any task: open the Status screen (true) or the launcher (false) on the UI task. */
void ui_request(bool status);

typedef enum { UI_HOME, UI_STATUS, UI_CHAT, UI_SCROLL, UI_KEYBOARD, UI_TYPE, UI_LOG, UI_PAGE, UI_GO, UI_TAP } ui_cmd_t;

typedef struct {
    ui_cmd_t cmd;
    int      arg;
    uint8_t  scope;      /* UI_CHAT */
    uint32_t target;     /* UI_CHAT */
    char     text[48];   /* UI_TYPE, or the UI_CHAT title */
} ui_req_t;

/* From any task: a console-driven step for testing screens from serial (D28), done on the UI task. */
void ui_request_cmd(ui_cmd_t cmd, int arg, const char *text);

/* From any task: open a conversation (lg_envelope scope and target) with this title. */
void ui_open_chat(uint8_t scope, uint32_t target, const char *title);

/* The UI task's least unused stack so far, in bytes. */
uint32_t ui_stack_headroom(void);
