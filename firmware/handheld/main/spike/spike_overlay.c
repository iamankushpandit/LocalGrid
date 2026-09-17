/*
 * The overlay: banner, alerts, and screen saver for the no-LVGL UI, matching the LVGL firmware
 * (ui_notify.c, ui_alert.c, lg_ui_screensaver.c):
 *
 * - A 1:1 or group message for a conversation that is not on screen: a bell, and a banner at the
 *   top for 15 s. Tapping the banner opens that conversation. Unread counts are kept per
 *   conversation until it is opened.
 * - A broadcast: the screen flashes the warning colour with the words on it and chimes, stops
 *   flashing after four cycles, and stays until its X is tapped (D41, revised).
 * - An urgent broadcast: the error colour, flashing and sounding every 4 s (even at volume off)
 *   until its X is tapped.
 * - After a minute untouched, if the setting is on, green characters fall down the panel. The
 *   first touch only dismisses it; the screen underneath is redrawn.
 *
 * Nothing here is allocated: the rain is fixed arrays, and the banner and alert draw straight to
 * the panel. When one goes away, the screen underneath is redrawn by the router.
 */
#include "spike_overlay.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_settings.h"
#include "lg_envelope.h"
#include "spike_nav.h"
#include "spike_theme.h"

static const char *TAG = "UI";

#define WATCH_MS          400
#define BANNER_MS         15000
#define BANNER_H          58
#define FLASH_MS          260
#define ANNOUNCE_FLASHES  8
#define EMERGENCY_REPEAT_MS 4000
#define SAVER_IDLE_MS     60000
#define RAIN_STEP_MS      70
#define RAIN_COLS_MAX     30
#define RAIN_TRAIL        12
#define CONVS_TRACKED     (LG_MAX_DEVICES + LG_MAX_GROUPS + 1)

typedef struct {
    bool     used;
    uint8_t  scope;
    uint32_t target;
    uint32_t unread;
} tracked_t;

typedef enum { OV_NONE, OV_ALERT, OV_SAVER } cover_t;

static struct {
    uint16_t  w;
    uint16_t  h;
    tracked_t convs[CONVS_TRACKED];
    uint32_t  seen_id;
    bool      primed;          /* the first watch only records what is already there */
    uint32_t  last_watch;
    /* banner */
    bool      banner;
    bool      banner_down;
    uint32_t  banner_since;
    char      banner_text[HH_TEXT_MAX + 64];
    uint8_t   pending_scope;
    uint32_t  pending_target;
    char      pending_title[40];
    /* alert */
    cover_t   cover;
    bool      emergency;
    bool      lit;
    uint8_t   flashes_left;
    uint32_t  flash_ms;
    uint32_t  repeat_ms;
    char      alert_who[HH_NAME_MAX];
    char      alert_text[HH_TEXT_MAX + 1];
    lg_rect_t close;
    bool      swallow;         /* a dismissing touch: ignore it until the finger lifts */
    /* rain */
    uint8_t   cols;
    int16_t   col_w;
    int16_t   row_h;
    int16_t   head[RAIN_COLS_MAX];
    uint8_t   speed[RAIN_COLS_MAX];
    uint8_t   wait[RAIN_COLS_MAX];
    uint32_t  rain_ms;
    uint32_t  rnd;
} s;

/* ---- unread counts ---- */

static tracked_t *slot_for(uint8_t scope, uint32_t target, bool create)
{
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        if (s.convs[i].used && s.convs[i].scope == scope && s.convs[i].target == target) {
            return &s.convs[i];
        }
    }
    for (size_t i = 0; create && i < CONVS_TRACKED; i++) {
        if (!s.convs[i].used) {
            s.convs[i] = (tracked_t){ .used = true, .scope = scope, .target = target };
            return &s.convs[i];
        }
    }
    return NULL;
}

uint32_t spike_notify_unread(uint8_t scope, uint32_t target)
{
    const tracked_t *c = slot_for(scope, target, false);
    return c != NULL ? c->unread : 0;
}

uint32_t spike_notify_unread_total(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        total += s.convs[i].used ? s.convs[i].unread : 0u;
    }
    return total;
}

void spike_notify_mark_seen(uint8_t scope, uint32_t target)
{
    tracked_t *c = slot_for(scope, target, false);
    if (c != NULL) {
        c->unread = 0;
    }
}

/* ---- banner ---- */

static lg_rect_t banner_rect(void)
{
    return (lg_rect_t){ 4, 4, (int16_t)(s.w - 8), BANNER_H };
}

static void paint_banner(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t r = banner_rect();
    lg_rect_t around = { 0, 0, (int16_t)s.w, (int16_t)(BANNER_H + 8) };
    lg_paint_panel(c, &around, &r, s.banner_down ? C_OUTLINE : C_BAR, C_BG, C_ACCENT, 1, 6);
    lg_rect_t inner = { (int16_t)(r.x + 8), (int16_t)(r.y + 4), (int16_t)(r.w - 16), (int16_t)(r.h - 8) };
    uint16_t starts[3];
    uint8_t lines = lg_text_wrap(F_SMALL, F_EMOJI, s.banner_text, inner.w, starts, 2);
    for (uint8_t l = 0; l < lines; l++) {
        lg_paint_text(c, &inner, inner.x, (int16_t)(inner.y + l * F_SMALL->line_height), F_SMALL, F_EMOJI, C_ACCENT,
                      s.banner_text + starts[l], (size_t)(starts[l + 1] - starts[l]));
    }
    lg_paint_text(c, &inner, inner.x, (int16_t)(inner.y + inner.h - F_SMALL->line_height), F_SMALL, NULL, C_MUTED,
                  "Tap to open", 11);
}

static void draw_banner(void)
{
    lg_rect_t around = { 0, 0, (int16_t)s.w, (int16_t)(BANNER_H + 8) };
    lg_draw_region(&around, paint_banner, NULL);
}

static void hide_banner(void)
{
    if (s.banner) {
        s.banner = false;
        spike_redraw_current();
    }
}

void spike_overlay_screen_painted(void)
{
    if (s.banner && s.cover == OV_NONE) {
        draw_banner();
    }
}

/* ---- alert ---- */

static void paint_alert(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_color_t tone = s.emergency ? C_ERROR : C_WARNING;
    lg_color_t ground = s.lit ? tone : C_BG;
    lg_color_t ink = s.lit ? C_BG : tone;
    lg_paint_panel(c, &all, &all, ground, ground, ground, 0, 0);
    int16_t y = (int16_t)(s.h / 4);
    const char *kind = s.emergency ? "URGENT" : "ANNOUNCEMENT";
    lg_paint_text(c, &all, (int16_t)((s.w - lg_draw_text_width(F_TITLE, NULL, kind)) / 2), y, F_TITLE, NULL, ink, kind,
                  strlen(kind));
    y = (int16_t)(y + F_TITLE->line_height + 6);
    lg_paint_text(c, &all, (int16_t)((s.w - lg_draw_text_width(F_SMALL, F_EMOJI, s.alert_who)) / 2), y, F_SMALL,
                  F_EMOJI, ink, s.alert_who, strlen(s.alert_who));
    y = (int16_t)(y + F_SMALL->line_height + 10);
    uint16_t starts[9];
    uint8_t lines = lg_text_wrap(F_TITLE, F_EMOJI, s.alert_text, (int16_t)(s.w - 24), starts, 8);
    for (uint8_t l = 0; l < lines; l++) {
        char line[HH_TEXT_MAX + 1];
        size_t n = (size_t)(starts[l + 1] - starts[l]);
        memcpy(line, s.alert_text + starts[l], n);
        line[n] = '\0';
        lg_paint_text(c, &all, (int16_t)((s.w - lg_draw_text_width(F_TITLE, F_EMOJI, line)) / 2),
                      (int16_t)(y + l * F_TITLE->line_height), F_TITLE, F_EMOJI, ink, line, n);
    }
    int16_t xw = lg_draw_text_width(F_ICON, NULL, LV_SYMBOL_CLOSE);
    lg_paint_text(c, &all, (int16_t)(s.close.x + (s.close.w - xw) / 2),
                  (int16_t)(s.close.y + (s.close.h - F_ICON->line_height) / 2), F_ICON, NULL, ink, LV_SYMBOL_CLOSE,
                  strlen(LV_SYMBOL_CLOSE));
}

static void draw_alert(void)
{
    lg_draw_scroll_area(0, 0);   /* a full-screen cover is drawn unscrolled; the screen is redrawn after */
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_region(&all, paint_alert, NULL);
}

static void show_alert(bool emergency, const char *who, const char *text, uint32_t now)
{
    if (s.cover == OV_ALERT && s.emergency && !emergency) {
        return;   /* an announcement never buries an emergency nobody has acknowledged */
    }
    s.banner = false;
    s.cover = OV_ALERT;
    s.emergency = emergency;
    s.lit = true;
    s.flashes_left = emergency ? 0 : ANNOUNCE_FLASHES;   /* 0: keep flashing */
    s.flash_ms = now;
    s.repeat_ms = now;
    snprintf(s.alert_who, sizeof(s.alert_who), "%s", who);
    snprintf(s.alert_text, sizeof(s.alert_text), "%s", text);
    draw_alert();
    (void)lg_bsp_audio_cue(emergency ? LG_CUE_URGENT : LG_CUE_ANNOUNCE);
    ESP_LOGI(TAG, "[UI] %s alert: %s", emergency ? "Emergency" : "Announcement", text);
}

/* ---- screen saver ---- */

static uint32_t rnd(void)
{
    s.rnd ^= s.rnd << 13;
    s.rnd ^= s.rnd >> 17;
    s.rnd ^= s.rnd << 5;
    return s.rnd;
}

static void rain_cell(uint8_t col, int16_t row, lg_color_t fg)
{
    if (row < 0 || row * s.row_h >= s.h) {
        return;
    }
    static const char SET[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ<>*+-/|=";
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ (int16_t)(col * s.col_w), (int16_t)(row * s.row_h), s.col_w, s.row_h };
    b.bg = b.outside = C_BG;
    b.font = F_SMALL;
    b.fg = fg;
    b.align = LG_ALIGN_CENTER;
    if (fg != C_BG) {
        b.text[0] = SET[rnd() % (sizeof(SET) - 1u)];
    }
    lg_draw_box(&b);
}

static void show_saver(uint32_t now)
{
    s.banner = false;
    s.cover = OV_SAVER;
    lg_draw_scroll_area(0, 0);
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_fill(&all, C_BG);
    for (uint8_t i = 0; i < s.cols; i++) {
        s.head[i] = (int16_t)-(int16_t)(rnd() % (uint32_t)(s.h / s.row_h + 1));   /* scattered above */
        s.speed[i] = (uint8_t)(1u + rnd() % 2u);
        s.wait[i] = 0;
    }
    s.rain_ms = now;
    ESP_LOGI(TAG, "[UI] Screen saver on");
}

static void rain_step(void)
{
    int16_t rows = (int16_t)(s.h / s.row_h);
    for (uint8_t i = 0; i < s.cols; i++) {
        if (++s.wait[i] < s.speed[i]) {
            continue;
        }
        s.wait[i] = 0;
        rain_cell(i, s.head[i], C_ACCENT);                        /* the old head joins the trail */
        rain_cell(i, (int16_t)(s.head[i] - RAIN_TRAIL), C_BG);   /* the tail end clears */
        s.head[i]++;
        rain_cell(i, s.head[i], C_TEXT);                          /* the new head is the bright one */
        if (s.head[i] - RAIN_TRAIL > rows) {
            s.head[i] = (int16_t)-(int16_t)(rnd() % 8u);
            s.speed[i] = (uint8_t)(1u + rnd() % 2u);
        }
    }
}

/* ---- the new-message watch ---- */

static const char *sender_name(const hh_status_t *st, uint32_t device)
{
    for (uint8_t i = 0; i < st->n_people; i++) {
        if (st->people[i].device == device) {
            return st->people[i].name;
        }
    }
    return "A handheld";
}

static void watch(uint32_t now)
{
    static hh_message_t m;
    const hh_status_t *st = spike_status();
    uint32_t newest_seen = s.seen_id;
    bool found = false;
    uint8_t n_scope = 0;
    uint32_t n_target = 0;
    static hh_message_t newest;
    for (size_t i = 0; i < HH_MESSAGES && hh_service_message(i, &m); i++) {
        if (m.id <= s.seen_id) {
            break;   /* newest first: everything after this has been considered */
        }
        if (m.id > newest_seen) {
            newest_seen = m.id;
        }
        if (m.mine || !s.primed) {
            continue;
        }
        uint8_t scope = m.scope;
        uint32_t target = m.scope == LG_SCOPE_DIRECT ? m.author : m.target;
        if (spike_chat_showing(scope, target) && s.cover != OV_SAVER) {
            continue;   /* the reader is looking at it */
        }
        tracked_t *c = slot_for(scope, target, true);
        if (c != NULL) {
            c->unread++;
        }
        if (!found) {
            found = true;   /* the first seen here is the newest */
            newest = m;
            n_scope = scope;
            n_target = target;
        }
    }
    s.seen_id = newest_seen;
    if (!s.primed) {
        s.primed = true;
        return;
    }
    if (!found) {
        return;
    }
    const char *who = sender_name(st, newest.author);
    if (s.cover == OV_SAVER) {
        s.cover = OV_NONE;   /* wake the screen for it */
        spike_redraw_current();
    }
    if (n_scope == LG_SCOPE_BROADCAST) {
        show_alert(newest.urgent, who, newest.text, now);
        return;
    }
    s.pending_scope = n_scope;
    s.pending_target = n_target;
    if (n_scope == LG_SCOPE_GROUP) {
        snprintf(s.pending_title, sizeof(s.pending_title), "Group");
        for (uint8_t i = 0; i < st->n_groups; i++) {
            if (st->groups[i].id == (uint16_t)n_target) {
                snprintf(s.pending_title, sizeof(s.pending_title), "%s", st->groups[i].name);
            }
        }
        snprintf(s.banner_text, sizeof(s.banner_text), "%s in %s: %s", who, s.pending_title, newest.text);
    } else {
        snprintf(s.pending_title, sizeof(s.pending_title), "%s", who);
        snprintf(s.banner_text, sizeof(s.banner_text), "%s: %s", who, newest.text);
    }
    (void)lg_bsp_audio_cue(LG_CUE_RECEIVED);
    s.banner = true;
    s.banner_down = false;
    s.banner_since = now;
    draw_banner();
    ESP_LOGI(TAG, "[UI] Notified: %s in %s", who, s.pending_title);
}

/* ---- the overlay's own loop and touch ---- */

void spike_overlay_start(uint16_t w, uint16_t h)
{
    s.w = w;
    s.h = h;
    s.rnd = 0x1F35A2C7u;
    s.row_h = F_SMALL->line_height;
    s.col_w = (int16_t)(lg_draw_text_width(F_SMALL, NULL, "W") > 0 ? lg_draw_text_width(F_SMALL, NULL, "W") : 8);
    s.cols = (uint8_t)(w / s.col_w > RAIN_COLS_MAX ? RAIN_COLS_MAX : w / s.col_w);
    s.col_w = (int16_t)(w / s.cols);
    s.close = (lg_rect_t){ (int16_t)(w - 44), 0, 44, 44 };
}

bool spike_overlay_covering(void)
{
    return s.cover != OV_NONE;
}

void spike_overlay_tick(uint32_t now, uint32_t last_touch_ms)
{
    if (now - s.last_watch >= WATCH_MS) {
        s.last_watch = now;
        watch(now);
    }
    switch (s.cover) {
    case OV_ALERT:
        if ((s.emergency || s.flashes_left > 0) && now - s.flash_ms >= FLASH_MS) {
            s.flash_ms = now;
            s.lit = !s.lit;
            if (!s.emergency && --s.flashes_left == 0) {
                s.lit = true;   /* stop flashing and leave the words up to be read */
            }
            draw_alert();
        }
        if (s.emergency && now - s.repeat_ms >= EMERGENCY_REPEAT_MS) {
            s.repeat_ms = now;
            (void)lg_bsp_audio_cue(LG_CUE_URGENT);   /* plays even at volume off (D40) */
        }
        break;
    case OV_SAVER:
        if (now - s.rain_ms >= RAIN_STEP_MS) {
            s.rain_ms = now;
            rain_step();
        }
        break;
    default:
        if (s.banner && now - s.banner_since >= BANNER_MS) {
            hide_banner();
        }
        if (now - last_touch_ms >= SAVER_IDLE_MS && lg_bsp_setting_get_bool("saver", true)) {
            show_saver(now);
        }
        break;
    }
}

bool spike_overlay_touch(int16_t x, int16_t y, bool down)
{
    if (s.swallow) {
        if (!down) {
            s.swallow = false;
        }
        return true;
    }
    switch (s.cover) {
    case OV_SAVER:
        if (down) {
            s.cover = OV_NONE;   /* the first touch only dismisses, so it cannot press what is underneath */
            s.swallow = true;
            ESP_LOGI(TAG, "[UI] Screen saver off");
            spike_redraw_current();
        }
        return true;
    case OV_ALERT:
        if (!down && lg_rect_hit(&s.close, x, y)) {
            s.cover = OV_NONE;   /* only the X closes an alert (D41) */
            ESP_LOGI(TAG, "[UI] Alert closed");
            spike_redraw_current();
        }
        return true;   /* the rest of the cover takes every touch and does nothing with it */
    default:
        break;
    }
    if (!s.banner) {
        return false;
    }
    lg_rect_t r = banner_rect();
    if (down && lg_rect_hit(&r, x, y)) {
        if (!s.banner_down) {
            s.banner_down = true;
            draw_banner();
        }
        return true;
    }
    if (!down && s.banner_down) {
        s.banner_down = false;
        if (lg_rect_hit(&r, x, y)) {
            s.banner = false;
            spike_notify_mark_seen(s.pending_scope, s.pending_target);
            ESP_LOGI(TAG, "[UI] Banner tapped: opening %s", s.pending_title);
            spike_go(NAV_CHAT, s.pending_scope, s.pending_target, s.pending_title);
        } else {
            draw_banner();
        }
        return true;
    }
    return false;
}

void spike_overlay_log(uint32_t now_ms, uint32_t last_touch_ms)
{
    ESP_LOGI(TAG, "[UI] Overlay: cover %d, banner %d, idle %" PRIu32 " ms, saver setting %s, unread %" PRIu32,
             (int)s.cover, s.banner, now_ms - last_touch_ms, lg_bsp_setting_get_bool("saver", true) ? "on" : "off",
             spike_notify_unread_total());
}
