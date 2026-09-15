/*
 * Touch controller drivers.
 *
 * Provenance: FT6336U register use, the XPT2046 pressure formula and threshold, and
 * the affine calibration follow Braino (github.com/iamankushpandit/Gume, commit
 * 1e2a11f, src/hal/BoardTouch.cpp), measured on these boards by the owner.
 */
#include "lg_bsp_touch.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "BSP";

#define CAL_NAMESPACE     "lgui"
#define CAL_KEY           "tcal"
#define CAL_MAGIC         0x3154474CUL   /* "LGT1" */

#define FT_REG_TD_STATUS  0x02           /* followed directly by the first touch point */
#define FT_I2C_HZ         400000

#define XPT_SPI_HZ        1000000
#define XPT_CMD_X         0xD0
#define XPT_CMD_Y         0x90
#define XPT_CMD_Z1        0xB0
#define XPT_CMD_Z2        0xC0
#define XPT_SAMPLES       4

typedef struct {
    uint32_t magic;
    char     board[4];
    float    ax, bx, cx, ay, by, cy;
} touch_cal_t;

static const lg_board_t       *s_board;
static SemaphoreHandle_t       s_lock;
static i2c_master_dev_handle_t s_ft;
static spi_device_handle_t     s_xpt;
static touch_cal_t             s_cal;
static bool                    s_calibrated;

static esp_err_t ft_start(const lg_touch_profile_t *t)
{
    if (t->rst != LG_PIN_NONE) {
        gpio_config_t rst = { .pin_bit_mask = 1ULL << t->rst, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&rst);
        gpio_set_level((gpio_num_t)t->rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)t->rst, 1);
        vTaskDelay(pdMS_TO_TICKS(300));   /* controller start-up */
    }
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)t->sda,
        .scl_io_num = (gpio_num_t)t->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_probe(bus, t->i2c_addr, 50);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = t->i2c_addr,
        .scl_speed_hz = FT_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &s_ft);
}

static esp_err_t xpt_start(const lg_touch_profile_t *t, int spi_host)
{
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = XPT_SPI_HZ,
        .spics_io_num = t->cs,
        .queue_size = 1,
    };
    return spi_bus_add_device((spi_host_device_t)spi_host, &dev_cfg, &s_xpt);
}

static uint16_t xpt_sample(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0, 0 };
    uint8_t rx[3] = { 0 };
    spi_transaction_t tr = { .length = 24, .tx_buffer = tx, .rx_buffer = rx };
    if (spi_device_polling_transmit(s_xpt, &tr) != ESP_OK) {
        return 0;
    }
    return (uint16_t)(((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFF);
}

static uint16_t xpt_pressure(void)
{
    uint16_t z1 = xpt_sample(XPT_CMD_Z1);
    uint16_t z2 = xpt_sample(XPT_CMD_Z2);
    return (z1 > 0 && z2 > z1) ? (uint16_t)(z1 + 4095 - z2) : 0;
}

static bool read_raw_locked(lg_bsp_touch_raw_t *out)
{
    const lg_touch_profile_t *t = &s_board->touch;
    if (t->kind == LG_TOUCH_FT6336_I2C) {
        uint8_t reg = FT_REG_TD_STATUS;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(s_ft, &reg, 1, buf, sizeof(buf), 20) != ESP_OK) {
            return false;
        }
        uint8_t points = buf[0] & 0x0F;
        if (points == 0 || points > 2) {
            return false;
        }
        out->x = (int16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
        out->y = (int16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
        out->pressure = UINT16_MAX;   /* capacitive contact is binary */
        out->down = true;
        return true;
    }
    if (t->kind == LG_TOUCH_XPT2046_SPI) {
        /* GPIO36 has no pull-up on the Hosyond, so pressure alone decides a press. */
        if (xpt_pressure() < t->pressure_threshold) {
            return false;
        }
        (void)xpt_sample(XPT_CMD_X);   /* first conversion after a pressure read settles */
        uint32_t sx = 0;
        uint32_t sy = 0;
        for (int i = 0; i < XPT_SAMPLES; i++) {
            sx += xpt_sample(XPT_CMD_X);
            sy += xpt_sample(XPT_CMD_Y);
        }
        uint16_t pressure = xpt_pressure();
        if (pressure < t->pressure_threshold) {
            return false;   /* lifted while sampling */
        }
        out->x = (int16_t)(sx / XPT_SAMPLES);
        out->y = (int16_t)(sy / XPT_SAMPLES);
        out->pressure = pressure;
        out->down = true;
        return true;
    }
    return false;
}

bool lg_bsp_touch_read_raw(lg_bsp_touch_raw_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool down = read_raw_locked(out);
    xSemaphoreGive(s_lock);
    return down;
}

static int16_t clamp(float v, uint16_t size)
{
    long r = lroundf(v);
    if (r < 0) {
        return 0;
    }
    return (int16_t)(r >= size ? size - 1 : r);
}

bool lg_bsp_touch_map(const lg_bsp_touch_raw_t *raw, int16_t *x, int16_t *y)
{
    if (s_board == NULL || !raw->down) {
        return false;
    }
    const lg_panel_profile_t *p = &s_board->panel;
    const lg_touch_profile_t *t = &s_board->touch;
    float fx;
    float fy;
    if (t->kind == LG_TOUCH_XPT2046_SPI) {
        if (!s_calibrated) {
            return false;
        }
        fx = s_cal.ax * raw->x + s_cal.bx * raw->y + s_cal.cx;
        fy = s_cal.ay * raw->x + s_cal.by * raw->y + s_cal.cy;
    } else {
        fx = t->swap_xy ? raw->y : raw->x;
        fy = t->swap_xy ? raw->x : raw->y;
        if (t->mirror_x) {
            fx = (float)(p->native_width - 1) - fx;
        }
        if (t->mirror_y) {
            fy = (float)(p->native_height - 1) - fy;
        }
    }
    *x = clamp(fx, p->native_width);
    *y = clamp(fy, p->native_height);
    return true;
}

static void load_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open(CAL_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    touch_cal_t cal;
    size_t len = sizeof(cal);
    if (nvs_get_blob(h, CAL_KEY, &cal, &len) == ESP_OK && len == sizeof(cal) && cal.magic == CAL_MAGIC &&
        strncmp(cal.board, s_board->code, sizeof(cal.board)) == 0) {
        s_cal = cal;
        s_calibrated = true;
    }
    nvs_close(h);
}

esp_err_t lg_bsp_touch_start(const lg_board_t *board, int spi_host)
{
    if (board == NULL || board->touch.kind == LG_TOUCH_NONE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_board = board;
    esp_err_t err = board->touch.kind == LG_TOUCH_FT6336_I2C ? ft_start(&board->touch)
                                                             : xpt_start(&board->touch, spi_host);
    if (err != ESP_OK) {
        s_board = NULL;
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        s_board = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (board->touch.kind == LG_TOUCH_XPT2046_SPI) {
        load_calibration();
    }
    ESP_LOGI(TAG, "[UI] Touch %s ready%s", board->touch.kind == LG_TOUCH_FT6336_I2C ? "FT6336U" : "XPT2046",
             lg_bsp_touch_needs_calibration() ? ", needs calibration" : "");
    return ESP_OK;
}

bool lg_bsp_touch_present(void)
{
    return s_board != NULL;
}

bool lg_bsp_touch_can_calibrate(void)
{
    return s_board != NULL && s_board->touch.kind == LG_TOUCH_XPT2046_SPI;
}

bool lg_bsp_touch_needs_calibration(void)
{
    return lg_bsp_touch_can_calibrate() && !s_calibrated;
}

bool lg_bsp_touch_wait_press(int16_t *raw_x, int16_t *raw_y, uint32_t timeout_ms)
{
    int64_t deadline = timeout_ms ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : INT64_MAX;
    lg_bsp_touch_raw_t raw;
    while (esp_timer_get_time() < deadline) {
        if (!lg_bsp_touch_read_raw(&raw)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int32_t sx = 0;
        int32_t sy = 0;
        int n = 0;
        for (int i = 0; i < 10; i++) {
            if (lg_bsp_touch_read_raw(&raw)) {
                sx += raw.x;
                sy += raw.y;
                n++;
            }
            vTaskDelay(pdMS_TO_TICKS(12));
        }
        if (n < 6) {
            continue;   /* a brush, not a press */
        }
        for (int released = 0; released < 3 && esp_timer_get_time() < deadline;) {
            released = lg_bsp_touch_read_raw(&raw) ? 0 : released + 1;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (raw_x != NULL) {
            *raw_x = (int16_t)(sx / n);
        }
        if (raw_y != NULL) {
            *raw_y = (int16_t)(sy / n);
        }
        return true;
    }
    return false;
}

esp_err_t lg_bsp_touch_set_calibration(const int16_t raw[3][2], const int16_t screen[3][2])
{
    if (!lg_bsp_touch_can_calibrate()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const float x0 = raw[0][0], y0 = raw[0][1];
    const float x1 = raw[1][0], y1 = raw[1][1];
    const float x2 = raw[2][0], y2 = raw[2][1];
    const float denom = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
    /* Readings this close together mean the panel was not really touched at three places. */
    if (fabsf(denom) < 1000.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    const float sx0 = screen[0][0], sy0 = screen[0][1];
    const float sx1 = screen[1][0], sy1 = screen[1][1];
    const float sx2 = screen[2][0], sy2 = screen[2][1];

    touch_cal_t cal = { .magic = CAL_MAGIC };
    size_t code_len = strnlen(s_board->code, sizeof(cal.board) - 1);
    memcpy(cal.board, s_board->code, code_len);
    cal.ax = (sx0 * (y1 - y2) + sx1 * (y2 - y0) + sx2 * (y0 - y1)) / denom;
    cal.bx = (sx0 * (x2 - x1) + sx1 * (x0 - x2) + sx2 * (x1 - x0)) / denom;
    cal.cx = (sx0 * (x1 * y2 - x2 * y1) + sx1 * (x2 * y0 - x0 * y2) + sx2 * (x0 * y1 - x1 * y0)) / denom;
    cal.ay = (sy0 * (y1 - y2) + sy1 * (y2 - y0) + sy2 * (y0 - y1)) / denom;
    cal.by = (sy0 * (x2 - x1) + sy1 * (x0 - x2) + sy2 * (x1 - x0)) / denom;
    cal.cy = (sy0 * (x1 * y2 - x2 * y1) + sy1 * (x2 * y0 - x0 * y2) + sy2 * (x0 * y1 - x1 * y0)) / denom;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CAL_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, CAL_KEY, &cal, sizeof(cal));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    s_cal = cal;
    s_calibrated = true;   /* use it this boot even if saving failed */
    ESP_LOGI(TAG, "[UI] Touch calibration %s", err == ESP_OK ? "saved" : "applied but not saved");
    return err;
}
