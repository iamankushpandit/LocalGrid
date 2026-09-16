#include "lg_ui_widgets.h"

#include "lg_theme.h"

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
