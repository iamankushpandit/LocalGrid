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
    LG_PANEL_ST7796,             /* 320x480 */
    LG_PANEL_JD9853,             /* 172x320 on the Waveshare ESP32-C6-Touch-LCD-1.47; ST7789-style addressing */
} lg_panel_kind_t;

typedef enum {
    LG_TOUCH_NONE = 0,
    LG_TOUCH_XPT2046_SPI,   /* resistive: on the display SPI bus with its own CS, or on a bus of its own */
    LG_TOUCH_FT6336_I2C,    /* capacitive */
    LG_TOUCH_AXS5106_I2C,   /* capacitive, the Waveshare ESP32-C6-Touch-LCD-1.47's */
} lg_touch_kind_t;

/*
 * What a board is for (D66). A handheld is the full messenger. An alert unit runs the same
 * firmware with the reduced screen set: alerts, SOS, and status only. Zero is a handheld, so
 * a profile that says nothing keeps the full UI.
 */
typedef enum {
    LG_ROLE_HANDHELD = 0,
    LG_ROLE_ALERT_UNIT,
} lg_board_role_t;

/*
 * Physical buttons (D66): a board's buttons and what each one does, so a builder can wire more
 * buttons without code changes. lg_bsp_button reports a press or a hold for a button's index in
 * this table; the screens choose the action from the bits below and what they are showing:
 * a short press does whichever of on_press applies there, a hold whichever of on_hold.
 */
#define LG_BOARD_BUTTONS_MAX 4

typedef enum {
    LG_ACT_SOS    = 1u << 0,    /* raise an SOS (hold, from the idle screen) */
    LG_ACT_SAFE   = 1u << 1,    /* "I'm safe" after an SOS (hold, from the SOS screen) */
    LG_ACT_CANCEL = 1u << 2,    /* cancel the SOS countdown */
    LG_ACT_READ   = 1u << 3,    /* read an alert, which dismisses it (D58) */
} lg_action_t;

typedef struct {
    int8_t  gpio;
    bool    active_low;         /* pressed reads 0 */
    bool    pull_up;            /* turn on the internal pull-up (active-low buttons without their own) */
    uint8_t on_press;           /* lg_action_t bits for a short press */
    uint8_t on_hold;            /* lg_action_t bits for a hold */
} lg_button_profile_t;

/*
 * Two boards sold under one name with different wiring (the Waveshare 1.47in ESP32-C6 with and
 * without touch). `probe` names a part only this board has on I2C: if a device answers at either
 * address on those pins, the board is this one. lg_bsp_board_resolve() uses it to switch to the
 * sibling profile when the device ID names the wrong one. Zero addresses: nothing to probe.
 */
typedef struct {
    int8_t  sda, scl;
    uint8_t addr[2];
} lg_board_probe_t;

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
    /* Where the glass starts in the controller's memory: a 172-wide panel on a 240-column
     * controller sits 34 columns in. Zero on panels that fill their controller. */
    uint8_t  x_gap;
    uint8_t  y_gap;
} lg_panel_profile_t;

typedef struct {
    lg_touch_kind_t kind;
    int8_t   cs;                 /* XPT2046 */
    /* XPT2046 wired to pins of its own rather than to the display's bus (the 2.8in LCDWIKI
     * boards and the CYD). mosi, miso, and sclk are read only when own_bus is set, so a
     * profile that leaves them out cannot claim GPIO0 by accident. */
    bool     own_bus;
    int8_t   mosi, miso, sclk;
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

typedef enum {
    LG_AUDIO_NONE = 0,       /* nothing this firmware can drive */
    LG_AUDIO_DAC,            /* classic ESP32 internal DAC into a small amplifier */
    LG_AUDIO_ES8311_I2S,     /* codec: I2C control on the touch bus, samples over I2S */
} lg_audio_kind_t;

typedef struct {
    lg_audio_kind_t kind;
    int8_t          speaker;        /* bare transducer on a DAC pin, or LG_PIN_NONE */
    uint8_t         codec_addr;     /* codec's address on the touch I2C bus, 0 if none */
    int8_t          i2s_mclk;       /* the five codec wires; LG_PIN_NONE on a DAC board */
    int8_t          i2s_bclk;
    int8_t          i2s_ws;
    int8_t          i2s_dout;
    int8_t          i2s_din;
    /*
     * The amplifier's enable line. Easy to miss and silent when missed: the speaker stays
     * mute however well the rest works, and on one board of this family the same pin was
     * declared as an LED, so switching the LED off switched the speaker off with it.
     */
    int8_t          amp_enable;
    bool            amp_active_low;
    uint8_t         max_volume;     /* ceiling set by listening, not from a datasheet */
} lg_audio_profile_t;

typedef struct {
    const char        *code;     /* three-character board code used in device IDs */
    const char        *name;
    lg_panel_profile_t panel;
    lg_touch_profile_t touch;
    lg_audio_profile_t audio;
    int8_t             boot_button;
    /* Supply sense: an ADC1 pin behind a resistor divider, or LG_PIN_NONE. Ratio in
     * thousandths (2000 = 2:1). The vendor's ratio, checked against a meter only where noted. */
    int8_t             supply_sense;
    uint16_t           supply_divider_milli;
    /* A GPS module's UART (D65): gps_rx takes the module's TXD, gps_tx goes to its RXD. LG_PIN_NONE
     * on a board with nowhere to wire one. Every profile sets both: a missing field would read as
     * GPIO0, the boot button. A pin here is a connector, not a promise that a module is fitted. */
    int8_t             gps_rx;
    int8_t             gps_tx;
    lg_board_role_t    role;     /* D66: a handheld, or an alert unit with the reduced screens */
    /* Buttons beyond the touch panel, n_buttons of them; rows past it are never read, so a
     * profile without buttons cannot claim GPIO0 by accident. */
    uint8_t            n_buttons;
    lg_button_profile_t buttons[LG_BOARD_BUTTONS_MAX];
    const char        *sibling;  /* code of a board sold under the same name, wired differently; NULL none */
    lg_board_probe_t   probe;    /* how to tell this board from its sibling (see above) */
} lg_board_t;

/* Returns the profile for a board code (e.g. "HY3"), or NULL if unknown. */
const lg_board_t *lg_board_find(const char *code);

static inline bool lg_board_is_alert_unit(const lg_board_t *b)
{
    return b != NULL && b->role == LG_ROLE_ALERT_UNIT;
}

/* Every action bit any of the board's buttons can do, pressed or held. */
static inline uint8_t lg_board_button_actions(const lg_board_t *b)
{
    uint8_t all = 0;
    for (uint8_t i = 0; b != NULL && i < b->n_buttons && i < LG_BOARD_BUTTONS_MAX; i++) {
        all = (uint8_t)(all | b->buttons[i].on_press | b->buttons[i].on_hold);
    }
    return all;
}

static inline bool lg_board_has_audio(const lg_board_t *b)
{
    return b != NULL && b->audio.kind != LG_AUDIO_NONE;
}

static inline bool lg_board_has_display(const lg_board_t *b)
{
    return b != NULL && b->panel.kind != LG_PANEL_NONE;
}

#ifdef __cplusplus
}
#endif
