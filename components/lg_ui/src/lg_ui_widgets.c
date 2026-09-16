#include "lg_ui_widgets.h"

#include <string.h>

#include "lg_theme.h"

void lg_ui_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *shown = lv_label_get_text(label);
    if (shown != NULL && strcmp(shown, text) == 0) {
        return;   /* nothing changed, so nothing is invalidated */
    }
    lv_label_set_text(label, text);
}

static lv_obj_t *s_keycap;
static lv_obj_t *s_keycap_label;

void lg_ui_keycap_show(const char *text, int32_t x, int32_t y)
{
    const lg_theme_t *t = lg_theme();
    if (s_keycap == NULL) {
        s_keycap = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_keycap);
        lv_obj_remove_flag(s_keycap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_keycap, t->touch_min * 3 / 2, t->touch_min * 3 / 2);
        lv_obj_set_style_bg_color(s_keycap, t->surface, 0);
        lv_obj_set_style_bg_opa(s_keycap, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_keycap, t->accent, 0);
        lv_obj_set_style_border_width(s_keycap, t->stroke, 0);
        lv_obj_set_style_radius(s_keycap, t->radius, 0);
        s_keycap_label = lv_label_create(s_keycap);
        lv_obj_set_style_text_font(s_keycap_label, t->font_huge, 0);
        lv_obj_set_style_text_color(s_keycap_label, t->text, 0);
        lv_obj_center(s_keycap_label);
    }
    lg_ui_set_text(s_keycap_label, text);

    lv_display_t *disp = lv_display_get_default();
    int32_t screen_w = lv_display_get_horizontal_resolution(disp);
    int32_t w = lv_obj_get_width(s_keycap);
    int32_t h = lv_obj_get_height(s_keycap);
    int32_t px = x - w / 2;
    if (px < 0) {
        px = 0;
    } else if (px + w > screen_w) {
        px = screen_w - w;
    }
    int32_t py = y - h - t->gap * 2;   /* above the finger, where it is not covered */
    if (py < 0) {
        py = y + t->gap * 2;
    }
    lv_obj_set_pos(s_keycap, px, py);
    lv_obj_remove_flag(s_keycap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keycap);
}

void lg_ui_keycap_hide(void)
{
    if (s_keycap != NULL) {
        lv_obj_add_flag(s_keycap, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *lg_ui_column(lv_obj_t *parent, int16_t gap)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *lg_ui_row(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, lg_theme()->gap, 0);
    return o;
}

lv_obj_t *lg_ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *lg_ui_text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *lg_ui_card(lv_obj_t *parent, const char *title)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *c = lg_ui_column(parent, t->gap);
    lv_obj_set_style_bg_color(c, t->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, t->outline, 0);
    lv_obj_set_style_border_width(c, t->hairline, 0);
    lv_obj_set_style_radius(c, t->radius, 0);
    lv_obj_set_style_pad_all(c, t->pad, 0);
    if (title != NULL) {
        lg_ui_label(c, t->font_small, t->muted, title);
    }
    return c;
}

lv_obj_t *lg_ui_icon_button(lv_obj_t *parent, const char *icon, lv_event_cb_t on_click, void *user_data)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *b = lv_button_create(parent);
    lg_theme_style_button_small(b);
    lv_obj_set_style_min_height(b, t->touch_min, 0);
    lv_obj_set_style_min_width(b, t->touch_min, 0);
    lv_obj_set_style_pad_all(b, t->gap / 2, 0);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, t->font_icon, 0);
    lv_label_set_text(l, icon);
    lv_obj_center(l);
    return b;
}

lv_obj_t *lg_ui_button_small(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lg_theme_style_button_small(b);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t   *s_toast;
static lv_obj_t   *s_toast_label;
static lv_timer_t *s_toast_timer;

#define TOAST_MS 15000   /* long enough to notice, pick the handheld up, and tap it */

static void toast_hide(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast != NULL) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void lg_ui_toast_hide(void)
{
    toast_hide(NULL);
}

void lg_ui_toast(const char *text, lv_event_cb_t on_click)
{
    const lg_theme_t *t = lg_theme();
    if (s_toast == NULL) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_toast);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);   /* the whole banner is the target */
        lv_obj_set_width(s_toast, LV_PCT(94));
        lv_obj_set_height(s_toast, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(s_toast, t->touch_min, 0);
        lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, t->pad);
        lv_obj_set_style_bg_color(s_toast, t->bar, 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_toast, t->accent, 0);
        lv_obj_set_style_border_width(s_toast, t->stroke, 0);
        lv_obj_set_style_radius(s_toast, t->radius, 0);
        lv_obj_set_style_pad_all(s_toast, t->gap, 0);
        lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_toast, t->gap / 2, 0);
        s_toast_label = lv_label_create(s_toast);
        lv_obj_set_width(s_toast_label, LV_PCT(100));
        lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s_toast_label, t->bar_text, 0);
        lv_obj_set_style_text_font(s_toast_label, t->font_small, 0);
        lv_obj_t *hint = lv_label_create(s_toast);
        lv_label_set_text(hint, "Tap to open");
        lv_obj_set_style_text_color(hint, t->accent, 0);
        lv_obj_set_style_text_font(hint, t->font_small, 0);
        if (on_click != NULL) {
            lv_obj_add_event_cb(s_toast, on_click, LV_EVENT_CLICKED, NULL);
        }
        s_toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
    lv_label_set_text(s_toast_label, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer != NULL) {
        lv_timer_set_repeat_count(s_toast_timer, 1);
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
    }
}

lv_obj_t *lg_ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lg_theme_style_button(b);
    lv_obj_set_style_min_height(b, lg_theme()->touch_min, 0);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}
