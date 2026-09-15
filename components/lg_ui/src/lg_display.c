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

#define LVGL_TASK_STACK     6144
#define LVGL_TASK_PRIORITY  3

static SemaphoreHandle_t s_lock;

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

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[UI] %s: %ux%u, backlight on", board->name, out->width, out->height);
    return ESP_OK;
}
