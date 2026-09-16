/*
 * ui_home.h - the handheld's home screen (P5).
 * Reads hh_service.h only; never touches the network directly (D27).
 */
#pragma once

#include "lg_identity.h"
#include "lvgl.h"

/* Builds and shows the home screen. Call after the display and theme are started. */
void ui_home_start(const lg_identity_t *identity);

/* The home screen, for other screens' Home buttons. */
lv_obj_t *ui_home_screen(void);
