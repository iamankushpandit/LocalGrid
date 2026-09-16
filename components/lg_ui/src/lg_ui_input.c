#include "lg_ui_input.h"

#include "lg_bsp_touch.h"

static volatile bool s_suspended;
static int16_t       s_last_x;
static int16_t       s_last_y;

/*
 * A press has to be seen twice before it counts, and so does a release.
 *
 * One sample used to decide both, which made this the least sceptical reader of the panel in
 * the firmware: the calibration screen in the same driver asks for six agreeing samples out
 * of ten before it believes a press, and three consecutive misses before it believes a lift.
 * The path that drives every real tap asked for nothing, so a single noisy conversion on the
 * resistive panel became a genuine LVGL click -- which is how a notification banner can be
 * "tapped" with nobody in the room -- and one dropped sample mid-press became a released key.
 *
 * Two polls is about 30 ms at LVGL's rate, far below noticing, and no finger is on the glass
 * for less than that. Coordinates still only move while a sample says down, so a spurious
 * miss cannot drag the pointer somewhere else.
 */
#define TOUCH_CONFIRM 2

static uint8_t s_down_run;
static uint8_t s_up_run;
static bool    s_reported_down;

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    lg_bsp_touch_raw_t raw;
    int16_t x = 0;
    int16_t y = 0;
    bool sample = !s_suspended && lg_bsp_touch_read_raw(&raw) && lg_bsp_touch_map(&raw, &x, &y);
    if (sample) {
        s_last_x = x;
        s_last_y = y;
        s_up_run = 0;
        if (s_down_run < TOUCH_CONFIRM) {
            s_down_run++;
        }
    } else {
        s_down_run = 0;
        if (s_up_run < TOUCH_CONFIRM) {
            s_up_run++;
        }
    }
    if (!s_reported_down && s_down_run >= TOUCH_CONFIRM) {
        s_reported_down = true;
    } else if (s_reported_down && s_up_run >= TOUCH_CONFIRM) {
        s_reported_down = false;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_reported_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void lg_ui_input_attach(lv_display_t *display)
{
    if (!lg_bsp_touch_present()) {
        return;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read);
    lv_indev_set_display(indev, display);
}

void lg_ui_input_suspend(bool suspended)
{
    s_suspended = suspended;
}
