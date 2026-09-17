#include "ui_screen.h"

static void on_unloaded(lv_event_t *e)
{
    void (*forget)(void) = lv_event_get_user_data(e);
    lv_obj_t *screen = lv_event_get_target_obj(e);
    if (forget != NULL) {
        forget();
    }
    lv_obj_delete_async(screen);
}

void ui_screen_free_on_leave(lv_obj_t *screen, void (*forget)(void))
{
    lv_obj_add_event_cb(screen, on_unloaded, LV_EVENT_SCREEN_UNLOADED, forget);
}
