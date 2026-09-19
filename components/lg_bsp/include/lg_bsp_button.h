/*
 * lg_bsp_button.h - physical buttons from the board profile (board support layer, D27, D66).
 *
 * Polled, debounced GPIO buttons. A button is known by its index in the profile's buttons[] table;
 * this layer says only that it was pressed briefly or held, and the screens decide what that does
 * from the profile's action bits (lg_board.h). No task and no interrupt: the UI task calls
 * lg_bsp_button_poll() every loop.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LG_BTN_NONE = 0,
    LG_BTN_DOWN,        /* the press began (debounced) */
    LG_BTN_SHORT,       /* released before the hold time: a press */
    LG_BTN_HOLD,        /* still down at the hold time; sent once per press, and no SHORT follows */
    LG_BTN_UP,          /* released after a hold */
} lg_btn_event_kind_t;

typedef struct {
    uint8_t             id;     /* index in the profile's buttons[] */
    lg_btn_event_kind_t kind;
} lg_btn_event_t;

/* Configures the profile's buttons. hold_ms is how long a press must last to be a hold. Returns
 * ESP_ERR_NOT_SUPPORTED when the board has none. */
esp_err_t lg_bsp_button_start(const lg_board_t *board, uint32_t hold_ms);

/* How many buttons were configured (0 before start or on a board without any). */
uint8_t lg_bsp_button_count(void);

/* Samples every button once and returns the next event, if any; call it every 10 to 30 ms from one
 * task. Several events waiting come out one per call. */
bool lg_bsp_button_poll(uint32_t now_ms, lg_btn_event_t *out);

/* How long the button has been down, in ms; 0 when it is up. For a hold's progress bar. */
uint32_t lg_bsp_button_held_ms(uint8_t id, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
