#include "ui_list.h"

#include <stdint.h>

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

void ui_list_fact(lv_obj_t *list, const char *label, const char *value)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, t->gap, 0);
    lv_obj_set_style_pad_ver(row, t->gap / 2, 0);
    lv_obj_set_style_pad_column(row, t->gap, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, t->outline, 0);
    lv_obj_set_style_border_width(row, t->hairline, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lg_ui_text(row, t->font_small, t->muted, label);
    lv_obj_t *right = lg_ui_text(row, t->font_small, t->text, value != NULL ? value : "");
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
}

void ui_list_value_warn(lv_obj_t *row)
{
    lv_obj_t *value = lv_obj_get_child(row, 1);   /* fill_row adds the label, then the value */
    if (value != NULL) {
        lv_obj_set_style_text_color(value, lg_theme()->warning, 0);
    }
}

lv_obj_t *ui_list_buttons(lv_obj_t *list)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, t->gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

lv_obj_t *ui_list_button(lv_obj_t *parent, const char *text, ui_button_kind_t kind, lv_event_cb_t on_click,
                         void *user_data)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *b = lv_button_create(parent);
    if (kind == UI_BUTTON_MAIN) {
        lg_theme_style_button_filled(b, t->accent);
    } else if (kind == UI_BUTTON_DANGER) {
        lg_theme_style_button_filled(b, t->error);
    } else {
        lg_theme_style_button(b);
    }
    if (lv_obj_get_style_flex_flow(parent, 0) == LV_FLEX_FLOW_ROW) {
        lv_obj_set_flex_grow(b, 1);   /* a buttons row: equal shares of the width */
    } else {
        lv_obj_set_width(b, LV_PCT(100));
    }
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, t->touch_min, 0);
    lv_obj_set_style_pad_all(b, t->gap, 0);
    lv_obj_t *l = lg_ui_text(b, t->font_body, lv_obj_get_style_text_color(b, 0), text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    return b;
}

void ui_list_choice(lv_obj_t *list, const char *label, const char *const *names, uint8_t count, uint8_t current,
                    lv_event_cb_t on_pick)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *head = lv_obj_create(list);
    lv_obj_remove_style_all(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lg_ui_text(head, t->font_small, t->muted, label);
    lg_ui_text(head, t->font_small, t->muted, current < count ? names[current] : "");

    lv_obj_t *row = ui_list_buttons(list);
    lv_obj_set_style_pad_column(row, t->hairline * 2, 0);
    for (uint8_t i = 0; i < count; i++) {
        lv_obj_t *b = ui_list_button(row, names[i], i == current ? UI_BUTTON_MAIN : UI_BUTTON_PLAIN, on_pick,
                                     (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(b, 0, 0);   /* four names across a 240 px panel */
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), t->font_small, 0);
        if (i != current) {
            lv_obj_set_style_border_color(b, t->outline, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), t->muted, 0);
        }
    }
}
