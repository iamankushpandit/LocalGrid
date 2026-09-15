/*
 * Board profiles.
 *
 * Provenance: pins, colour order, inversion, backlight polarity, and touch wiring for
 * the E32R32P (Hosyond 3.2in) and Freenove FNK0104B were measured on these exact
 * boards by the owner for Braino (github.com/iamankushpandit/Gume, commit 1e2a11f:
 * include/boards/e32r32p.h, include/boards/fnk0104b.h, and the TFT_eSPI flags in
 * platformio.ini). They are restated here as facts, not copied code.
 */
#include "lg_board.h"

#include <string.h>

static const lg_board_t BOARDS[] = {
    {
        .code = "ELG",
        .name = "Elegoo ESP32 dev board",
        .panel = { .kind = LG_PANEL_NONE },
        .touch = { .kind = LG_TOUCH_NONE },
        .boot_button = 0,
    },
    {
        /* LCDWIKI E32R32P, sold as Hosyond 3.2in. ST7789P3, BGR (measured: red and blue
         * swap with RGB), backlight GPIO27 active high. TFT_eSPI's ST7789 init turns
         * inversion on, and Braino adds none on top, so the panel runs inverted. */
        .code = "HY3",
        .name = "Hosyond 3.2in (E32R32P)",
        .panel = {
            .kind = LG_PANEL_ST7789,
            .native_width = 240, .native_height = 320,
            .mosi = 13, .miso = 12, .sclk = 14, .cs = 15, .dc = 2, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = true, .mirror_x = false, .mirror_y = false,
            .backlight = 27, .backlight_active_high = true,
            .px_per_10mm = 49,   /* 3.2in diagonal, 240x320: about 125 px per inch */
        },
        .touch = {
            .kind = LG_TOUCH_XPT2046_SPI,
            .cs = 33, .irq = 36, .irq_usable = false,   /* GPIO36 has no pull-up on this board */
            .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE,
            .pressure_threshold = 350,
        },
        .boot_button = 0,
    },
    {
        /* Freenove FNK0104B. ILI9341 with TFT_eSPI's ILI9341_2 sequence, BGR, inversion
         * on (TFT_INVERSION_ON), backlight GPIO45 active high. TFT_eSPI's portrait
         * rotation for ILI9341 sets the horizontal mirror. */
        .code = "F4B",
        .name = "Freenove FNK0104B 2.8in",
        .panel = {
            .kind = LG_PANEL_ILI9341,
            .native_width = 240, .native_height = 320,
            .mosi = 11, .miso = 13, .sclk = 12, .cs = 10, .dc = 46, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = true, .mirror_x = true, .mirror_y = false,
            .backlight = 45, .backlight_active_high = true,
            .px_per_10mm = 56,   /* 2.8in diagonal, 240x320: about 143 px per inch */
        },
        .touch = {
            .kind = LG_TOUCH_FT6336_I2C,
            .cs = LG_PIN_NONE, .irq = 17, .irq_usable = true,
            .sda = 16, .scl = 15, .rst = 18, .i2c_addr = 0x38,
            /* Braino's hardware-verified FT6336 transform (native to landscape, then
             * TFT_eSPI rotation 0) reduces to no swap and no mirror in portrait. */
            .swap_xy = false, .mirror_x = false, .mirror_y = false,
        },
        .boot_button = 0,
    },
};

const lg_board_t *lg_board_find(const char *code)
{
    if (code == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++) {
        if (strcmp(BOARDS[i].code, code) == 0) {
            return &BOARDS[i];
        }
    }
    return NULL;
}
