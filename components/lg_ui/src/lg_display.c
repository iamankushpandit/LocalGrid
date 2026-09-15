/*
 * Panel bring-up on esp_lcd and LVGL integration.
 *
 * Both panels use ESP-IDF's built-in ST7789 panel object for addressing and pixel
 * transfer. Controller-specific power and gamma registers are sent first, from the
 * command sequences TFT_eSPI uses for these controllers (TFT_Drivers/ST7789_Init.h
 * and the ILI9341_2 branch of ILI9341_Init.h), which Braino runs on these boards.
 */
#include "lg_display.h"
#include "lg_touch.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "UI";

#define DRAW_LINES          24
#define LCD_HOST            SPI2_HOST
#define LVGL_TASK_STACK     6144
#define LVGL_TASK_PRIORITY  3

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[15];
} panel_cmd_t;

static const panel_cmd_t ILI9341_2_INIT[] = {
    { 0xCF, 3, { 0x00, 0xC1, 0x30 } },
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 } },
    { 0xE8, 3, { 0x85, 0x00, 0x78 } },
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 } },
    { 0xF7, 1, { 0x20 } },
    { 0xEA, 2, { 0x00, 0x00 } },
    { 0xC0, 1, { 0x10 } },                         /* power control 1 */
    { 0xC1, 1, { 0x00 } },                         /* power control 2 */
    { 0xC5, 2, { 0x30, 0x30 } },                   /* VCOM control 1 */
    { 0xC7, 1, { 0xB7 } },                         /* VCOM control 2 */
    { 0xB1, 2, { 0x00, 0x1A } },                   /* frame rate */
    { 0xB6, 3, { 0x08, 0x82, 0x27 } },             /* display function control */
    { 0xF2, 1, { 0x00 } },                         /* 3-gamma off */
    { 0x26, 1, { 0x01 } },                         /* gamma curve */
    { 0xE0, 15, { 0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9, 0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00 } },
    { 0xE1, 15, { 0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F } },
};

static const panel_cmd_t ST7789_INIT[] = {
    { 0x13, 0, { 0 } },                            /* normal display mode */
    { 0xB6, 2, { 0x0A, 0x82 } },
    { 0xB0, 2, { 0x00, 0xE0 } },                   /* RAM control */
    { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 } }, /* porch */
    { 0xB7, 1, { 0x35 } },                         /* gate control */
    { 0xBB, 1, { 0x28 } },                         /* VCOM */
    { 0xC0, 1, { 0x0C } },                         /* LCM control */
    { 0xC2, 2, { 0x01, 0xFF } },
    { 0xC3, 1, { 0x10 } },                         /* VRH */
    { 0xC4, 1, { 0x20 } },                         /* VDV */
    { 0xC6, 1, { 0x0F } },                         /* frame rate */
    { 0xD0, 2, { 0xA4, 0xA1 } },                   /* power control 1 */
    { 0xE0, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17 } },
    { 0xE1, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E } },
};

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t      s_lock;

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

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)disp;
    lv_draw_sw_rgb565_swap(px, (uint32_t)lv_area_get_size(area));   /* the panel takes big-endian RGB565 */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
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

static void send_table(esp_lcd_panel_io_handle_t io, const panel_cmd_t *table, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        esp_lcd_panel_io_tx_param(io, table[i].cmd, table[i].len ? table[i].data : NULL, table[i].len);
    }
}

static void backlight(const lg_panel_profile_t *p, bool on)
{
    if (p->backlight != LG_PIN_NONE) {
        gpio_set_level((gpio_num_t)p->backlight, on == p->backlight_active_high ? 1 : 0);
    }
}

esp_err_t lg_display_start(const lg_board_t *board, lg_display_t *out)
{
    if (!lg_board_has_display(board)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const lg_panel_profile_t *p = &board->panel;

    if (p->backlight != LG_PIN_NONE) {
        gpio_config_t bl = { .pin_bit_mask = 1ULL << p->backlight, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&bl);
        backlight(p, false);
    }

    uint32_t line_bytes = (uint32_t)p->native_width * 2u;
    spi_bus_config_t bus = {
        .mosi_io_num = p->mosi,
        .miso_io_num = p->miso,
        .sclk_io_num = p->sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(line_bytes * DRAW_LINES + 16),
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    /* Touch first: an XPT2046 on the shared bus gets its chip select driven high before
     * the panel is talked to. A missing controller leaves the screen usable. */
    if (board->touch.kind != LG_TOUCH_NONE && lg_touch_start(board, LCD_HOST) != ESP_OK) {
        ESP_LOGW(TAG, "[UI] Touch controller not responding on %s", board->name);
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = (gpio_num_t)p->cs,
        .dc_gpio_num = (gpio_num_t)p->dc,
        .spi_mode = 0,
        .pclk_hz = p->spi_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = (gpio_num_t)p->rst,
        .rgb_ele_order = p->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    /* Same order as TFT_eSPI: ILI9341 registers go in before sleep out, ST7789 registers after. */
    if (p->kind == LG_PANEL_ILI9341) {
        send_table(io, ILI9341_2_INIT, sizeof(ILI9341_2_INIT) / sizeof(ILI9341_2_INIT[0]));
        esp_lcd_panel_init(s_panel);   /* sleep out, memory access control, pixel format */
    } else {
        esp_lcd_panel_init(s_panel);
        send_table(io, ST7789_INIT, sizeof(ST7789_INIT) / sizeof(ST7789_INIT[0]));
    }
    esp_lcd_panel_invert_color(s_panel, p->invert);
    esp_lcd_panel_mirror(s_panel, p->mirror_x, p->mirror_y);

    /* Clear to black before the backlight comes on, so no power-on noise is visible. */
    uint8_t *line = heap_caps_calloc(line_bytes * DRAW_LINES, 1, MALLOC_CAP_DMA);
    if (line == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t y = 0; y < p->native_height; y += DRAW_LINES) {
        uint16_t h = (uint16_t)((p->native_height - y) < DRAW_LINES ? (p->native_height - y) : DRAW_LINES);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, p->native_width, y + h, line);
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    backlight(p, true);

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        heap_caps_free(line);
        return ESP_ERR_NO_MEM;
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(p->native_width, p->native_height);
    lv_display_set_buffers(disp, line, NULL, line_bytes * DRAW_LINES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_done };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);
    lg_touch_attach(disp);

    out->display = disp;
    out->width = (uint16_t)lv_display_get_horizontal_resolution(disp);
    out->height = (uint16_t)lv_display_get_vertical_resolution(disp);
    out->px_per_10mm = p->px_per_10mm;

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[UI] %s: %ux%u, backlight on", board->name, out->width, out->height);
    return ESP_OK;
}
