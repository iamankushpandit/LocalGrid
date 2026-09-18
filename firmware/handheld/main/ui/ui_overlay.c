/*
 * The overlay: banner, alerts, and screen saver:
 *
 * - A 1:1 or group message for a conversation that is not on screen: a bell, and a banner at the
 *   top for 15 s. Tapping the banner opens that conversation. Unread counts are kept per
 *   conversation until it is opened.
 * - A broadcast: the screen flashes the warning colour with the words on it and chimes, stops
 *   flashing after four cycles, and stays until Read is tapped (D41, revised).
 * - An urgent broadcast: the error colour, flashing and sounding every 4 s (even at volume off)
 *   until Read is tapped.
 * - Either alert says where its sender is when that is known (D65): "240 m NE of you" with this
 *   handheld's own fix, "240 m NE of MAIN" without one, else the coordinates, and how old the
 *   position is past two minutes. It is looked at again each second and repainted only on change.
 * - After a minute untouched, if the setting is on, green characters fall down the panel. The
 *   first touch only dismisses it; the screen underneath is redrawn.
 * - While the handheld is locked (D62), alerts still take the screen and are read as usual, and
 *   afterwards the lock screen comes back; other messages chime and count as unread, with no
 *   banner, and do not wake the saver.
 *
 * Nothing here is allocated: the rain is fixed arrays, and the banner and alert draw straight to
 * the panel. When one goes away, the screen underneath is redrawn by the router.
 */
#include "ui_overlay.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_settings.h"
#include "lg_envelope.h"
#include "ui_geo.h"
#include "ui_lock.h"
#include "ui_nav.h"
#include "ui_theme.h"

static const char *TAG = "UI";

#define WATCH_MS          400
#define BANNER_MS         15000
#define BANNER_H          58
#define FLASH_MS          260    /* an announcement flashes eight times and settles */
#define EMERGENCY_FLASH_MS 500   /* an emergency flashes until it is read, so it pulses slower */
#define ALERT_FRAME       12     /* the flashing border: the alert itself is painted once */
#define ANNOUNCE_FLASHES  8
#define EMERGENCY_REPEAT_MS 4000
#define SAVER_IDLE_MS     60000
#define RAIN_STEP_MS      70
#define RAIN_COLS_MAX     30
#define RAIN_TRAIL        12
#define CONVS_TRACKED     (LG_MAX_DEVICES + LG_MAX_GROUPS + 1)
#define ALERT_TEXT_LINES  8
#define ALERT_WHERE_MS    1000   /* how often the sender's location line is looked at again */
#define ALERT_STALE_S     120    /* an older position says how old it is */
#define WHERE_MAX         48

/*
 * Where everything on an alert goes, worked out in one place (alert_layout) and used by the
 * painter and by touch alike, so the words, the location lines, and Read can never overlap.
 */
typedef struct {
    int16_t   margin;       /* clear of the flashing border */
    int16_t   kind_y;
    int16_t   who_y;
    int16_t   text_y;
    uint8_t   text_lines;   /* lines of the message that fit above the location */
    uint16_t  starts[ALERT_TEXT_LINES + 1];
    uint8_t   where_lines;  /* 0, 1, or 2 */
    lg_rect_t where;        /* the location lines, empty when the sender's position is not known */
    lg_rect_t read;         /* the Read button */
} alert_layout_t;

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
    uint32_t  alert_id;   /* the message the alert is showing, reported read when Read is tapped */
    uint32_t  alert_author;
    char      alert_who[HH_NAME_MAX];
    char      alert_text[HH_TEXT_MAX + 1];
    char      alert_where[2][WHERE_MAX];   /* "240 m NE of you", "position 5 min old" */
    uint32_t  where_ms;
    alert_layout_t lay;
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

uint32_t ui_notify_unread(uint8_t scope, uint32_t target)
{
    const tracked_t *c = slot_for(scope, target, false);
    return c != NULL ? c->unread : 0;
}

uint32_t ui_notify_unread_total(void)
{
    uint32_t total = 0;
    for (size_t i = 0; i < CONVS_TRACKED; i++) {
        total += s.convs[i].used ? s.convs[i].unread : 0u;
    }
    return total;
}

void ui_notify_mark_seen(uint8_t scope, uint32_t target)
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
        ui_redraw_current();
    }
}

void ui_overlay_screen_painted(void)
{
    if (s.banner && s.cover == OV_NONE) {
        draw_banner();
    }
}

/* ---- alert ---- */

/*
 * The alert is painted once and then only its border flashes. Repainting the whole panel every
 * 260 ms cost a full screen of pixels a flash -- on the classic ESP32 that is most of the SPI
 * bus, for as long as an emergency is up, and it made the rest of the UI crawl. The border is
 * about a fifth of the pixels and reads the same from across a tent.
 */
/*
 * Where the sender is (D65), in up to two lines: from this handheld when it has its own fix, else
 * from MAIN when MAIN's position is known, else the coordinates; and how old the position is when
 * it is more than two minutes old. Returns the lines written; 0 when the sender's position is not
 * known.
 */
static uint8_t alert_where(uint32_t author, char out[2][WHERE_MAX])
{
    out[0][0] = out[1][0] = '\0';
    hh_position_t them;
    if (!hh_service_position(author, &them) || !them.valid) {
        return 0;
    }
    hh_position_t from;
    char where[32];
    if (hh_service_own_position(&from) && from.valid) {
        ui_geo_where_text(from.lat_u, from.lon_u, them.lat_u, them.lon_u, where, sizeof(where));
        snprintf(out[0], WHERE_MAX, "%s of you", where);
    } else if (hh_service_position(HH_SUBJECT_MAIN, &from) && from.valid) {
        ui_geo_where_text(from.lat_u, from.lon_u, them.lat_u, them.lon_u, where, sizeof(where));
        snprintf(out[0], WHERE_MAX, "%s of MAIN", where);
    } else {
        ui_geo_coord_text(them.lat_u, them.lon_u, out[0], WHERE_MAX);
    }
    if (them.age_s <= ALERT_STALE_S) {
        return 1;
    }
    char age[16];
    ui_geo_age_text(them.age_s, age, sizeof(age));
    snprintf(out[1], WHERE_MAX, "position %s old", age);
    return 2;
}

/*
 * The alert's layout, top to bottom: the kind and sender, the message, the location lines, and
 * Read across the bottom, all inside the flashing border. Read and the location are placed from
 * the bottom up; the message gets the lines that are left (never fewer than it needs to overlap
 * nothing), and the heading and message are centred in the space above the location.
 */
static void alert_layout(uint8_t where_lines)
{
    alert_layout_t *L = &s.lay;
    int16_t w = (int16_t)s.w;
    int16_t h = (int16_t)s.h;
    L->margin = (int16_t)(ALERT_FRAME + UI_PAD);
    int16_t inner_w = (int16_t)(w - 2 * L->margin);
    int16_t bw = (int16_t)(w / 2);
    int16_t bh = (int16_t)(F_TITLE->line_height + 20);
    L->read = (lg_rect_t){ (int16_t)((w - bw) / 2), (int16_t)(h - L->margin - bh), bw, bh };
    int16_t bottom = (int16_t)(L->read.y - 2 * UI_GAP);
    L->where_lines = where_lines;
    if (where_lines > 0) {
        int16_t wh = (int16_t)(where_lines * F_BODY->line_height);
        L->where = (lg_rect_t){ L->margin, (int16_t)(bottom - wh), inner_w, wh };
        bottom = (int16_t)(L->where.y - 2 * UI_GAP);
    } else {
        L->where = (lg_rect_t){ 0, 0, 0, 0 };
    }
    int16_t top = (int16_t)(L->margin + UI_GAP);
    int16_t head = (int16_t)(F_TITLE->line_height + 6 + F_SMALL->line_height + 10);
    int16_t room = (int16_t)(bottom - top - head);
    int16_t fit = room > 0 ? (int16_t)(room / F_TITLE->line_height) : 0;
    uint8_t max_lines = (uint8_t)(fit > ALERT_TEXT_LINES ? ALERT_TEXT_LINES : fit);
    L->text_lines = max_lines > 0 ? lg_text_wrap(F_TITLE, F_EMOJI, s.alert_text, inner_w, L->starts, max_lines) : 0;
    int16_t used = (int16_t)(head + L->text_lines * F_TITLE->line_height);
    int16_t y = (int16_t)(top + (bottom - top - used) / 2);
    L->kind_y = y < top ? top : y;
    L->who_y = (int16_t)(L->kind_y + F_TITLE->line_height + 6);
    L->text_y = (int16_t)(L->who_y + F_SMALL->line_height + 10);
}

static void centred(const lg_canvas_t *c, const lg_rect_t *clip, int16_t y, const lg_font_t *font,
                    const lg_font_t *fallback, lg_color_t ink, const char *text, size_t n)
{
    char line[HH_TEXT_MAX + 1];
    n = n < sizeof(line) ? n : sizeof(line) - 1u;
    memcpy(line, text, n);
    line[n] = '\0';
    lg_paint_text(c, clip, (int16_t)((s.w - lg_draw_text_width(font, fallback, line)) / 2), y, font, fallback, ink,
                  line, n);
}

static void draw_alert_frame(void);

static void paint_alert(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    const alert_layout_t *L = &s.lay;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_color_t tone = s.emergency ? C_ERROR : C_WARNING;
    lg_color_t ground = tone;
    lg_color_t ink = C_BG;
    lg_paint_panel(c, &all, &all, ground, ground, ground, 0, 0);
    const char *kind = s.emergency ? "URGENT" : "ANNOUNCEMENT";
    centred(c, &all, L->kind_y, F_TITLE, NULL, ink, kind, strlen(kind));
    centred(c, &all, L->who_y, F_SMALL, F_EMOJI, ink, s.alert_who, strlen(s.alert_who));
    for (uint8_t l = 0; l < L->text_lines; l++) {
        centred(c, &all, (int16_t)(L->text_y + l * F_TITLE->line_height), F_TITLE, F_EMOJI, ink,
                s.alert_text + L->starts[l], (size_t)(L->starts[l + 1] - L->starts[l]));
    }
    for (uint8_t l = 0; l < L->where_lines; l++) {
        centred(c, &L->where, (int16_t)(L->where.y + l * F_BODY->line_height), F_BODY, NULL, ink, s.alert_where[l],
                strlen(s.alert_where[l]));
    }
    /* "Read" dismisses it: a named button says what the tap means, where the X in the corner
     * was both easy to miss and easy to hit by accident (owner, 2026-09-17). */
    lg_paint_panel(c, &all, &L->read, ink, ground, ink, 2, 6);
    const char *word = "Read";
    lg_paint_text(c, &L->read, (int16_t)(L->read.x + (L->read.w - lg_draw_text_width(F_TITLE, NULL, word)) / 2),
                  (int16_t)(L->read.y + (L->read.h - F_TITLE->line_height) / 2), F_TITLE, NULL, ground, word,
                  strlen(word));
}

static void draw_alert(void)
{
    lg_draw_scroll_area(0, 0);   /* a full-screen cover is drawn unscrolled; the screen is redrawn after */
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_region(&all, paint_alert, NULL);
}

/*
 * The sender's location is looked at again once a second while the alert is up. Only when its
 * words change is anything painted: the location lines alone when their number is the same, or
 * the whole alert (then the border) when a line came or went and the layout moved.
 */
static void alert_where_tick(uint32_t now)
{
    if (now - s.where_ms < ALERT_WHERE_MS) {
        return;
    }
    s.where_ms = now;
    char fresh[2][WHERE_MAX];
    uint8_t lines = alert_where(s.alert_author, fresh);
    if (lines == s.lay.where_lines && strcmp(fresh[0], s.alert_where[0]) == 0 &&
        strcmp(fresh[1], s.alert_where[1]) == 0) {
        return;
    }
    memcpy(s.alert_where, fresh, sizeof(fresh));
    if (lines == s.lay.where_lines) {
        lg_draw_region(&s.lay.where, paint_alert, NULL);
        return;
    }
    alert_layout(lines);
    draw_alert();
    draw_alert_frame();
}

/* One flash: four bars around the edge, and nothing else touched. */
static void draw_alert_frame(void)
{
    lg_color_t tone = s.emergency ? C_ERROR : C_WARNING;
    lg_color_t edge = s.lit ? C_BG : tone;
    int16_t w = (int16_t)s.w;
    int16_t h = (int16_t)s.h;
    const lg_rect_t bars[4] = {
        { 0, 0, w, ALERT_FRAME },
        { 0, (int16_t)(h - ALERT_FRAME), w, ALERT_FRAME },
        { 0, ALERT_FRAME, ALERT_FRAME, (int16_t)(h - 2 * ALERT_FRAME) },
        { (int16_t)(w - ALERT_FRAME), ALERT_FRAME, ALERT_FRAME, (int16_t)(h - 2 * ALERT_FRAME) },
    };
    for (int i = 0; i < 4; i++) {
        lg_draw_fill(&bars[i], edge);
    }
}

static void show_alert(bool emergency, const char *who, const char *text, uint32_t id, uint32_t author, uint32_t now)
{
    if (s.cover == OV_ALERT && s.emergency && !emergency) {
        return;   /* an announcement never buries an emergency nobody has acknowledged */
    }
    s.banner = false;
    s.cover = OV_ALERT;
    s.emergency = emergency;
    s.lit = false;   /* the frame starts dark against the coloured panel, then pulses */
    s.flashes_left = emergency ? 0 : ANNOUNCE_FLASHES;   /* 0: keep flashing */
    s.flash_ms = now;
    s.repeat_ms = now;
    s.alert_id = id;
    s.alert_author = author;
    s.where_ms = now;
    snprintf(s.alert_who, sizeof(s.alert_who), "%s", who);
    snprintf(s.alert_text, sizeof(s.alert_text), "%s", text);
    alert_layout(alert_where(author, s.alert_where));
    draw_alert();
    draw_alert_frame();
    (void)lg_bsp_audio_cue(emergency ? LG_CUE_URGENT : LG_CUE_ANNOUNCE);
    ESP_LOGI(TAG, "[UI] %s alert: %s", emergency ? "Emergency" : "Announcement", text);
    if (s.lay.where_lines > 0) {
        ESP_LOGI(TAG, "[UI] Alert sender is %s%s%s", s.alert_where[0], s.lay.where_lines > 1 ? "; " : "",
                 s.alert_where[1]);
    }
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
    const hh_status_t *st = ui_status();
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
        if (ui_chat_showing(scope, target) && s.cover != OV_SAVER) {
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
    bool locked = ui_lock_active();
    if (s.cover == OV_SAVER && (n_scope == LG_SCOPE_BROADCAST || !locked)) {
        s.cover = OV_NONE;   /* wake the screen for it (locked: only for an alert, the rest has no banner) */
        ui_redraw_current();
    }
    if (n_scope == LG_SCOPE_BROADCAST) {
        show_alert(newest.urgent, who, newest.text, newest.id, newest.author, now);
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
    if (locked) {
        /* D62: the sound and the unread count, but no banner: tapping one opens the chat, and
         * nothing under the lock may be reached. */
        ESP_LOGI(TAG, "[UI] Notified while locked: %s in %s", who, s.pending_title);
        return;
    }
    s.banner = true;
    s.banner_down = false;
    s.banner_since = now;
    draw_banner();
    ESP_LOGI(TAG, "[UI] Notified: %s in %s", who, s.pending_title);
}

/* ---- the overlay's own loop and touch ---- */

void ui_overlay_start(uint16_t w, uint16_t h)
{
    s.w = w;
    s.h = h;
    s.rnd = 0x1F35A2C7u;
    s.row_h = F_SMALL->line_height;
    s.col_w = (int16_t)(lg_draw_text_width(F_SMALL, NULL, "W") > 0 ? lg_draw_text_width(F_SMALL, NULL, "W") : 8);
    s.cols = (uint8_t)(w / s.col_w > RAIN_COLS_MAX ? RAIN_COLS_MAX : w / s.col_w);
    s.col_w = (int16_t)(w / s.cols);
    alert_layout(0);   /* Read's place is known before any alert; each alert lays itself out again */
}

bool ui_overlay_saver_now(uint32_t now_ms)
{
    if (s.cover != OV_NONE || !lg_bsp_setting_get_bool("saver", true)) {
        return false;
    }
    show_saver(now_ms);
    return true;
}

void ui_overlay_drop_banner(void)
{
    s.banner = false;
    s.banner_down = false;
}

bool ui_overlay_covering(void)
{
    return s.cover != OV_NONE;
}

void ui_overlay_tick(uint32_t now, uint32_t last_touch_ms)
{
    if (now - s.last_watch >= WATCH_MS) {
        s.last_watch = now;
        watch(now);
    }
    switch (s.cover) {
    case OV_ALERT:
        if ((s.emergency || s.flashes_left > 0) &&
            now - s.flash_ms >= (s.emergency ? EMERGENCY_FLASH_MS : FLASH_MS)) {
            s.flash_ms = now;
            s.lit = !s.lit;
            if (!s.emergency && --s.flashes_left == 0) {
                s.lit = true;   /* stop flashing and leave the words up to be read */
            }
            draw_alert_frame();   /* only the border moves; the words stay as painted */
        }
        if (s.emergency && now - s.repeat_ms >= EMERGENCY_REPEAT_MS) {
            s.repeat_ms = now;
            (void)lg_bsp_audio_cue(LG_CUE_URGENT);   /* plays even at volume off (D40) */
        }
        alert_where_tick(now);
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

bool ui_overlay_touch(int16_t x, int16_t y, bool down)
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
            ui_redraw_current();
        }
        return true;
    case OV_ALERT:
        if (!down && lg_rect_hit(&s.lay.read, x, y)) {
            s.cover = OV_NONE;   /* only Read closes an alert (D41, revised) */
            hh_service_mark_read(s.alert_id);   /* Read means read: the sender sees it (D58) */
            ESP_LOGI(TAG, "[UI] Alert closed");
            ui_redraw_current();
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
            ui_notify_mark_seen(s.pending_scope, s.pending_target);
            ESP_LOGI(TAG, "[UI] Banner tapped: opening %s", s.pending_title);
            ui_go(NAV_CHAT, s.pending_scope, s.pending_target, s.pending_title);
        } else {
            draw_banner();
        }
        return true;
    }
    return false;
}

void ui_overlay_log(uint32_t now_ms, uint32_t last_touch_ms)
{
    ESP_LOGI(TAG, "[UI] Overlay: cover %d, banner %d, idle %" PRIu32 " ms, saver setting %s, unread %" PRIu32,
             (int)s.cover, s.banner, now_ms - last_touch_ms, lg_bsp_setting_get_bool("saver", true) ? "on" : "off",
             ui_notify_unread_total());
}
