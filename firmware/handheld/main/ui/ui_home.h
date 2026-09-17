/*
 * ui_home.h - the handheld's home screen (P5).
 * Reads hh_service.h only; never touches the network directly (D27).
 */
#pragma once

#include "lg_identity.h"
#include "lvgl.h"

/* Builds the Status screen, reached from the launcher's Status tile. */
void ui_home_build(const lg_identity_t *identity);

/* Shows it. */
void ui_home_open(void);

/* The Status screen, for other screens' Home buttons. */
lv_obj_t *ui_home_screen(void);
