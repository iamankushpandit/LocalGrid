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
