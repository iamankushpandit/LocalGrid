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
        .supply_sense = LG_PIN_NONE,   /* no divider on this dev board; wire one to an ADC1 pin to measure */
        .gps_rx = LG_PIN_NONE,
        .gps_tx = LG_PIN_NONE,
    },
    {
        /* LCDWIKI E32R32P, sold as Hosyond 3.2in. ST7789P3, BGR (measured: red and blue
         * swap with RGB), backlight GPIO27 active high. TFT_eSPI's ST7789 init turns
         * inversion on, and Braino adds none on top, so the panel runs inverted. */
        .code = "HY3",
        .name = "Hosyond 3.2in (E32R32P)",
        /* GPIO26 is DAC channel 1 on a classic ESP32, and the amplifier behind it is enabled
         * by GPIO4 held LOW. Both come from the vendor's own E32R32P pin table rather than
         * from a sibling board, and nothing else in this profile claims either pin. The 75
         * volume ceiling is inherited from the rest of that board family and has not been set
         * by listening on this one, so treat it as a starting point. */
        .audio = { .kind = LG_AUDIO_DAC, .speaker = 26, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = 4, .amp_active_low = true, .max_volume = 75 },
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
        /* Braino (E32R32P): IO34, ADC1_CH6, behind the 2:1 divider the vendor manual states. With
         * no battery fitted it reads the charger output, which follows USB. */
        .supply_sense = 34,
        .supply_divider_milli = 2000,
        .gps_rx = LG_PIN_NONE,   /* UART0 is this board's console, and no free connector carries a UART */
        .gps_tx = LG_PIN_NONE,
    },
    {
        /* Freenove FNK0104B. ILI9341 with TFT_eSPI's ILI9341_2 sequence, BGR, inversion
         * on (TFT_INVERSION_ON), backlight GPIO45 active high. TFT_eSPI's portrait
         * rotation for ILI9341 sets the horizontal mirror. */
        .code = "F4B",
        .name = "Freenove FNK0104B 2.8in",
        /* This board has an ES8311 codec, a microphone and a speaker, and the ESP32-S3 has no
         * DAC, so every sound goes through the codec over I2S. There is no bare speaker pin
         * and there must not be one: the transducer hangs off the codec's amplifier.
         *
         * The wiring below was established on this board rather than inferred, and three
         * parts of it are easy to get wrong: the amplifier enable is active LOW, MCLK is 384
         * times the sample rate (6.144 MHz at 16 kHz), and the codec's DAC volume register
         * resets to silence. The S3 has no audio PLL, so MCLK comes from the PLL through the
         * I2S fractional divider. None of these pins touch GPIO33 to 37,
         * which are the octal PSRAM lines here and must never be assigned.
         * lg_bsp_audio.c drives it. */
        .audio = { .kind = LG_AUDIO_ES8311_I2S, .speaker = LG_PIN_NONE, .codec_addr = 0x18,
                   .i2s_mclk = 4, .i2s_bclk = 5, .i2s_ws = 7, .i2s_dout = 8, .i2s_din = 6,
                   .amp_enable = 1, .amp_active_low = true, .max_volume = 85 },
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
        /* Braino (FNK0104B): GPIO9, ADC1_CH8, x2.0 as Freenove's sketch states. Braino read
         * 4.09-4.16 V here on USB with no pack fitted. */
        .supply_sense = 9,
        .supply_divider_milli = 2000,
        /* D65: the 4-pin "UART" connector (RXD, TXD, GND, 5V). The console is USB-Serial-JTAG, so
         * these are free: board RXD is GPIO44 (the GPS module's TXD goes here), board TXD GPIO43.
         * The owner wired a GT-U7 here on device 1. */
        .gps_rx = 44,
        .gps_tx = 43,
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
