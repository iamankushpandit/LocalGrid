#include "lg_ui_widgets.h"

#include <stdbool.h>
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
    /* A bare glyph, no box: the outline cost width and made every control shout. The target
     * stays a touch target wide; it is only drawn, faintly, while pressed. */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, t->radius, 0);
    lv_obj_set_style_bg_color(b, t->outline, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, t->accent, 0);
    lv_obj_set_style_min_height(b, t->touch_min * 3 / 4, 0);
    lv_obj_set_style_min_width(b, t->touch_min, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_ext_click_area(b, t->gap);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    /* A four-byte UTF-8 sequence is an emoji, which only the 20 px emoji font can draw. */
    bool emoji = (unsigned char)icon[0] >= 0xF0;
    lv_obj_set_style_text_font(l, emoji ? t->font_icon : t->font_symbol, 0);
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
    /* The timer pauses itself rather than running out: LVGL frees a timer whose repeat count
     * reaches 0, and its pointer is kept for the next banner, so a count of 1 was a
     * use-after-free on the second message, and a hang in the drawing task after it. */
    if (s_toast_timer != NULL) {
        lv_timer_pause(s_toast_timer);
    }
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
    }
    lv_label_set_text(s_toast_label, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer != NULL) {
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

/*
 * The brand mark from assets/brand/localgrid-icon.svg, rebuilt from LVGL parts because this
 * build carries no SVG renderer. Coordinates are the SVG's own 64-unit grid, scaled to the
 * requested size at runtime, so the mark matches the admin page's at any panel size.
 */
#define LOGO_GRID 64

static int32_t logo_scale(int32_t v, int32_t size)
{
    return (v * size + LOGO_GRID / 2) / LOGO_GRID;
}

static int32_t logo_stroke(int32_t v, int32_t size)
{
    int32_t s = logo_scale(v, size);
    return s < 1 ? 1 : s;
}

static void logo_arc(lv_obj_t *tile, int32_t size, int32_t radius, lv_opa_t opa)
{
    const lg_theme_t *t = lg_theme();
    int32_t w = logo_stroke(3, size);
    int32_t r = logo_scale(radius, size) + w / 2;   /* LVGL draws the stroke inside the box */
    lv_obj_t *arc = lv_arc_create(tile);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, r * 2, r * 2);
    lv_obj_set_pos(arc, logo_scale(32, size) - r, logo_scale(22, size) - r);
    lv_arc_set_bg_angles(arc, 225, 315);   /* the upper quarter, as the SVG's two waves */
    lv_obj_set_style_arc_color(arc, t->accent, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, w, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
}

static void logo_line(lv_obj_t *tile, int32_t size, lv_point_precise_t *pts, uint32_t n, int32_t width,
                      lv_opa_t opa)
{
    for (uint32_t i = 0; i < n; i++) {
        pts[i].x = logo_scale((int32_t)pts[i].x, size);
        pts[i].y = logo_scale((int32_t)pts[i].y, size);
    }
    lv_obj_t *line = lv_line_create(tile);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, n);   /* LVGL keeps the pointer: the arrays are static */
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_color(line, lg_theme()->accent, 0);
    lv_obj_set_style_line_width(line, logo_stroke(width, size), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
}

static void logo_dot(lv_obj_t *tile, int32_t size, int32_t cx, int32_t cy, int32_t diameter, bool hollow)
{
    const lg_theme_t *t = lg_theme();
    int32_t d = logo_scale(diameter, size);
    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_pos(dot, logo_scale(cx, size) - d / 2, logo_scale(cy, size) - d / 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, hollow ? t->bar : t->accent, 0);
    if (hollow) {
        lv_obj_set_style_border_color(dot, t->accent, 0);
        lv_obj_set_style_border_width(dot, logo_stroke(2, size), 0);
    }
}

lv_obj_t *lg_ui_logo(lv_obj_t *parent, int32_t size)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tile, size, size);
    lv_obj_set_style_radius(tile, logo_scale(14, size), 0);
    lv_obj_set_style_bg_color(tile, t->bar, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    logo_arc(tile, size, 11, LV_OPA_90);
    logo_arc(tile, size, 18, LV_OPA_50);

    /* One logo per screen at most, so the scaled points can live in static arrays. */
    static lv_point_precise_t tent[] = { { 32, 22 }, { 14, 50 }, { 50, 50 }, { 32, 22 } };
    static lv_point_precise_t mast[] = { { 32, 22 }, { 32, 39 } };
    static lv_point_precise_t legs[] = { { 14, 50 }, { 32, 39 }, { 50, 50 } };
    static const lv_point_precise_t tent_src[] = { { 32, 22 }, { 14, 50 }, { 50, 50 }, { 32, 22 } };
    static const lv_point_precise_t mast_src[] = { { 32, 22 }, { 32, 39 } };
    static const lv_point_precise_t legs_src[] = { { 14, 50 }, { 32, 39 }, { 50, 50 } };
    memcpy(tent, tent_src, sizeof(tent));   /* rebuilt screens scale from the originals again */
    memcpy(mast, mast_src, sizeof(mast));
    memcpy(legs, legs_src, sizeof(legs));
    logo_line(tile, size, tent, 4, 3, LV_OPA_COVER);
    logo_line(tile, size, mast, 2, 2, LV_OPA_60);
    logo_line(tile, size, legs, 3, 2, LV_OPA_60);

    logo_dot(tile, size, 32, 22, 9, false);   /* diameters: the SVG radii doubled */
    logo_dot(tile, size, 14, 50, 9, false);
    logo_dot(tile, size, 50, 50, 9, false);
    logo_dot(tile, size, 32, 39, 7, true);
    return tile;
}
