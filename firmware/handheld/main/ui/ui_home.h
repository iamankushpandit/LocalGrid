/*
 * ui_home.h - the handheld's home screen (P5).
 * Reads hh_service.h only; never touches the network directly (D27).
 */
#pragma once

#include "lg_identity.h"

/* Builds and shows the home screen. Call after the display and theme are started. */
void ui_home_start(const lg_identity_t *identity);
