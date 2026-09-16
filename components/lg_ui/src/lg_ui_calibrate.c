/*
 * Touch calibration screen for resistive panels.
 *
 * Three targets at fixed fractions of the screen, far apart and clear of the edges
 * where resistive film is least linear. Pixel positions are derived from the
 * resolution at runtime; sizes and strokes come from the theme. The fit itself and
 * its storage belong to lg_bsp_touch.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_bsp_touch.h"
#include "lg_display.h"
#include "lg_theme.h"
#include "lg_ui_input.h"

#define CAL_POINTS     3
#define CAL_TIMEOUT_MS 30000

static lv_obj_t *crosshair(lv_obj_t *parent)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, t->touch_min, t->touch_min);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(c, t->warning, 0);
    lv_obj_set_style_border_width(c, t->stroke, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_obj_create(c);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_color(bar, t->warning, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_size(bar, i == 0 ? LV_PCT(100) : t->stroke, i == 0 ? t->stroke : LV_PCT(100));
        lv_obj_center(bar);
    }
    return c;
}

static void delete_when_left(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_target_obj(e));
}

bool lg_ui_calibrate(void)
{
    if (!lg_bsp_touch_can_calibrate()) {
        return true;
    }
    const lg_theme_t *t = lg_theme();
    if (!lg_display_lock(1000)) {
        return false;
    }
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    const int16_t screen[CAL_POINTS][2] = {
        { (int16_t)(w * 15 / 100), (int16_t)(h * 12 / 100) },
        { (int16_t)(w * 85 / 100), (int16_t)(h * 50 / 100) },
        { (int16_t)(w * 50 / 100), (int16_t)(h * 88 / 100) },
    };

    lv_obj_t *previous = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lg_theme_apply_screen(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_width(title, LV_PCT(80));
    lv_obj_set_style_text_font(title, t->font_title, 0);
    lv_obj_set_style_text_color(title, t->accent, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "Touch calibration");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, h * 24 / 100);
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_width(hint, LV_PCT(70));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, t->muted, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "");
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_MID, 0, t->gap);
    lv_obj_t *target = crosshair(scr);
    lv_screen_load(scr);
    lg_display_unlock();

    lg_ui_input_suspend(true);
    int16_t raw[CAL_POINTS][2];
    bool ok = true;
    char text[48];
    for (int i = 0; i < CAL_POINTS && ok; i++) {
        lg_display_lock(1000);
        lv_obj_set_pos(target, screen[i][0] - t->touch_min / 2, screen[i][1] - t->touch_min / 2);
        snprintf(text, sizeof(text), "Tap and hold the target, %d of %d", i + 1, CAL_POINTS);
        lv_label_set_text(hint, text);
        lg_display_unlock();
        ok = lg_bsp_touch_wait_press(&raw[i][0], &raw[i][1], CAL_TIMEOUT_MS);
        if (ok) {
            printf("[UI] calibration point %d: raw %d,%d for screen %d,%d\n", i + 1, raw[i][0], raw[i][1],
                   screen[i][0], screen[i][1]);
        }
    }
    esp_err_t err = ok ? lg_bsp_touch_set_calibration(raw, screen) : ESP_ERR_TIMEOUT;
    ok = !lg_bsp_touch_needs_calibration();   /* applied, even if it could not be saved */

    lg_display_lock(1000);
    lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title, ok ? "Calibration saved" : "Calibration failed");
    lv_obj_set_style_text_color(title, ok ? t->success : t->error, 0);
    lv_label_set_text(hint, ok ? "" : err == ESP_ERR_TIMEOUT ? "No touch for 30 seconds" : "Touch three different places");
    lg_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));

    lg_display_lock(1000);
    if (previous != NULL && lv_obj_is_valid(previous)) {
        lv_screen_load(previous);
        lv_obj_delete(scr);
    } else {
        /* The screen underneath was freed when this one replaced it (screens that exist only
         * while shown). Stay up until the caller loads the next screen, then go. */
        lv_obj_add_event_cb(scr, delete_when_left, LV_EVENT_SCREEN_UNLOADED, NULL);
    }
    lg_display_unlock();
    lg_ui_input_suspend(false);
    return ok;
}
