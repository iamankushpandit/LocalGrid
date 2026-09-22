/*
 * The two themes and the choice between them: see ui_theme.h.
 */
#include "ui_theme.h"

#include "esp_log.h"
#include "lg_bsp_settings.h"
#include "ui_nav.h"

#define SETTING_KEY "daylight"

/*
 * lg_rgb is a function, so a theme cannot be a static initialiser: both tables are filled by
 * build() before the first screen is drawn. Until then ui_theme points at a table of zeros, which
 * draws black on black rather than dereferencing nothing.
 */
static ui_theme_t s_night;
static ui_theme_t s_daylight;
static ui_theme_kind_t s_kind = UI_THEME_NIGHT;

const ui_theme_t *ui_theme = &s_night;

/*
 * Built at runtime because lg_rgb is a function: a theme is a table of roles, and the values here
 * are the only place a colour is written down (D10).
 */
static void build(void)
{
    s_night = (ui_theme_t){
        .name       = "Night",
        .bg         = lg_rgb(0x000000),
        .bar        = lg_rgb(0x0B1A12),
        .surface    = lg_rgb(0x0A140F),
        .outline    = lg_rgb(0x1F3A2A),
        .text       = lg_rgb(0xD2F5DE),
        .muted      = lg_rgb(0x7FA78F),
        .accent     = lg_rgb(0x5FD38D),
        .accent_ink = lg_rgb(0x06120C),
        .warning    = lg_rgb(0xF0B64A),
        .error      = lg_rgb(0xFF6B6B),
        .mark_wait  = lg_rgb(0x7FA78F),
        .mark_node  = lg_rgb(0x4A8FD4),
        .mark_deliv = lg_rgb(0x5FD38D),
        .mark_read  = lg_rgb(0xB98CFF),
    };
    /*
     * For sunlight: black on white and nothing else (owner, 2026-09-21: "White theme should be
     * white and black for contrast, not white and green"). Colour is what fails first outdoors -
     * a tinted ink loses contrast against a bright reflection long before a black one does - so
     * this theme spends its whole contrast budget on light against dark and distinguishes the
     * delivery marks by shade rather than by hue.
     *
     * Two colours survive, both because they carry meaning no shade of grey would. Error is a dark
     * red: an SOS must not look like ordinary text on a screen someone is squinting at. Warning is
     * a dark amber, used for a message that went out and was never confirmed - which must be
     * noticeable without looking as final as a failure.
     */
    s_daylight = (ui_theme_t){
        .name       = "Daylight",
        .bg         = lg_rgb(0xFFFFFF),
        .bar        = lg_rgb(0xE6E6E6),
        .surface    = lg_rgb(0xF4F4F4),
        .outline    = lg_rgb(0x8A8A8A),
        .text       = lg_rgb(0x000000),
        .muted      = lg_rgb(0x4A4A4A),   /* still dark: a light grey vanishes outdoors */
        .accent     = lg_rgb(0x000000),   /* selection is black, with white ink on it */
        .accent_ink = lg_rgb(0xFFFFFF),
        .warning    = lg_rgb(0x7A4A00),   /* the one amber: 'sent, nobody confirmed' must not read
                                             as ordinary text, nor as alarming as an error */
        .error      = lg_rgb(0x9B0014),   /* danger, and nothing else is this colour */
        .mark_wait  = lg_rgb(0x8A8A8A),   /* the marks differ by shade, not by hue */
        .mark_node  = lg_rgb(0x6A6A6A),
        .mark_deliv = lg_rgb(0x2B2B2B),
        .mark_read  = lg_rgb(0x000000),
    };
}

static void apply(ui_theme_kind_t kind)
{
    s_kind = kind;
    ui_theme = (kind == UI_THEME_DAYLIGHT) ? &s_daylight : &s_night;
}

void ui_theme_start(void)
{
    build();
    apply(lg_bsp_setting_get_bool(SETTING_KEY, false) ? UI_THEME_DAYLIGHT : UI_THEME_NIGHT);
    ESP_LOGI("UI", "[UI] Theme: %s", ui_theme->name);
}

ui_theme_kind_t ui_theme_kind(void)
{
    return s_kind;
}

const char *ui_theme_name(ui_theme_kind_t kind)
{
    return kind == UI_THEME_DAYLIGHT ? "Daylight" : "Night";
}

void ui_theme_set(ui_theme_kind_t kind)
{
    if (kind == s_kind) {
        return;
    }
    apply(kind);
    (void)lg_bsp_setting_set_bool(SETTING_KEY, kind == UI_THEME_DAYLIGHT);
    ESP_LOGI("UI", "[UI] Theme: %s", ui_theme->name);
    ui_repaint();   /* a retained screen would otherwise keep half the old colours */
}
