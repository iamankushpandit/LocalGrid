/*
 * The flashing announcement and the emergency takeover.
 *
 * Both are one overlay on LVGL's top layer, built once and shown again as needed: a screen
 * this loud is not worth allocating and freeing, and the handheld with the least memory is the
 * one most likely to be showing it. Nothing here is allocated per alert.
 *
 * The flash is the screen's own colour, not the backlight. A backlight blinking on and off
 * reads as a board fault; a panel changing colour reads as something demanding attention.
 *
 * Neither kind goes away by itself or by a stray touch: only the X closes it (D41, revised), so
 * a message nobody was looking at is still there when someone picks the handheld up. An
 * announcement stops flashing after a few cycles and waits quietly; an emergency keeps
 * flashing and sounding while it waits: D6 lets urgent broadcasts through when grid time is
 * unset and nothing else can be sent, so if one is on screen it is the only thing the grid
 * managed to say.
 */
#include "ui_alert.h"

#include <stdio.h>

#include "esp_log.h"
#include "lg_bsp_audio.h"
#include "lg_theme.h"
#include "lg_ui_screensaver.h"
#include "lg_ui_widgets.h"

static const char *TAG = "UI";

#define FLASH_MS          260   /* one half-cycle: fast enough to catch an eye, slow enough to read through */
#define ANNOUNCE_FLASHES  8     /* four full cycles, then it settles and waits for the X */
#define EMERGENCY_REPEAT_MS 4000 /* the siren again, while nobody has acknowledged it */

static struct {
    lv_obj_t   *cover;
    lv_obj_t   *kind_label;
    lv_obj_t   *who_label;
    lv_obj_t   *text_label;
    lv_obj_t   *close;
    lv_timer_t *flash;
    lv_timer_t *repeat;
    uint8_t     flashes_left;
    bool        lit;            /* which half of the flash we are in */
    bool        showing;
    uint8_t     kind;
} s;

static void hide(void)
{
    s.showing = false;
    if (s.flash != NULL) {
        lv_timer_pause(s.flash);
    }
    if (s.repeat != NULL) {
        lv_timer_pause(s.repeat);
    }
    lv_obj_add_flag(s.cover, LV_OBJ_FLAG_HIDDEN);
}

static void on_close(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "[UI] Alert closed");
    hide();
}

/* Paints the cover for whichever half of the flash we are in. */
static void paint(bool lit)
{
    const lg_theme_t *t = lg_theme();
    bool emergency = s.kind == UI_ALERT_EMERGENCY;
    lv_color_t ground = lit ? (emergency ? t->error : t->warning) : t->bg;
    lv_color_t ink = lit ? t->bg : (emergency ? t->error : t->warning);
    lv_obj_set_style_bg_color(s.cover, ground, 0);
    lv_obj_set_style_text_color(s.kind_label, ink, 0);
    lv_obj_set_style_text_color(s.who_label, ink, 0);
    lv_obj_set_style_text_color(s.text_label, ink, 0);
}

static void on_flash(lv_timer_t *timer)
{
    (void)timer;
    s.lit = !s.lit;
    paint(s.lit);
    if (s.flashes_left > 0 && --s.flashes_left == 0 && s.kind == UI_ALERT_ANNOUNCEMENT) {
        /* Stop flashing but leave the words up, so it can be read rather than only noticed. */
        lv_timer_pause(s.flash);
        paint(true);
    }
}

static void on_repeat(lv_timer_t *timer)
{
    (void)timer;
    if (s.showing && s.kind == UI_ALERT_EMERGENCY) {
        lg_bsp_audio_cue(LG_CUE_URGENT);   /* plays even at volume off, by design (D40) */
    }
}

void ui_alert_start(void)
{
    if (s.cover != NULL) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    s.cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.cover);
    lv_obj_set_size(s.cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s.cover, LV_OPA_COVER, 0);
    lv_obj_add_flag(s.cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s.cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s.cover, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s.cover, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s.cover, t->pad, 0);
    lv_obj_set_style_pad_row(s.cover, t->gap, 0);

    s.kind_label = lg_ui_text(s.cover, t->font_title, t->bg, "");
    s.who_label = lg_ui_text(s.cover, t->font_small, t->bg, "");
    s.text_label = lg_ui_label(s.cover, t->font_title, t->bg, "");
    lv_obj_set_style_text_align(s.text_label, LV_TEXT_ALIGN_CENTER, 0);

    /*
     * The X is the only way out, for both kinds. It floats in the corner outside the column so
     * the words stay centred. The cover stays clickable with no handler of its own, so a touch
     * elsewhere neither closes the alert nor reaches the screen underneath.
     */
    s.close = lg_ui_icon_button(s.cover, LV_SYMBOL_CLOSE, on_close, NULL);
    lv_obj_add_flag(s.close, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s.close, LV_ALIGN_TOP_RIGHT, 0, 0);

    s.flash = lv_timer_create(on_flash, FLASH_MS, NULL);
    s.repeat = lv_timer_create(on_repeat, EMERGENCY_REPEAT_MS, NULL);
    lv_timer_pause(s.flash);
    lv_timer_pause(s.repeat);
    lv_obj_add_flag(s.cover, LV_OBJ_FLAG_HIDDEN);
}

bool ui_alert_showing(void)
{
    return s.showing;
}

void ui_alert_show(ui_alert_kind_t kind, const char *who, const char *text)
{
    if (s.cover == NULL) {
        return;
    }
    /* An emergency interrupts an announcement; an announcement never buries an emergency that
     * nobody has acknowledged yet. */
    if (s.showing && s.kind == UI_ALERT_EMERGENCY && kind == UI_ALERT_ANNOUNCEMENT) {
        return;
    }

    s.kind = (uint8_t)kind;
    s.showing = true;
    bool emergency = kind == UI_ALERT_EMERGENCY;

    lg_ui_set_text(s.kind_label, emergency ? "URGENT" : "ANNOUNCEMENT");
    lg_ui_set_text(s.who_label, who != NULL ? who : "");
    lg_ui_set_text(s.text_label, text != NULL ? text : "");
    /* The saver owns the top layer while it runs, so an alert underneath it would be invisible. */
    lg_ui_screensaver_dismiss();

    s.flashes_left = emergency ? 0 : ANNOUNCE_FLASHES;   /* 0 means keep flashing */
    s.lit = true;
    paint(true);
    lv_obj_remove_flag(s.cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.cover);

    lv_timer_reset(s.flash);
    lv_timer_resume(s.flash);
    if (emergency) {
        lv_timer_reset(s.repeat);
        lv_timer_resume(s.repeat);
        lg_bsp_audio_cue(LG_CUE_URGENT);
    } else {
        lv_timer_pause(s.repeat);
        lg_bsp_audio_cue(LG_CUE_ANNOUNCE);
    }
    ESP_LOGI(TAG, "[UI] %s alert: %s", emergency ? "Emergency" : "Announcement", text != NULL ? text : "");
}
