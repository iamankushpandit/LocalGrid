/*
 * lg_board.h - hardware profiles (decisions D9, D23).
 *
 * Pixel sizes, pins, and panel quirks exist only in these profiles. Firmware finds
 * the profile at boot from the board code in the device ID (lg_identity.h), so one
 * binary serves every board with the same chip.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LG_PIN_NONE (-1)

typedef enum {
    LG_PANEL_NONE = 0,
    LG_PANEL_ILI9341,
    LG_PANEL_ST7789,
} lg_panel_kind_t;

typedef enum {
    LG_TOUCH_NONE = 0,
    LG_TOUCH_XPT2046_SPI,   /* resistive, shares the display SPI bus with its own CS */
    LG_TOUCH_FT6336_I2C,    /* capacitive */
} lg_touch_kind_t;

typedef struct {
    lg_panel_kind_t kind;
    uint16_t native_width;       /* panel in its native orientation, which is portrait on every board so far */
    uint16_t native_height;
    int8_t   mosi, miso, sclk, cs, dc, rst;
    uint32_t spi_hz;
    bool     bgr;                /* colour element order */
    bool     invert;             /* display inversion on */
    bool     mirror_x;           /* portrait needs a horizontal mirror */
    bool     mirror_y;
    int8_t   backlight;
    bool     backlight_active_high;
    uint16_t px_per_10mm;        /* pixel density, used to size touch targets in millimetres */
} lg_panel_profile_t;

typedef struct {
    lg_touch_kind_t kind;
    int8_t   cs;                 /* XPT2046 */
    int8_t   irq;
    bool     irq_usable;
    int8_t   sda, scl, rst;      /* FT6336 */
    uint8_t  i2c_addr;
    uint16_t pressure_threshold; /* XPT2046 */
    /* Capacitive controllers report the panel's native frame; these map it onto the
     * portrait screen. Resistive panels are mapped by calibration instead. */
    bool     swap_xy;
    bool     mirror_x;
    bool     mirror_y;
} lg_touch_profile_t;

typedef struct {
    const char        *code;     /* three-character board code used in device IDs */
    const char        *name;
    lg_panel_profile_t panel;
    lg_touch_profile_t touch;
    int8_t             boot_button;
} lg_board_t;

/* Returns the profile for a board code (e.g. "HY3"), or NULL if unknown. */
const lg_board_t *lg_board_find(const char *code);

static inline bool lg_board_has_display(const lg_board_t *b)
{
    return b != NULL && b->panel.kind != LG_PANEL_NONE;
}

#ifdef __cplusplus
}
#endif
