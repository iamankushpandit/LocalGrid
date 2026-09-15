/*
 * lg_theme.h - the single place colours, fonts, and spacing come from (decision D10).
 *
 * Roles follow Braino's palette design: a screen asks for "muted text" or "success",
 * never a colour value, so adding a theme later changes no screen code. Sizes are
 * derived from the screen at runtime (decision D9).
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char     *name;
    lv_color_t      bg;
    lv_color_t      bar;
    lv_color_t      bar_text;
    lv_color_t      surface;
    lv_color_t      outline;
    lv_color_t      text;
    lv_color_t      muted;
    lv_color_t      accent;
    lv_color_t      success;
    lv_color_t      error;
    lv_color_t      warning;
    uint8_t         radius;

    /* Derived from the screen by lg_theme_init. */
    const lv_font_t *font_small;
    const lv_font_t *font_body;
    const lv_font_t *font_title;
    const lv_font_t *font_huge;
    int16_t          pad;          /* outer padding */
    int16_t          gap;          /* space between stacked items */
    int16_t          touch_min;    /* smallest touch target, from 8 mm */
    int16_t          stroke;       /* lines that must be seen: targets, crosshairs */
    int16_t          hairline;     /* borders */
} lg_theme_t;

/* Picks font sizes and spacing for a screen of this size and density. */
void lg_theme_init(uint16_t width, uint16_t height, uint16_t px_per_10mm);

const lg_theme_t *lg_theme(void);

/* Styles a screen's background and default text from the theme. */
void lg_theme_apply_screen(lv_obj_t *screen);

/* Styles a button from the theme. */
void lg_theme_style_button(lv_obj_t *button);

#ifdef __cplusplus
}
#endif
