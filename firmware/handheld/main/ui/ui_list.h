/*
 * ui_list.h - the label/value/action list that information and settings screens are built
 * from, so every one of them looks and behaves the same.
 *
 * Rows are rebuilt from live data rather than kept as widgets, which is why the list has a
 * clear: a screen's refresh empties it and adds the rows again. Structure follows Braino's
 * RowList; the code is LocalGrid's own.
 */
#pragma once

#include "lvgl.h"

/* A scrollable column that fills the space its parent leaves. */
lv_obj_t *ui_list_create(lv_obj_t *parent);

/*
 * Empties the list, keeping the scroll offset so a refresh does not throw the reader back to
 * the top. Rebuilding a list under someone's finger is what made Settings scroll itself up.
 */
void ui_list_clear(lv_obj_t *list);

/* A heading between groups of rows. */
void ui_list_section(lv_obj_t *list, const char *title);

/* Label on the left, value on the right; value may be NULL. */
void ui_list_row(lv_obj_t *list, const char *label, const char *value);

/* Same, but tappable, and drawn as a control. Returns the row. */
lv_obj_t *ui_list_action(lv_obj_t *list, const char *label, const char *value, lv_event_cb_t on_click,
                         void *user_data);

/* A line of explanation, wrapped, in the muted colour. */
void ui_list_note(lv_obj_t *list, const char *text);

/*
 * A fact that cannot be changed: small label and value on one line with a hairline under it,
 * no box. Kept visibly different from actions so nothing looks tappable unless it is.
 */
void ui_list_fact(lv_obj_t *list, const char *label, const char *value);

/* Colours an action row's value as a warning ("needed"), rather than the usual accent. */
void ui_list_value_warn(lv_obj_t *row);

typedef enum {
    UI_BUTTON_PLAIN = 0,   /* outlined, like an action row */
    UI_BUTTON_MAIN,        /* filled with the accent: the one thing a page is for */
    UI_BUTTON_DANGER,      /* filled with the error colour: restarts and resets */
} ui_button_kind_t;

/* A row of buttons sharing the width equally; add them with ui_list_button. */
lv_obj_t *ui_list_buttons(lv_obj_t *list);

/* A centred button, one touch target tall. Its parent is a list or a ui_list_buttons row. */
lv_obj_t *ui_list_button(lv_obj_t *parent, const char *text, ui_button_kind_t kind, lv_event_cb_t on_click,
                         void *user_data);

/*
 * A label and the current choice's name, over one button per choice with the current one
 * filled. Each button's user data is its index, read with lv_event_get_user_data.
 */
void ui_list_choice(lv_obj_t *list, const char *label, const char *const *names, uint8_t count, uint8_t current,
                    lv_event_cb_t on_pick);
