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
static lv_font_t s_huge_emoji;
static lv_font_t s_icon_emoji;

static const lv_font_t *with_emoji(lv_font_t *copy, const lv_font_t *base)
{
    *copy = *base;
    copy->fallback = &lg_font_emoji_20;
    return copy;
}

/*
 * The smallest size the theme offers, for a delivery marker beside a timestamp: at body size
 * two ticks and a clock crowd a bubble. It falls back to 12 px where Montserrat 10 was not
 * compiled in, so a build that has not enabled it still works, only less tidily.
 */
#if defined(LV_FONT_MONTSERRAT_10) && LV_FONT_MONTSERRAT_10
#define FONT_10 (&lv_font_montserrat_10)
#else
#define FONT_10 FONT_12
#endif

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
        /*
         * Marker colours (D42). Read is a violet, deliberately outside this palette's greens:
         * accent and success are the same value here, so two states drawn in "different"
         * greens were being drawn identically. Stepping out of the family is what keeps
         * delivered and read from collapsing into each other at marker size again.
         */
        .mark_wait      = lv_color_hex(0x7FA78F),
        .mark_node      = lv_color_hex(0x4A8FD4),
        .mark_delivered = lv_color_hex(0x5FD38D),
        .mark_read      = lv_color_hex(0xB98CFF),
        .radius   = 6,
    };

    uint16_t short_side = width < height ? width : height;
    bool compact = short_side <= 240;
    s_theme.font_tiny  = compact ? FONT_10 : FONT_12;
    s_theme.font_small = compact ? FONT_12 : &lv_font_montserrat_14;
    s_theme.font_body  = compact ? &lv_font_montserrat_14 : FONT_16;
    s_theme.font_title = FONT_20;
    s_theme.font_huge  = FONT_28;
    /* One icon size everywhere, so a control never looks bigger than its neighbour.
     * 20 px is also the emoji font's only size, so a drawn face matches a drawn glyph. */
    s_theme.font_icon  = FONT_20;
    s_theme.pad = (int16_t)(short_side / 30);    /* 8 px on a 240 px side */
    s_theme.gap = (int16_t)(short_side / 48);    /* 5 px on a 240 px side */
    s_theme.touch_min = (int16_t)((px_per_10mm * 8u) / 10u);
    s_theme.font_body = with_emoji(&s_body_emoji, s_theme.font_body);
    s_theme.font_small = with_emoji(&s_small_emoji, s_theme.font_small);
    /* The magnified keycap uses the largest font, and emoji keys have to show in it.
     * The emoji font is one 20 px size, so an emoji there is drawn at 20 px inside a
     * 28 px line: still larger than the key it magnifies. */
    s_theme.font_huge = with_emoji(&s_huge_emoji, s_theme.font_huge);
    s_theme.font_icon = with_emoji(&s_icon_emoji, s_theme.font_icon);
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

void lg_theme_style_button_filled(lv_obj_t *button, lv_color_t fill)
{
    lg_theme_style_button(button);
    lv_obj_set_style_bg_color(button, fill, 0);
    lv_obj_set_style_bg_color(button, lv_color_mix(fill, s_theme.bg, LV_OPA_70), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, fill, 0);
    lv_obj_set_style_text_color(button, s_theme.bg, 0);
}

#if LV_USE_TABVIEW
void lg_theme_style_tabview(lv_obj_t *tabview)
{
    lv_obj_t *bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(bar, s_theme.hairline * 2, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, s_theme.outline, 0);
    lv_obj_set_style_border_width(bar, s_theme.hairline, 0);

    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *tab = lv_obj_get_child(bar, (int32_t)i);
        lv_obj_set_style_bg_color(tab, s_theme.bar, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(tab, s_theme.muted, 0);
        lv_obj_set_style_border_color(tab, s_theme.outline, 0);
        lv_obj_set_style_border_width(tab, s_theme.hairline, 0);
        lv_obj_set_style_border_side(tab, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_radius(tab, s_theme.radius, 0);
        lv_obj_set_style_bg_color(tab, s_theme.outline, LV_STATE_PRESSED);

        lv_obj_set_style_bg_color(tab, s_theme.surface, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(tab, s_theme.bar_text, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(tab, s_theme.accent, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(tab, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT,
                                     LV_STATE_CHECKED);
        lv_obj_set_style_border_width(tab, s_theme.hairline, LV_STATE_CHECKED);
    }

    lv_obj_t *content = lv_tabview_get_content(tabview);
    for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
        lv_obj_t *page = lv_obj_get_child(content, (int32_t)i);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_pad_top(page, s_theme.gap, 0);
        lv_obj_set_style_bg_color(page, s_theme.outline, LV_PART_SCROLLBAR);
    }
}
#endif
