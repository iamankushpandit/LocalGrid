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
    /*
     * Delivery marker colours (D42). Separate roles rather than borrowed ones: reusing
     * success and accent would bake marker meaning into unrelated roles, and those two are
     * the same value today, which is why delivered and read were indistinguishable.
     */
    lv_color_t      mark_wait;        /* this handheld still holds it */
    lv_color_t      mark_node;        /* a node took it; it may be stored for someone offline */
    lv_color_t      mark_delivered;   /* the recipient's handheld confirmed it */
    lv_color_t      mark_read;        /* that handheld showed it to its reader */
    uint8_t         radius;

    /* Derived from the screen by lg_theme_init. */
    const lv_font_t *font_tiny;    /* markers and timestamps: smaller than body text */
    const lv_font_t *font_small;
    const lv_font_t *font_body;
    const lv_font_t *font_title;
    const lv_font_t *font_huge;
    const lv_font_t *font_icon;   /* emoji glyphs: the emoji font has one size, 20 px */
    const lv_font_t *font_symbol; /* plain symbols (arrows, house, tick): smaller on small screens */
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

/* A secondary button: small text and tight padding, still a full touch target tall. */
void lg_theme_style_button_small(lv_obj_t *button);

#ifdef __cplusplus
}
#endif
