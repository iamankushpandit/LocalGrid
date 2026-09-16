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

/* The bar's children, in the order ui_bar_create adds them. Reading the status label by
 * position keeps no table of bars, so screens can be built and freed any number of times. */
#define BAR_CHILD_STATUS 2

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

    (void)lg_ui_text(bar, t->font_small, t->bar_text, "");   /* status, child BAR_CHILD_STATUS */
    return bar;
}

void ui_bar_update(lv_obj_t *bar, const hh_status_t *st)
{
    lv_obj_t *status = bar != NULL ? lv_obj_get_child(bar, BAR_CHILD_STATUS) : NULL;
    if (status == NULL) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    char text[40];
    if (st->link == HH_LINK_ONLINE && !st->time_restricted && st->grid_time != 0) {
        uint32_t day = st->grid_time % 86400u;
        snprintf(text, sizeof(text), "%d dBm  %02u:%02u", st->rssi, (unsigned)(day / 3600u),
                 (unsigned)(day / 60u % 60u));
        lv_obj_set_style_text_color(status, t->bar_text, 0);
    } else if (st->link == HH_LINK_ONLINE) {
        snprintf(text, sizeof(text), "%d dBm  no time", st->rssi);
        lv_obj_set_style_text_color(status, t->warning, 0);
    } else if (st->link == HH_LINK_SEARCHING || st->link == HH_LINK_STOPPED) {
        snprintf(text, sizeof(text), "offline");
        lv_obj_set_style_text_color(status, t->error, 0);
    } else {
        snprintf(text, sizeof(text), "joining");
        lv_obj_set_style_text_color(status, t->warning, 0);
    }
    lg_ui_set_text(status, text);   /* only a changed cluster invalidates the bar */
}
