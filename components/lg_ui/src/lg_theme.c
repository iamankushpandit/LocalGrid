#include "lg_theme.h"

#include "lg_emoji.h"

/* Fonts that are not enabled in Kconfig fall back to the always-on 14 px face. */
#if LV_FONT_MONTSERRAT_12
#define FONT_12 (&lv_font_montserrat_12)
#else
#define FONT_12 (&lv_font_montserrat_14)
#endif
#if LV_FONT_MONTSERRAT_16
#define FONT_16 (&lv_font_montserrat_16)
#else
#define FONT_16 (&lv_font_montserrat_14)
#endif
#if LV_FONT_MONTSERRAT_20
#define FONT_20 (&lv_font_montserrat_20)
#else
#define FONT_20 (&lv_font_montserrat_14)
#endif
#if LV_FONT_MONTSERRAT_28
#define FONT_28 (&lv_font_montserrat_28)
#else
#define FONT_28 FONT_20
#endif

static lg_theme_t s_theme;

/*
 * Text fonts that fall back to the emoji font: LVGL looks a missing glyph up in the
 * fallback, so one label can carry letters and emoji. These are copies of the built-in
 * fonts, which are const and shared, with only the fallback pointer changed.
 */
static lv_font_t s_body_emoji;
static lv_font_t s_small_emoji;

static const lv_font_t *with_emoji(lv_font_t *copy, const lv_font_t *base)
{
    *copy = *base;
    copy->fallback = &lg_font_emoji_20;
    return copy;
}

void lg_theme_init(uint16_t width, uint16_t height, uint16_t px_per_10mm)
{
    /* Terminal: the Phase 1 theme. More themes become more initialisers like this one. */
    s_theme = (lg_theme_t){
        .name     = "Terminal",
        .bg       = lv_color_hex(0x000000),
        .bar      = lv_color_hex(0x0B1A12),
        .bar_text = lv_color_hex(0x5FD38D),
        .surface  = lv_color_hex(0x0A140F),
        .outline  = lv_color_hex(0x1F3A2A),
        .text     = lv_color_hex(0xD2F5DE),
        .muted    = lv_color_hex(0x7FA78F),
        .accent   = lv_color_hex(0x5FD38D),
        .success  = lv_color_hex(0x5FD38D),
        .error    = lv_color_hex(0xFF6B6B),
        .warning  = lv_color_hex(0xF0B64A),
        .radius   = 6,
    };

    uint16_t short_side = width < height ? width : height;
    bool compact = short_side <= 240;
    s_theme.font_small = compact ? FONT_12 : &lv_font_montserrat_14;
    s_theme.font_body  = compact ? &lv_font_montserrat_14 : FONT_16;
    s_theme.font_title = FONT_20;
    s_theme.font_huge  = FONT_28;
    s_theme.pad = (int16_t)(short_side / 30);    /* 8 px on a 240 px side */
    s_theme.gap = (int16_t)(short_side / 48);    /* 5 px on a 240 px side */
    s_theme.touch_min = (int16_t)((px_per_10mm * 8u) / 10u);
    s_theme.font_body = with_emoji(&s_body_emoji, s_theme.font_body);
    s_theme.font_small = with_emoji(&s_small_emoji, s_theme.font_small);
    s_theme.stroke = (int16_t)(short_side / 120 > 2 ? short_side / 120 : 2);
    s_theme.hairline = (int16_t)(s_theme.stroke / 2);
}

const lg_theme_t *lg_theme(void)
{
    return &s_theme;
}

void lg_theme_apply_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, s_theme.bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, s_theme.text, 0);
    lv_obj_set_style_text_font(screen, s_theme.font_body, 0);
}

void lg_theme_style_button_small(lv_obj_t *button)
{
    lg_theme_style_button(button);
    lv_obj_set_style_text_font(button, s_theme.font_small, 0);
    lv_obj_set_style_pad_hor(button, s_theme.gap, 0);
    lv_obj_set_style_pad_ver(button, s_theme.gap / 2, 0);
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(button, s_theme.touch_min * 3 / 4, 0);
}

void lg_theme_style_button(lv_obj_t *button)
{
    lv_obj_set_style_bg_color(button, s_theme.surface, 0);
    lv_obj_set_style_bg_color(button, s_theme.outline, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, s_theme.accent, 0);
    lv_obj_set_style_border_width(button, s_theme.hairline, 0);
    lv_obj_set_style_radius(button, s_theme.radius, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_text_color(button, s_theme.text, 0);
    lv_obj_set_style_text_font(button, s_theme.font_body, 0);
}
