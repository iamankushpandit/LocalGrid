/*
 * LVGL on the board's panel. Pixels go through lg_bsp_display; this file owns only
 * the LVGL display, its task, and the lock.
 */
#include "lg_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_bsp_display.h"
#include "lg_ui_input.h"

static const char *TAG = "UI";

/*
 * 6144 was not enough, and the panic proved it on both boards rather than one. A decoded
 * backtrace from the Hosyond shows why, and it is structural rather than a fat frame:
 * lv_timer_handler -> lv_display_refr_timer -> refr_invalid_areas -> refr_area ->
 * refr_configured_layer -> refr_obj_and_children, then lv_obj_redraw and lv_obj_refr
 * recursing down the tree, and at the bottom LVGL dispatches the draw INLINE on the same
 * stack -- lv_draw_finalize_task_creation -> lv_draw_dispatch -> lv_draw_dispatch_layer ->
 * lv_draw_sw_label -> iterate_characters -> draw_letter_cb -> blend_color_to_rgb565. A glyph
 * blend therefore runs about nine frames beneath a tree walk that is already several deep.
 *
 * Measured with `status` reporting the high-water mark, and `screen settings` to reach the
 * deepest tree from serial:
 *
 *   launcher  4992 bytes peak (FNK0104B), 4768 (Hosyond)
 *   Settings  6544 bytes peak (Hosyond)
 *
 * So Settings needed about 400 bytes MORE than the old 6144 ever had, while the launcher
 * fitted with 1.1 to 1.4 KB spare. That is the whole crash: the launcher drew, and opening
 * Settings walked past the end.
 *
 * 10240 rather than 8192, which would leave only about 1.6 KB over the measured peak -- the
 * same thin margin that just failed. Chat with bubbles on screen is still unmeasured, and the
 * delivery marker adds a nested row and a second label to every bubble, so the deepest screen
 * in this firmware is probably not the one that has been measured yet.
 */
#define LVGL_TASK_STACK     10240
#define LVGL_TASK_PRIORITY  3

static SemaphoreHandle_t s_lock;
/*
 * Kept so the task's remaining stack can be read. A 6 KB stack running LVGL's layout
 * recursion inside timer callbacks overflowed on one board and not the other, and there was
 * no way to tell how close the survivor was: the panic says a stack overflowed, never how
 * much headroom the working case had.
 */
static TaskHandle_t s_lvgl_task;

uint32_t lg_display_stack_headroom(void)
{
    if (s_lvgl_task == NULL) {
        return 0;   /* asked before the display started; 0 means "no reading", not "no room" */
    }
    /* FreeRTOS reports the high-water mark in words on this port, not bytes. */
    return (uint32_t)uxTaskGetStackHighWaterMark(s_lvgl_task) * sizeof(StackType_t);
}

bool lg_display_lock(uint32_t timeout_ms)
{
    return s_lock != NULL && xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lg_display_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

static void flush_done(void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)disp;
    lv_draw_sw_rgb565_swap(px, (uint32_t)lv_area_get_size(area));   /* the panel takes big-endian RGB565 */
    lg_bsp_display_draw(area->x1, area->y1, area->x2, area->y2, px);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t wait = 20;
        if (lg_display_lock(100)) {
            wait = lv_timer_handler();
            lg_display_unlock();
        }
        if (wait < 5) {
            wait = 5;
        } else if (wait > 50) {
            wait = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

esp_err_t lg_display_start(const lg_board_t *board, lg_display_t *out)
{
    if (!lg_board_has_display(board)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(board->panel.native_width, board->panel.native_height);
    esp_err_t err = lg_bsp_display_start(board, flush_done, disp);
    if (err != ESP_OK) {
        return err;
    }
    size_t buffer_bytes = 0;
    uint8_t *buffer = lg_bsp_display_buffer(&buffer_bytes);
    lv_display_set_buffers(disp, buffer, NULL, (uint32_t)buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lg_ui_input_attach(disp);

    out->display = disp;
    out->width = (uint16_t)lv_display_get_horizontal_resolution(disp);
    out->height = (uint16_t)lv_display_get_vertical_resolution(disp);
    out->px_per_10mm = board->panel.px_per_10mm;

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIORITY, &s_lvgl_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[UI] %s: %ux%u, backlight on", board->name, out->width, out->height);
    return ESP_OK;
}
