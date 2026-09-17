/*
 * ui_launcher.h - the first screen: a grid of tiles, one per thing the handheld does.
 *
 * Nothing but the launcher lives here; each tile opens its own screen, which carries the
 * shared top bar back to this one. Structure follows Braino's launcher; the code is
 * LocalGrid's own.
 */
#pragma once

#include "lg_identity.h"
#include "lvgl.h"

/* Builds the launcher and shows it. Call after the display and theme are started. */
void ui_launcher_start(const lg_identity_t *identity);

/* Shows the launcher again, for the Home control on other screens. */
void ui_launcher_open(void);

/* The launcher screen, for other screens' Home buttons. */
lv_obj_t *ui_launcher_screen(void);
