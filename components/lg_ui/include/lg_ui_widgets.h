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
 * An icon-only control: the glyph at the theme's one icon size, on a square target no
 * smaller than the touch minimum. Every icon in the firmware goes through here, so they
 * are all the same size whatever row they sit in.
 */
lv_obj_t *lg_ui_icon_button(lv_obj_t *parent, const char *icon, lv_event_cb_t on_click, void *user_data);

/*
 * Shows one shared banner on LVGL's top layer, above whatever screen is displayed, and
 * hides it after a few seconds. on_click is registered the first time only, so the caller
 * keeps whatever the tap should act on. Call with the display locked.
 */
void lg_ui_toast(const char *text, lv_event_cb_t on_click);

/* Hides the banner now, for example once its conversation has been opened. */
void lg_ui_toast_hide(void);

/*
 * Writes a label only when the text differs. LVGL repaints just the areas it is told
 * changed, so setting identical text is a needless invalidation; screens refresh several
 * times a second and most of what they say stays the same.
 */
void lg_ui_set_text(lv_obj_t *label, const char *text);

/*
 * The magnified key, as phone keyboards have shown since the first iPhone: while a key is
 * held, the glyph appears again above the finger, large enough to read around it. Shown on
 * press, hidden on release. Call with the display locked.
 */
void lg_ui_keycap_show(const char *text, int32_t x, int32_t y);
void lg_ui_keycap_hide(void);

#ifdef __cplusplus
}
#endif
