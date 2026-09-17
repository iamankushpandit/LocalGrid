/*
 * Touch calibration for resistive panels, shared by the handheld UI and the test app (D23, D55):
 * three targets, each tapped and held, then the mapping is saved by lg_bsp.
 */
#include "lg_draw.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_bsp_touch.h"

static const char *TAG = "UI";

#define CAL_POINTS     3
#define CAL_TIMEOUT_MS 30000

static void cal_box(const lg_draw_palette_t *p, const char *text, lg_color_t fg, int16_t y, const lg_font_t *font)
{
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ 0, y, (int16_t)lg_draw_width(), (int16_t)(font->line_height + 8) };
    b.bg = b.outside = p->bg;
    b.font = font;
    b.fg = fg;
    b.align = LG_ALIGN_CENTER;
    snprintf(b.text, sizeof(b.text), "%s", text);
    lg_draw_box(&b);
}

static void cal_target(const lg_draw_palette_t *p, int16_t x, int16_t y, bool on)
{
    int16_t arm = (int16_t)(lg_draw_width() / 22);   /* about 15 px on a 320 px panel */
    lg_rect_t h = { (int16_t)(x - arm), (int16_t)(y - 1), (int16_t)(2 * arm + 1), 3 };
    lg_rect_t v = { (int16_t)(x - 1), (int16_t)(y - arm), 3, (int16_t)(2 * arm + 1) };
    lg_draw_fill(&h, on ? p->accent : p->bg);
    lg_draw_fill(&v, on ? p->accent : p->bg);
}

bool lg_draw_calibrate(const lg_draw_palette_t *p)
{
    if (!lg_bsp_touch_can_calibrate()) {
        return true;   /* capacitive: nothing to fit */
    }
    int16_t w = (int16_t)lg_draw_width();
    int16_t h = (int16_t)lg_draw_height();
    lg_draw_scroll_area(0, 0);
    lg_rect_t all = { 0, 0, w, h };
    lg_draw_fill(&all, p->bg);
    int16_t title_y = (int16_t)(h * 30 / 100);
    int16_t hint_y = (int16_t)(title_y + p->title->line_height + 10);
    cal_box(p, "Touch calibration", p->accent, title_y, p->title);
    int16_t screen[CAL_POINTS][2] = {
        { (int16_t)(w * 15 / 100), (int16_t)(h * 12 / 100) },
        { (int16_t)(w * 85 / 100), (int16_t)(h * 50 / 100) },
        { (int16_t)(w * 50 / 100), (int16_t)(h * 88 / 100) },
    };
    int16_t raw[CAL_POINTS][2];
    bool ok = true;
    for (int i = 0; i < CAL_POINTS && ok; i++) {
        char hint[48];
        snprintf(hint, sizeof(hint), "Tap and hold the target, %d of %d", i + 1, CAL_POINTS);
        cal_box(p, hint, p->muted, hint_y, p->small);
        cal_target(p, screen[i][0], screen[i][1], true);
        ok = lg_bsp_touch_wait_press(&raw[i][0], &raw[i][1], CAL_TIMEOUT_MS);
        cal_target(p, screen[i][0], screen[i][1], false);
        if (ok) {
            ESP_LOGI(TAG, "[UI] calibration point %d: raw %d,%d for screen %d,%d", i + 1, raw[i][0], raw[i][1],
                     screen[i][0], screen[i][1]);
        }
    }
    esp_err_t err = ok ? lg_bsp_touch_set_calibration(raw, screen) : ESP_ERR_TIMEOUT;
    ok = !lg_bsp_touch_needs_calibration();
    cal_box(p, ok ? "Calibration saved" : "Calibration failed", ok ? p->accent : p->error, title_y, p->title);
    cal_box(p, ok ? "" : err == ESP_ERR_TIMEOUT ? "No touch for 30 seconds" : "Touch three different places", p->muted,
            hint_y, p->small);
    vTaskDelay(pdMS_TO_TICKS(1500));
    return ok;
}
