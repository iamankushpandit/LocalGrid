/*
 * The shared top bar.
 *
 * Two sizes only, as Braino learned: the Home control is a full touch target, and the
 * status text beside the title is small. Everything is laid out by flex, so the bar fits
 * any panel width.
 */
#include "ui_bar.h"

#include <stdio.h>

#include "lg_theme.h"
#include "lg_ui_widgets.h"

typedef struct {
    lv_obj_t *title;
    lv_obj_t *status;
} bar_parts_t;

static bar_parts_t s_parts[4];   /* one per screen that has a bar */
static uint8_t     s_count;

static bar_parts_t *parts_of(lv_obj_t *bar)
{
    uint8_t index = (uint8_t)(uintptr_t)lv_obj_get_user_data(bar);
    return index < s_count ? &s_parts[index] : NULL;
}

lv_obj_t *ui_bar_create(lv_obj_t *screen, const char *title, lv_event_cb_t on_home)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, t->bar, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, t->radius, 0);
    lv_obj_set_style_pad_all(bar, t->gap / 2, 0);
    lv_obj_set_style_pad_column(bar, t->gap, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lg_ui_icon_button(bar, LV_SYMBOL_HOME, on_home, NULL);   /* an icon; the title needs the width */

    lv_obj_t *label = lg_ui_text(bar, t->font_body, t->bar_text, title);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *status = lg_ui_text(bar, t->font_small, t->bar_text, "");

    if (s_count < (uint8_t)(sizeof(s_parts) / sizeof(s_parts[0]))) {
        s_parts[s_count].title = label;
        s_parts[s_count].status = status;
        lv_obj_set_user_data(bar, (void *)(uintptr_t)s_count);
        s_count++;
    }
    return bar;
}

void ui_bar_update(lv_obj_t *bar, const hh_status_t *st)
{
    bar_parts_t *p = bar != NULL ? parts_of(bar) : NULL;
    if (p == NULL) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    char text[40];
    if (st->link == HH_LINK_ONLINE && !st->time_restricted && st->grid_time != 0) {
        uint32_t day = st->grid_time % 86400u;
        snprintf(text, sizeof(text), "%d dBm  %02u:%02u", st->rssi, (unsigned)(day / 3600u),
                 (unsigned)(day / 60u % 60u));
        lv_obj_set_style_text_color(p->status, t->bar_text, 0);
    } else if (st->link == HH_LINK_ONLINE) {
        snprintf(text, sizeof(text), "%d dBm  no time", st->rssi);
        lv_obj_set_style_text_color(p->status, t->warning, 0);
    } else if (st->link == HH_LINK_SEARCHING || st->link == HH_LINK_STOPPED) {
        snprintf(text, sizeof(text), "offline");
        lv_obj_set_style_text_color(p->status, t->error, 0);
    } else {
        snprintf(text, sizeof(text), "joining");
        lv_obj_set_style_text_color(p->status, t->warning, 0);
    }
    lg_ui_set_text(p->status, text);   /* only a changed cluster invalidates the bar */
}
