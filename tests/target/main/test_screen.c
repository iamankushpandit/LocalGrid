/*
 * Test results and touch check on the handheld's own screen (D23), drawn with lg_draw (D55).
 *
 * Positions are fractions of the resolution read at runtime and heights follow the fonts'
 * line heights; the colours below are the handheld theme's roles.
 */
#include "test_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_board.h"
#include "lg_bsp_board.h"
#include "lg_bsp_touch.h"
#include "lg_draw.h"

#define SUITES  5
#define TARGETS 5

#define C_BG      lg_rgb(0x000000)
#define C_SURFACE lg_rgb(0x0A140F)
#define C_OUTLINE lg_rgb(0x1F3A2A)
#define C_TEXT    lg_rgb(0xD2F5DE)
#define C_MUTED   lg_rgb(0x7FA78F)
#define C_ACCENT  lg_rgb(0x5FD38D)
#define C_WARNING lg_rgb(0xF0B64A)
#define C_ERROR   lg_rgb(0xFF6B6B)

#define F_SMALL (&lg_font_montserrat_12)
#define F_BODY  (&lg_font_montserrat_14)
#define F_TITLE (&lg_font_montserrat_20)
#define F_HUGE  (&lg_font_montserrat_28)

static bool     s_active;
static uint16_t s_w;
static uint16_t s_h;
static int16_t  s_pad;

/* Results screen. */
static lg_box_t s_head;
static lg_box_t s_sub;
static lg_box_t s_card;
static lg_box_t s_device[2];
static lg_box_t s_suite_name[SUITES];
static lg_box_t s_suite_status[SUITES];
static lg_box_t s_result;
static lg_box_t s_summary[2];
static lg_box_t s_hint;

/* Touch check screen. */
static lg_box_t s_check_status;
static lg_box_t s_check_point;
static lg_box_t s_targets[TARGETS];
static lg_box_t s_button_results;
static lg_box_t s_button_calibrate;
static uint8_t  s_hit_mask;

static lg_box_t box(int16_t x, int16_t y, int16_t w, const lg_font_t *font, lg_color_t fg, lg_color_t bg,
                    uint8_t align, const char *text)
{
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ x, y, w, (int16_t)(font->line_height + 4) };
    b.bg = bg;
    b.outside = bg;
    b.font = font;
    b.fg = fg;
    b.align = align;
    snprintf(b.text, sizeof(b.text), "%s", text);
    return b;
}

static void draw_results(void)
{
    lg_rect_t all = { 0, 0, (int16_t)s_w, (int16_t)s_h };
    lg_draw_fill(&all, C_BG);
    lg_draw_box(&s_head);
    lg_draw_box(&s_sub);
    lg_draw_box(&s_card);
    for (int i = 0; i < 2; i++) {
        lg_draw_box(&s_device[i]);
    }
    for (int i = 0; i < SUITES; i++) {
        lg_draw_box(&s_suite_name[i]);
        lg_draw_box(&s_suite_status[i]);
    }
    lg_draw_box(&s_result);
    lg_draw_box(&s_summary[0]);
    lg_draw_box(&s_summary[1]);
    lg_draw_box(&s_hint);
}

void test_screen_start(const lg_identity_t *identity)
{
    /* D66: the Waveshare 1.47in C6 comes in two wirings under one name; drive whichever it is. */
    const lg_board_t *board = lg_bsp_board_resolve(identity->present ? lg_board_find(identity->board) : NULL);
    if (!lg_board_has_display(board)) {
        return;
    }
    if (lg_draw_start(board, C_BG, &s_w, &s_h) != ESP_OK) {
        ESP_LOGE("UI", "[UI] Display start failed on %s", board->name);
        return;
    }
    s_pad = (int16_t)(s_w / 40);
    int16_t inner = (int16_t)(s_w - 2 * s_pad);
    int16_t y = s_pad;
    s_head = box(s_pad, y, inner, F_TITLE, C_ACCENT, C_BG, LG_ALIGN_LEFT, "LocalGrid");
    y = (int16_t)(y + s_head.rect.h);
    s_sub = box(s_pad, y, inner, F_SMALL, C_MUTED, C_BG, LG_ALIGN_LEFT, "Self test");
    y = (int16_t)(y + s_sub.rect.h + s_pad);

    /* One card holds the device and the suites. */
    int16_t card_top = y;
    int16_t cx = (int16_t)(s_pad * 2);
    int16_t cw = (int16_t)(s_w - 4 * s_pad);
    y = (int16_t)(y + s_pad);
    s_device[0] = box(cx, y, cw, F_BODY, C_TEXT, C_SURFACE, LG_ALIGN_LEFT, identity->id);
    y = (int16_t)(y + s_device[0].rect.h);
    s_device[1] = box(cx, y, cw, F_SMALL, C_MUTED, C_SURFACE, LG_ALIGN_LEFT, board->name);
    y = (int16_t)(y + s_device[1].rect.h + s_pad);
    static const char *names[SUITES] = { "Core protocol", "Encryption", "Messaging", "LoRa backbone",
                                         "Device identity" };
    for (int i = 0; i < SUITES; i++) {
        s_suite_name[i] = box(cx, y, (int16_t)(cw * 45 / 100), F_BODY, C_TEXT, C_SURFACE, LG_ALIGN_LEFT, names[i]);
        s_suite_status[i] = box((int16_t)(cx + cw * 45 / 100), y, (int16_t)(cw - cw * 45 / 100), F_SMALL, C_MUTED,
                                C_SURFACE, LG_ALIGN_RIGHT, "waiting");
        s_suite_status[i].rect.h = s_suite_name[i].rect.h;
        y = (int16_t)(y + s_suite_name[i].rect.h + s_pad / 2);
    }
    y = (int16_t)(y + s_pad / 2);
    memset(&s_card, 0, sizeof(s_card));
    s_card.rect = (lg_rect_t){ s_pad, card_top, inner, (int16_t)(y - card_top) };
    s_card.bg = C_SURFACE;
    s_card.outside = C_BG;
    s_card.border = C_OUTLINE;
    s_card.border_w = 1;
    s_card.radius = (uint8_t)s_pad;
    y = (int16_t)(y + 2 * s_pad);

    s_result = box(s_pad, y, inner, F_HUGE, C_WARNING, C_BG, LG_ALIGN_CENTER, "RUNNING");
    y = (int16_t)(y + s_result.rect.h + s_pad);
    s_summary[0] = box(s_pad, y, inner, F_SMALL, C_MUTED, C_BG, LG_ALIGN_CENTER, "");
    y = (int16_t)(y + s_summary[0].rect.h);
    s_summary[1] = box(s_pad, y, inner, F_SMALL, C_MUTED, C_BG, LG_ALIGN_CENTER, "");
    y = (int16_t)(y + s_summary[1].rect.h + s_pad);
    s_hint = box(s_pad, y, inner, F_SMALL, C_ACCENT, C_BG, LG_ALIGN_CENTER, "");
    draw_results();
    s_active = true;
}

void test_screen_suite(int index, int checks, int failures, uint32_t elapsed_ms, bool done)
{
    if (!s_active || index < 0 || index >= SUITES) {
        return;
    }
    char text[LG_BOX_TEXT_MAX];
    if (!done) {
        s_suite_status[index].fg = C_WARNING;
        snprintf(text, sizeof(text), "running");
    } else {
        uint32_t tenths = (elapsed_ms + 50) / 100;
        if (failures) {
            snprintf(text, sizeof(text), "%d of %d failed, %u.%u s", failures, checks, (unsigned)(tenths / 10),
                     (unsigned)(tenths % 10));
        } else {
            snprintf(text, sizeof(text), "%d passed, %u.%u s", checks, (unsigned)(tenths / 10), (unsigned)(tenths % 10));
        }
        s_suite_status[index].fg = failures ? C_ERROR : C_ACCENT;
    }
    snprintf(s_suite_status[index].text, sizeof(s_suite_status[index].text), "%s", text);
    lg_draw_box(&s_suite_status[index]);   /* the colour may change with the same words */
}

void test_screen_finish(int checks, int failures, uint32_t min_heap_bytes)
{
    if (!s_active) {
        return;
    }
    s_result.fg = failures ? C_ERROR : C_ACCENT;
    snprintf(s_result.text, sizeof(s_result.text), "%s", failures ? "FAIL" : "PASS");
    lg_draw_box(&s_result);
    char text[LG_BOX_TEXT_MAX];
    snprintf(text, sizeof(text), "%d checks, %d failures", checks, failures);
    lg_draw_set_text(&s_summary[0], text);
    snprintf(text, sizeof(text), "lowest free memory %u KB", (unsigned)(min_heap_bytes / 1024u));
    lg_draw_set_text(&s_summary[1], text);
}

/* ---- touch check ---- */

/* Calibrates once when asked (always), then again until the panel has a calibration. */
static void calibrate(bool always)
{
    const lg_draw_palette_t p = { .bg = C_BG, .accent = C_ACCENT, .muted = C_MUTED, .error = C_ERROR,
                                  .title = F_TITLE, .small = F_SMALL };
    if (always) {
        (void)lg_draw_calibrate(&p);
    }
    while (lg_bsp_touch_needs_calibration()) {
        (void)lg_draw_calibrate(&p);
    }
}

static void draw_button(lg_box_t *b, bool pressed)
{
    b->bg = pressed ? C_OUTLINE : C_SURFACE;
    lg_draw_box(b);
}

static void draw_check(void)
{
    lg_rect_t all = { 0, 0, (int16_t)s_w, (int16_t)s_h };
    lg_draw_fill(&all, C_BG);
    s_hit_mask = 0;
    s_check_status.fg = C_TEXT;
    snprintf(s_check_status.text, sizeof(s_check_status.text), "Tap each target");
    lg_draw_box(&s_check_status);
    snprintf(s_check_point.text, sizeof(s_check_point.text), "Touch anywhere");
    lg_draw_box(&s_check_point);
    for (int i = 0; i < TARGETS; i++) {
        s_targets[i].bg = C_SURFACE;
        lg_draw_box(&s_targets[i]);
    }
    draw_button(&s_button_results, false);
    if (lg_bsp_touch_can_calibrate()) {
        draw_button(&s_button_calibrate, false);
    }
}

static void build_check(void)
{
    int16_t inner = (int16_t)(s_w - 2 * s_pad);
    s_check_status = box(s_pad, s_pad, inner, F_TITLE, C_TEXT, C_BG, LG_ALIGN_CENTER, "");
    s_check_point = box(s_pad, (int16_t)(s_pad + s_check_status.rect.h), inner, F_SMALL, C_MUTED, C_BG,
                        LG_ALIGN_CENTER, "");
    /* Target centres as percentages of the screen: four near the corners, one in the middle. */
    static const uint8_t centre[TARGETS][2] = { { 14, 26 }, { 86, 26 }, { 50, 48 }, { 14, 70 }, { 86, 70 } };
    int16_t size = (int16_t)(s_w / 7);   /* about 9 mm on these panels */
    for (int i = 0; i < TARGETS; i++) {
        char number[4];
        snprintf(number, sizeof(number), "%d", i + 1);
        lg_box_t *t = &s_targets[i];
        *t = box((int16_t)(s_w * centre[i][0] / 100 - size / 2), (int16_t)(s_h * centre[i][1] / 100 - size / 2), size,
                 F_BODY, C_TEXT, C_SURFACE, LG_ALIGN_CENTER, number);
        t->rect.h = size;
        t->outside = C_BG;
        t->border = C_ACCENT;
        t->border_w = 2;
        t->radius = (uint8_t)(size / 2);
    }
    bool can_calibrate = lg_bsp_touch_can_calibrate();
    int16_t bw = can_calibrate ? (int16_t)(s_w * 45 / 100) : (int16_t)(s_w * 60 / 100);
    int16_t bh = size;
    int16_t by = (int16_t)(s_h - s_pad - bh);
    s_button_results = box(can_calibrate ? s_pad : (int16_t)((s_w - bw) / 2), by, bw, F_BODY, C_ACCENT, C_SURFACE,
                           LG_ALIGN_CENTER, "Results");
    s_button_calibrate = box((int16_t)(s_w - s_pad - bw), by, bw, F_BODY, C_ACCENT, C_SURFACE, LG_ALIGN_CENTER,
                             "Calibrate");
    for (lg_box_t *b = &s_button_results; b <= &s_button_calibrate; b++) {
        b->rect.h = bh;
        b->outside = C_BG;
        b->border = C_ACCENT;
        b->border_w = 1;
        b->radius = (uint8_t)s_pad;
    }
}

static void target_hit(int index, int16_t x, int16_t y)
{
    printf("[UI] touch target %d hit at %d,%d\n", index + 1, x, y);
    uint8_t all = (uint8_t)((1u << TARGETS) - 1u);
    if (s_hit_mask == all) {
        return;
    }
    s_hit_mask |= (uint8_t)(1u << index);
    s_targets[index].bg = C_ACCENT;
    s_targets[index].fg = C_BG;
    lg_draw_box(&s_targets[index]);
    s_targets[index].fg = C_TEXT;
    char text[LG_BOX_TEXT_MAX];
    if (s_hit_mask == all) {
        s_check_status.fg = C_ACCENT;
        snprintf(text, sizeof(text), "Touch OK: every target hit");
        printf("TOUCH_CHECK: PASS\n");
    } else {
        snprintf(text, sizeof(text), "%d of %d targets hit", __builtin_popcount(s_hit_mask), TARGETS);
    }
    snprintf(s_check_status.text, sizeof(s_check_status.text), "%s", text);
    lg_draw_box(&s_check_status);
}

void test_screen_touch_loop(void)
{
    if (!s_active || !lg_bsp_touch_present()) {
        return;
    }
    lg_draw_set_text(&s_hint, "Tap the screen to check touch");
    lg_bsp_touch_wait_press(NULL, NULL, 0);
    calibrate(false);
    build_check();
    snprintf(s_hint.text, sizeof(s_hint.text), "Tap the screen for the touch check");
    draw_check();
    printf("[UI] touch check started\n");

    bool on_check = true;
    bool was_down = false;
    int16_t x = 0;
    int16_t y = 0;
    lg_box_t *pressed = NULL;
    for (;;) {
        int16_t nx = 0;
        int16_t ny = 0;
        bool down = lg_draw_touch(&nx, &ny);
        if (down) {
            x = nx;
            y = ny;
        }
        if (on_check && down) {
            char text[LG_BOX_TEXT_MAX];
            snprintf(text, sizeof(text), "Touch at %d, %d", x, y);
            lg_draw_set_text(&s_check_point, text);
            if (!was_down) {
                for (lg_box_t *b = &s_button_results; b <= &s_button_calibrate; b++) {
                    if (lg_rect_hit(&b->rect, x, y) && (b == &s_button_results || lg_bsp_touch_can_calibrate())) {
                        pressed = b;
                        draw_button(b, true);
                    }
                }
            }
        }
        if (was_down && !down) {   /* a tap ends where the finger last was */
            if (!on_check) {
                on_check = true;
                draw_check();
            } else if (pressed != NULL) {
                bool inside = lg_rect_hit(&pressed->rect, x, y);
                draw_button(pressed, false);
                if (inside && pressed == &s_button_results) {
                    on_check = false;
                    draw_results();
                } else if (inside) {
                    calibrate(true);
                    draw_check();
                }
                pressed = NULL;
            } else {
                for (int i = 0; i < TARGETS; i++) {
                    if (lg_rect_hit(&s_targets[i].rect, x, y)) {
                        target_hit(i, x, y);
                    }
                }
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
