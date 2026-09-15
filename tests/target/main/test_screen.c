/*
 * Test results and touch check on the handheld's own screen.
 *
 * Layout is relative: widths are percentages of the screen, heights follow
 * content, positions are fractions of the resolution read at runtime, and fonts,
 * spacing, and strokes come from the theme. No pixel values here.
 */
#include "test_screen.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_board.h"
#include "lg_display.h"
#include "lg_theme.h"
#include "lg_touch.h"

#define SUITES  3
#define TARGETS 5

static bool      s_active;
static lv_obj_t *s_results;
static lv_obj_t *s_suite_status[SUITES];
static lv_obj_t *s_result;
static lv_obj_t *s_summary;
static lv_obj_t *s_hint;

static lv_obj_t     *s_check;
static lv_obj_t     *s_targets[TARGETS];
static lv_obj_t     *s_check_status;
static lv_obj_t     *s_check_point;
static lv_obj_t     *s_dot;
static uint8_t       s_hit_mask;
static volatile bool s_recalibrate;

static lv_obj_t *column(lv_obj_t *parent, int16_t gap)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* taps reach the screen */
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *c = column(parent, t->gap);
    lv_obj_set_style_bg_color(c, t->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, t->outline, 0);
    lv_obj_set_style_border_width(c, t->hairline, 0);
    lv_obj_set_style_radius(c, t->radius, 0);
    lv_obj_set_style_pad_all(c, t->pad, 0);
    return c;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click)
{
    lv_obj_t *b = lv_button_create(parent);
    lg_theme_style_button(b);
    lv_obj_set_height(b, lg_theme()->touch_min);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void test_screen_start(const lg_identity_t *identity)
{
    const lg_board_t *board = identity->present ? lg_board_find(identity->board) : NULL;
    if (!lg_board_has_display(board)) {
        return;
    }
    static lg_display_t display;
    if (lg_display_start(board, &display) != ESP_OK) {
        ESP_LOGE("UI", "[UI] Display start failed on %s", board->name);
        return;
    }
    lg_theme_init(display.width, display.height, display.px_per_10mm);
    const lg_theme_t *t = lg_theme();

    lg_display_lock(1000);
    lv_obj_t *scr = lv_screen_active();
    s_results = scr;
    lg_theme_apply_screen(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, t->pad, 0);
    lv_obj_set_style_pad_row(scr, t->gap * 2, 0);

    lv_obj_t *head = column(scr, 0);
    label(head, t->font_title, t->accent, "LocalGrid");
    label(head, t->font_small, t->muted, "Self test");

    lv_obj_t *dev = card(scr);
    label(dev, t->font_small, t->muted, "Device");
    label(dev, t->font_body, t->text, identity->id);
    label(dev, t->font_small, t->muted, board->name);

    lv_obj_t *suites = card(scr);
    static const char *names[SUITES] = { "Core protocol", "Encryption", "Messaging" };
    for (int i = 0; i < SUITES; i++) {
        lv_obj_t *row = lv_obj_create(suites);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, t->font_body, 0);
        lv_obj_set_style_text_color(name, t->text, 0);
        lv_label_set_text(name, names[i]);
        s_suite_status[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_suite_status[i], t->font_small, 0);
        lv_obj_set_style_text_color(s_suite_status[i], t->muted, 0);
        lv_label_set_text(s_suite_status[i], "waiting");
    }

    lv_obj_t *res = column(scr, t->gap);
    s_result = label(res, t->font_huge, t->warning, "RUNNING");
    lv_obj_set_style_text_align(s_result, LV_TEXT_ALIGN_CENTER, 0);
    s_summary = label(res, t->font_small, t->muted, "");
    lv_obj_set_style_text_align(s_summary, LV_TEXT_ALIGN_CENTER, 0);
    s_hint = label(res, t->font_small, t->accent, "");
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lg_display_unlock();
    s_active = true;
}

void test_screen_suite(int index, int checks, int failures, uint32_t elapsed_ms, bool done)
{
    if (!s_active || index < 0 || index >= SUITES) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    char text[40];
    lg_display_lock(1000);
    if (!done) {
        lv_label_set_text(s_suite_status[index], "running");
        lv_obj_set_style_text_color(s_suite_status[index], t->warning, 0);
    } else {
        uint32_t tenths = (elapsed_ms + 50) / 100;
        if (failures) {
            snprintf(text, sizeof(text), "%d of %d failed, %u.%u s", failures, checks, (unsigned)(tenths / 10),
                     (unsigned)(tenths % 10));
        } else {
            snprintf(text, sizeof(text), "%d passed, %u.%u s", checks, (unsigned)(tenths / 10), (unsigned)(tenths % 10));
        }
        lv_label_set_text(s_suite_status[index], text);
        lv_obj_set_style_text_color(s_suite_status[index], failures ? t->error : t->success, 0);
    }
    lg_display_unlock();
}

void test_screen_finish(int checks, int failures, uint32_t min_heap_bytes)
{
    if (!s_active) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    char text[64];
    lg_display_lock(1000);
    lv_label_set_text(s_result, failures ? "FAIL" : "PASS");
    lv_obj_set_style_text_color(s_result, failures ? t->error : t->success, 0);
    snprintf(text, sizeof(text), "%d checks, %d failures\nlowest free memory %u KB", checks, failures,
             (unsigned)(min_heap_bytes / 1024u));
    lv_label_set_text(s_summary, text);
    lg_display_unlock();
}

/* ---- touch check (LVGL callbacks run in the LVGL task with the display lock held) ---- */

static void reset_targets(void)
{
    const lg_theme_t *t = lg_theme();
    s_hit_mask = 0;
    for (int i = 0; i < TARGETS; i++) {
        lv_obj_set_style_bg_color(s_targets[i], t->surface, 0);
    }
    lv_label_set_text(s_check_status, "Tap each target");
    lv_obj_set_style_text_color(s_check_status, t->text, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

static void target_clicked(lv_event_t *e)
{
    const lg_theme_t *t = lg_theme();
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    printf("[UI] touch target %d hit at %d,%d\n", index + 1, (int)p.x, (int)p.y);
    uint8_t all = (uint8_t)((1u << TARGETS) - 1u);
    if (s_hit_mask == all) {
        return;
    }
    s_hit_mask |= (uint8_t)(1u << index);
    lv_obj_set_style_bg_color(s_targets[index], t->success, 0);
    if (s_hit_mask == all) {
        lv_label_set_text(s_check_status, "Touch OK: every target hit");
        lv_obj_set_style_text_color(s_check_status, t->success, 0);
        printf("TOUCH_CHECK: PASS\n");
    } else {
        char text[32];
        snprintf(text, sizeof(text), "%d of %d targets hit", __builtin_popcount(s_hit_mask), TARGETS);
        lv_label_set_text(s_check_status, text);
    }
}

static void check_pressing(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    char text[32];
    snprintf(text, sizeof(text), "Touch at %d, %d", (int)p.x, (int)p.y);
    lv_label_set_text(s_check_point, text);
    int32_t size = lv_obj_get_width(s_dot);
    lv_obj_set_pos(s_dot, p.x - size / 2, p.y - size / 2);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

static void open_results(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_results);
}

static void open_check(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_check);
}

static void recalibrate_clicked(lv_event_t *e)
{
    (void)e;
    s_recalibrate = true;   /* the wizard blocks, so the app task runs it */
}

static void build_check_screen(void)
{
    const lg_theme_t *t = lg_theme();
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    s_check = lv_obj_create(NULL);
    lg_theme_apply_screen(s_check);
    lv_obj_remove_flag(s_check, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_check, check_pressing, LV_EVENT_PRESSING, NULL);

    s_check_status = label(s_check, t->font_title, t->text, "");
    lv_obj_set_style_text_align(s_check_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_check_status, LV_ALIGN_TOP_MID, 0, t->pad);
    s_check_point = label(s_check, t->font_small, t->muted, "Touch anywhere");
    lv_obj_set_style_text_align(s_check_point, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_check_point, s_check_status, LV_ALIGN_OUT_BOTTOM_MID, 0, t->gap);

    /* Target centres as percentages of the screen: four near the corners, one in the middle. */
    static const uint8_t centre[TARGETS][2] = { { 14, 26 }, { 86, 26 }, { 50, 48 }, { 14, 70 }, { 86, 70 } };
    for (int i = 0; i < TARGETS; i++) {
        lv_obj_t *tg = lv_obj_create(s_check);
        lv_obj_remove_style_all(tg);
        lv_obj_add_flag(tg, LV_OBJ_FLAG_EVENT_BUBBLE);   /* the screen also tracks the finger */
        lv_obj_set_size(tg, t->touch_min, t->touch_min);
        lv_obj_set_style_radius(tg, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(tg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tg, t->accent, 0);
        lv_obj_set_style_border_width(tg, t->stroke, 0);
        lv_obj_set_pos(tg, w * centre[i][0] / 100 - t->touch_min / 2, h * centre[i][1] / 100 - t->touch_min / 2);
        lv_obj_add_event_cb(tg, target_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *number = lv_label_create(tg);
        lv_obj_set_style_text_color(number, t->text, 0);
        lv_label_set_text_fmt(number, "%d", i + 1);
        lv_obj_center(number);
        s_targets[i] = tg;
    }

    bool can_calibrate = lg_touch_can_calibrate();
    lv_obj_t *back = button(s_check, "Results", open_results);
    lv_obj_set_width(back, can_calibrate ? LV_PCT(45) : LV_PCT(60));
    lv_obj_align(back, can_calibrate ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_MID, can_calibrate ? t->pad : 0, -t->pad);
    if (can_calibrate) {
        lv_obj_t *cal = button(s_check, "Calibrate", recalibrate_clicked);
        lv_obj_set_width(cal, LV_PCT(45));
        lv_obj_align(cal, LV_ALIGN_BOTTOM_RIGHT, -t->pad, -t->pad);
    }

    s_dot = lv_obj_create(s_check);
    lv_obj_remove_style_all(s_dot);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_dot, t->gap * 2, t->gap * 2);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, t->warning, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);

    reset_targets();
    lv_obj_add_event_cb(s_results, open_check, LV_EVENT_CLICKED, NULL);
}

void test_screen_touch_loop(void)
{
    if (!s_active || !lg_touch_present()) {
        return;
    }
    lg_display_lock(1000);
    lv_label_set_text(s_hint, "Tap the screen to check touch");
    lg_display_unlock();
    lg_touch_wait_press(NULL, NULL, 0);
    while (lg_touch_needs_calibration()) {
        lg_touch_calibrate();
    }

    lg_display_lock(1000);
    build_check_screen();
    lv_label_set_text(s_hint, "Tap the screen for the touch check");
    lv_screen_load(s_check);
    lg_display_unlock();
    printf("[UI] touch check started\n");

    for (;;) {
        if (s_recalibrate) {
            s_recalibrate = false;
            lg_touch_calibrate();
            lg_display_lock(1000);
            reset_targets();
            lg_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
