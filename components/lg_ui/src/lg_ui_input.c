#include "lg_ui_input.h"

#include "lg_bsp_touch.h"

static volatile bool s_suspended;
static int16_t       s_last_x;
static int16_t       s_last_y;

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    lg_bsp_touch_raw_t raw;
    int16_t x = 0;
    int16_t y = 0;
    bool down = !s_suspended && lg_bsp_touch_read_raw(&raw) && lg_bsp_touch_map(&raw, &x, &y);
    if (down) {
        s_last_x = x;
        s_last_y = y;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
