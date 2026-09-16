/*
 * lg_ui_widgets.h - small building blocks for themed, resolution-independent screens.
 * Widths are percentages or content-sized; spacing, fonts, and colours come from lg_theme.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Full-width transparent column; not clickable, so taps reach what is behind it. */
lv_obj_t *lg_ui_column(lv_obj_t *parent, int16_t gap);

/* Full-width transparent row with its children spread to both ends. */
lv_obj_t *lg_ui_row(lv_obj_t *parent);

/* Full-width label that wraps. */
lv_obj_t *lg_ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);

/* Content-sized label for rows. */
lv_obj_t *lg_ui_text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);

/* Surface card: a padded column with an optional muted title. */
lv_obj_t *lg_ui_card(lv_obj_t *parent, const char *title);

/* Themed button at least one touch target tall, with a centred label. */
lv_obj_t *lg_ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *user_data);

/* Secondary button: small text, tight padding, content width. Returns the button; its
 * label is its first child, for callers that change the text. */
lv_obj_t *lg_ui_button_small(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *user_data);

/*
 * Shows one shared banner on LVGL's top layer, above whatever screen is displayed, and
 * hides it after a few seconds. on_click is registered the first time only, so the caller
 * keeps whatever the tap should act on. Call with the display locked.
 */
void lg_ui_toast(const char *text, lv_event_cb_t on_click);

#ifdef __cplusplus
}
#endif
