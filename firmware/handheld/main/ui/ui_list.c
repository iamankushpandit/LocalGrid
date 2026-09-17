#include "ui_list.h"

#include "lg_theme.h"
#include "lg_ui_widgets.h"

lv_obj_t *ui_list_create(lv_obj_t *parent)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *list = lg_ui_column(parent, t->gap);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_height(list, LV_PCT(100));
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

void ui_list_clear(lv_obj_t *list)
{
    /*
     * Keep the reader's place. lv_obj_clean zeroes the list's scroll offset, so a screen that
     * rebuilds on a refresh scrolled itself back to the top under the reader's finger: the bug
     * the owner hit in Settings. Deleting the rows one by one leaves the offset alone, the new
     * rows are placed under it when LVGL next lays the screen out. If the new rows are shorter,
     * LVGL pulls the offset back in that same layout pass: a deleted child marks the list with
     * readjust_scroll_after_layout (lv_obj.c, LV_EVENT_CHILD_DELETED).
     *
     * Putting the offset back by hand instead (lv_obj_scroll_to_y after the rows were added)
     * forced a whole-screen layout pass inside the refresh timer, on top of the rebuild, on the
     * drawing task's small stack. Nothing here forces layout.
     *
     * Last to first, and by index: a row that is already being deleted stays in the child list
     * and lv_obj_delete ignores it, so a loop over "the first child" would never end.
     */
    for (int32_t i = (int32_t)lv_obj_get_child_count(list) - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(list, i));
    }
}

void ui_list_section(lv_obj_t *list, const char *title)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *label = lg_ui_label(list, t->font_small, t->accent, title);
    lv_obj_set_style_pad_top(label, t->gap, 0);
}

static lv_obj_t *row_shell(lv_obj_t *list, bool tappable)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *row = tappable ? lv_button_create(list) : lv_obj_create(list);
    if (tappable) {
        lg_theme_style_button(row);
    } else {
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, t->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, t->outline, 0);
        lv_obj_set_style_border_width(row, t->hairline, 0);
        lv_obj_set_style_radius(row, t->radius, 0);
    }
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, t->touch_min, 0);
    lv_obj_set_style_pad_all(row, t->gap, 0);
    lv_obj_set_style_pad_column(row, t->gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void fill_row(lv_obj_t *row, const char *label, const char *value, lv_color_t value_color)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *left = lg_ui_text(row, t->font_body, t->text, label);
    lv_obj_set_flex_grow(left, 1);
    lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
    if (value != NULL && value[0] != '\0') {
        lg_ui_text(row, t->font_small, value_color, value);
    }
}

void ui_list_row(lv_obj_t *list, const char *label, const char *value)
{
    fill_row(row_shell(list, false), label, value, lg_theme()->muted);
}

lv_obj_t *ui_list_action(lv_obj_t *list, const char *label, const char *value, lv_event_cb_t on_click,
                         void *user_data)
{
    lv_obj_t *row = row_shell(list, true);
    fill_row(row, label, value, lg_theme()->accent);
    lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, user_data);
    return row;
}

void ui_list_note(lv_obj_t *list, const char *text)
{
    const lg_theme_t *t = lg_theme();
    lg_ui_label(list, t->font_small, t->muted, text);
}
