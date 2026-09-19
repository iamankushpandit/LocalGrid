/*
 * The alert and distress unit (D66): its three screens and its SOS.
 *
 * Idle        a watch face (D67): the LocalGrid mark, the unit's name and battery; local time in large
 *             digits with a blinking colon, the date, and a small GPS mark when a GPS keeps the time;
 *             the AP it is on; and the SOS button.
 * Countdown   after SOS was held for 3 s: 5 s on a red screen with a big Cancel.
 * SOS         "SOS sent", who has read it ("Seen by N"), and "I'm safe" (hold 3 s).
 *
 * Every action works from touch (on a board that has it) and from the profile's buttons: a hold
 * raises the SOS or says "I'm safe", a short press cancels the countdown or reads an alert.
 *
 * The SOS is an urgent broadcast, so it goes out with no grid time (D6) and past the announcer
 * list (D56): "SOS from <name> near <AP>", with the AP's coordinates when the grid knows them
 * (D65; only MAIN reports a position today). Until a handheld has read it (D58), it is sent again
 * every 60 s as a new message, "SOS (repeat n) from ...". A new message rather than the same one
 * again, because an AP and every handheld drop a repeat of a message they have seen (dedup), so
 * the same message would reach nobody new, and a handheld that closed the first alert would never
 * be told the emergency is still on. "Seen by" counts the readers of all of them together.
 *
 * APs let one urgent broadcast through per sender every LG_URGENT_INTERVAL_MS. The unit never
 * sends two urgent broadcasts closer than that (plus a margin), and when an AP refuses one for
 * rate anyway it is sent again once that interval has passed, a bounded number of times. A message
 * still waiting for an AP (none in range) is retransmitted by the client, so no repeat is stacked
 * on it: the 60 s count starts once an AP has taken it.
 *
 * All state is one allocation made at start, so a full handheld carries a pointer and no more.
 */
#include "ui_unit.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hh_service.h"
#include "lg_body.h"
#include "lg_bsp_button.h"
#include "lg_envelope.h"
#include "lg_node.h"        /* LG_URGENT_INTERVAL_MS: the APs' limit the repeats must respect */
#include "ui_geo.h"
#include "ui_lock.h"        /* the LocalGrid mark */
#include "ui_nav.h"
#include "ui_overlay.h"
#include "ui_theme.h"

static const char *TAG = "UI";

#define HOLD_MS           3000u   /* hold SOS, or I'm safe, this long */
#define COUNTDOWN_S       5u
#define SOS_REPEAT_MS     60000u  /* again, as a new message, until someone has read one */
#define URGENT_GAP_MS     (LG_URGENT_INTERVAL_MS + 500u)   /* our own spacing between urgent sends */
#define RATE_RETRIES_MAX  10u
#define SCAN_MS           500u    /* how often our own messages' states and readers are looked at */
#define REFRESH_MS        250u
#define SOS_TRACK         8u      /* SOS messages whose readers are counted; older ones keep their count */
#define WORDS_MAX         48u
#define PROGRESS_INSET    (UI_PAD + UI_GAP)   /* the hold bar sits this far inside the button */

typedef enum { U_IDLE, U_COUNTDOWN, U_ACTIVE } ustate_t;

/* One urgent broadcast of ours, followed from the moment it is queued to who has read it. */
typedef struct {
    bool     queued;              /* handed to the service; its message-list entry not found yet */
    uint32_t after_id;            /* the newest message id before it was queued */
    uint32_t id;                  /* its message-list id, once found */
    uint8_t  state;               /* hh_msg_state_t */
    uint8_t  reject;
    uint32_t read_mask;
    char     text[HH_TEXT_MAX + 1];
} sent_t;

typedef struct {
    const lg_board_t *board;
    uint16_t   w;
    uint16_t   h;
    bool       touch;
    ustate_t   state;

    /* boxes: which ones are on screen depends on the state */
    lg_box_t   name, batt, date, ap, ap_note, notice;                /* idle */
    lg_box_t   ap_note2, notice2;   /* the second line those two wrap onto: 172 px is narrow */
    lg_box_t   title, line1, line2, seen, names;                     /* countdown and SOS */
    lg_box_t   hint;
    lg_rect_t  action;            /* SOS, Cancel, or I'm safe */
    lg_color_t action_bg;
    lg_color_t action_ink;
    const char *action_word;
    lg_color_t ground;            /* the screen's background */
    int16_t    progress_w;        /* how much of the hold bar is drawn */

    /* the watch face (idle, D67): regions painted only when what they show changes */
    lg_rect_t  logo;
    lg_rect_t  rule;              /* the hairline under the header */
    lg_rect_t  digit[4];
    lg_rect_t  colon;
    lg_rect_t  gps;
    float      seg;               /* segment thickness, px */
    char       shown[4];          /* digits on screen, '-' for "--:--"; 0 = repaint */
    int8_t     colon_shown;       /* 0 dim, 1 lit, 2 lit muted (no time), -1 repaint */
    int8_t     gps_shown;         /* face_gps_t on screen, -1 repaint */

    /* holds */
    bool       touch_hold;
    uint32_t   touch_since;
    bool       need_lift;         /* a finger still down from the last screen: ignored until it lifts */
    int8_t     btn_down;          /* a button whose hold means something here is down; -1 none */

    uint32_t   refresh_ms;
    uint32_t   scan_ms;
    uint32_t   count_start;

    /* the SOS */
    bool       sos_active;
    uint16_t   repeat;            /* 0 for the first message */
    uint32_t   ids[SOS_TRACK];
    uint8_t    n_ids;
    uint8_t    ids_next;
    uint32_t   mask;              /* readers of any of them */
    sent_t     last;              /* the newest SOS message */
    uint32_t   next_ms;           /* when the next one is due; 0 none */
    bool       retry_same;        /* the next one repeats a refused one: same number */
    bool       accepted_any;      /* an AP has taken one of them */
    uint8_t    rate_retries;
    uint32_t   sched_id;          /* (id, state) the schedule was last worked out for */
    uint8_t    sched_state;
    uint32_t   last_urgent_ms;
    bool       sent_any;
    char       last_ap[HH_SSID_MAX];

    /* I'm safe */
    sent_t     safe;
    uint32_t   safe_due_ms;       /* 0 none */
    uint8_t    safe_retries;
    uint32_t   safe_sched_id;
    uint8_t    safe_sched_state;

    hh_message_t m;               /* scratch for the message scan */
} unit_t;

static unit_t *u;

/* ---- drawing ---- */

static lg_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, const lg_font_t *font, lg_color_t fg, lg_color_t bg,
                    uint8_t align)
{
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ x, y, w, h };
    b.bg = bg;
    b.outside = bg;
    b.font = font;
    b.fallback = F_EMOJI;
    b.fg = fg;
    b.align = align;
    b.pad = UI_PAD;
    return b;
}

/* Sets a box's words and colour, drawing only on a change (D29). */
static void put(lg_box_t *b, lg_color_t fg, const char *text)
{
    if (b->fg != fg) {
        b->fg = fg;
        snprintf(b->text, sizeof(b->text), "%s", text);
        lg_draw_box(b);
        return;
    }
    (void)lg_draw_set_text(b, text);
}

/*
 * Two stacked one-line boxes as one wrapped line: the text breaks at a word to fit the width, and
 * whatever does not fit on the second line is cut with an ellipsis. On the 172 px unit a reason such
 * as "The AP did not accept this handheld" ran straight off the edge.
 */
static void put2(lg_box_t *first, lg_box_t *second, lg_color_t fg, const char *text)
{
    uint16_t starts[3];
    uint8_t lines = lg_text_wrap(first->font, NULL, text, (int16_t)(first->rect.w - 2 * UI_PAD), starts, 2);
    char a[sizeof(first->text)] = "";
    char b[sizeof(second->text)] = "";
    if (lines > 0) {
        size_t n = starts[1] - starts[0];
        snprintf(a, sizeof(a), "%.*s", (int)(n < sizeof(a) ? n : sizeof(a) - 1u), text + starts[0]);
    }
    if (lines > 1) {
        size_t rest = strlen(text + starts[1]);
        size_t n = starts[2] - starts[1];
        snprintf(b, sizeof(b), "%.*s%s", (int)(n < sizeof(b) - 4u ? n : sizeof(b) - 4u), text + starts[1],
                 rest > n ? "..." : "");
    }
    put(first, fg, a);
    put(second, fg, b);
}

static int16_t line_h(const lg_font_t *f)
{
    return (int16_t)(f->line_height + UI_GAP);
}

/* put, for a line that also changes font (the date, or a warning that needs the smaller one). */
static void put_font(lg_box_t *b, const lg_font_t *font, lg_color_t fg, const char *text)
{
    if (b->font != font) {
        b->font = font;
        b->fg = fg;
        snprintf(b->text, sizeof(b->text), "%s", text);
        lg_draw_box(b);
        return;
    }
    put(b, fg, text);
}

/* ---- the watch face (D67) ----
 *
 * Seven-segment digits drawn from distance tests, as the logo is, so they scale to any panel: the
 * largest that fit the width beside the GPS mark and the height between the header and the AP
 * lines. Unlit segments show faintly, as on a watch. Each digit, the colon and the GPS mark is its
 * own region, painted only when what it shows changes: a new minute repaints one or two digits, a
 * second only the colon.
 */
typedef enum { FACE_GPS_NONE = 0, FACE_GPS_OFF, FACE_GPS_ON } face_gps_t;

/* Segments a..g as bits 0..6. */
static uint8_t segments_of(char ch)
{
    static const uint8_t DIGITS[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    return ch >= '0' && ch <= '9' ? DIGITS[ch - '0'] : ch == '-' ? 0x40u : 0x00u;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Squared distance from (px, py) to the segment a-b: the C6 has no FPU, so one square root a pixel. */
static float seg_distance2(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax;
    float dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? clamp01(((px - ax) * dx + (py - ay) * dy) / len2) : 0.0f;
    float qx = ax + t * dx - px;
    float qy = ay + t * dy - py;
    return qx * qx + qy * qy;
}

static float seg_distance(float px, float py, float ax, float ay, float bx, float by)
{
    return sqrtf(seg_distance2(px, py, ax, ay, bx, by));
}

/* How much of a pixel a stroke half wide covers at distance d from its centre line, 0..255. */
static uint8_t cover(float d, float half)
{
    return (uint8_t)(clamp01(half + 0.5f - d) * 255.0f);
}

typedef struct {
    const lg_rect_t *r;
    char             ch;     /* a digit: '0'..'9', '-' or ' ' */
    bool             lit;    /* the colon */
    lg_color_t       ink;
    uint8_t          gps;    /* face_gps_t */
} face_paint_t;

/* The rows of r inside this band. */
static bool band_rows(const lg_canvas_t *c, const lg_rect_t *r, int16_t *y_from, int16_t *y_to)
{
    *y_from = r->y > c->band.y ? r->y : c->band.y;
    int16_t end_r = (int16_t)(r->y + r->h);
    int16_t end_b = (int16_t)(c->band.y + c->band.h);
    *y_to = end_r < end_b ? end_r : end_b;
    return *y_from < *y_to;
}

static void paint_digit(const lg_canvas_t *c, void *ctx)
{
    const face_paint_t *f = ctx;
    const lg_rect_t *r = f->r;
    lg_paint_panel(c, r, r, C_BG, C_BG, C_BG, 0, 0);
    float half = u->seg / 2.0f;
    float x0 = half + 0.5f;
    float x1 = (float)r->w - half - 0.5f;
    float y0 = half + 0.5f;
    float ym = (float)r->h / 2.0f;
    float y1 = (float)r->h - half - 0.5f;
    float gap = u->seg * 0.9f;   /* ends pulled in so the segments stand apart */
    const float S[7][4] = {
        { x0 + gap, y0, x1 - gap, y0 },   /* a */
        { x1, y0 + gap, x1, ym - gap },   /* b */
        { x1, ym + gap, x1, y1 - gap },   /* c */
        { x0 + gap, y1, x1 - gap, y1 },   /* d */
        { x0, ym + gap, x0, y1 - gap },   /* e */
        { x0, y0 + gap, x0, ym - gap },   /* f */
        { x0 + gap, ym, x1 - gap, ym },   /* g */
    };
    uint8_t on = segments_of(f->ch);
    int16_t y_from, y_to;
    if (!band_rows(c, r, &y_from, &y_to)) {
        return;
    }
    for (int16_t y = y_from; y < y_to; y++) {
        float py = (float)(y - r->y) + 0.5f;
        for (int16_t x = r->x; x < r->x + r->w; x++) {
            float px = (float)(x - r->x) + 0.5f;
            float lit = 1e6f;
            float dim = 1e6f;
            for (int k = 0; k < 7; k++) {
                float d = seg_distance2(px, py, S[k][0], S[k][1], S[k][2], S[k][3]);
                if (on & (1u << k)) {
                    lit = d < lit ? d : lit;
                } else {
                    dim = d < dim ? d : dim;
                }
            }
            float reach2 = (half + 0.5f) * (half + 0.5f);   /* beyond this nothing is painted: no root needed */
            if (dim < reach2) {
                lg_paint_pixel(c, r, x, y, C_SURFACE, cover(sqrtf(dim), half));
            }
            if (lit < reach2) {
                lg_paint_pixel(c, r, x, y, f->ink, cover(sqrtf(lit), half));
            }
        }
    }
}

static void paint_colon(const lg_canvas_t *c, void *ctx)
{
    const face_paint_t *f = ctx;
    const lg_rect_t *r = f->r;
    lg_paint_panel(c, r, r, C_BG, C_BG, C_BG, 0, 0);
    float radius = u->seg * 0.6f;
    float cx = (float)r->w / 2.0f;
    float ya = (float)r->h * 0.3f;
    float yb = (float)r->h * 0.7f;
    int16_t y_from, y_to;
    if (!band_rows(c, r, &y_from, &y_to)) {
        return;
    }
    for (int16_t y = y_from; y < y_to; y++) {
        float py = (float)(y - r->y) + 0.5f;
        for (int16_t x = r->x; x < r->x + r->w; x++) {
            float px = (float)(x - r->x) + 0.5f;
            float da = sqrtf((px - cx) * (px - cx) + (py - ya) * (py - ya));
            float db = sqrtf((px - cx) * (px - cx) + (py - yb) * (py - yb));
            float d = da < db ? da : db;
            lg_paint_pixel(c, r, x, y, f->lit ? f->ink : C_SURFACE, (uint8_t)(clamp01(radius + 0.5f - d) * 255.0f));
        }
    }
}

/* A location crosshair: a ring with four ticks, and a centre dot when a GPS keeps the time; the
 * same outline, faint and without the dot, when the time is set by hand; nothing when unset. */
static void paint_gps(const lg_canvas_t *c, void *ctx)
{
    const face_paint_t *f = ctx;
    const lg_rect_t *r = f->r;
    lg_paint_panel(c, r, r, C_BG, C_BG, C_BG, 0, 0);
    if (f->gps == FACE_GPS_NONE) {
        return;
    }
    bool on = f->gps == FACE_GPS_ON;
    lg_color_t ink = on ? C_ACCENT : C_MUTED;
    float fade = on ? 1.0f : 0.55f;
    float size = (float)r->w;
    float c0 = size / 2.0f;
    float ring = size / 2.0f - size * 0.2f;
    float half = size >= 14.0f ? 0.8f : 0.6f;
    int16_t y_from, y_to;
    if (!band_rows(c, r, &y_from, &y_to)) {
        return;
    }
    for (int16_t y = y_from; y < y_to; y++) {
        float py = (float)(y - r->y) + 0.5f;
        for (int16_t x = r->x; x < r->x + r->w; x++) {
            float px = (float)(x - r->x) + 0.5f;
            float dx = px - c0;
            float dy = py - c0;
            float dist = sqrtf(dx * dx + dy * dy);
            float d = fabsf(dist - ring);
            float ticks = fminf(fminf(seg_distance(px, py, 0.5f, c0, c0 - ring, c0), seg_distance(px, py, size - 0.5f, c0, c0 + ring, c0)),
                                fminf(seg_distance(px, py, c0, 0.5f, c0, c0 - ring), seg_distance(px, py, c0, size - 0.5f, c0, c0 + ring)));
            d = d < ticks ? d : ticks;
            float a = clamp01(half + 0.5f - d);
            if (on) {
                a = fmaxf(a, clamp01(size * 0.14f + 0.5f - dist));   /* the dot */
            }
            lg_paint_pixel(c, r, x, y, ink, (uint8_t)(a * fade * 255.0f));
        }
    }
}

static void paint_logo(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_paint_panel(c, &u->logo, &u->logo, C_BG, C_BG, C_BG, 0, 0);
    ui_paint_logo(c, u->logo.x, u->logo.y);
}

/* Paints what changed on the face: digits (4 chars, '-' for unset), the colon, the GPS mark. */
static void face_show(const char digits[4], bool colon_lit, lg_color_t ink, face_gps_t gps)
{
    for (int i = 0; i < 4; i++) {
        if (u->shown[i] != digits[i]) {
            u->shown[i] = digits[i];
            face_paint_t f = { .r = &u->digit[i], .ch = digits[i], .ink = ink };
            lg_draw_region(&u->digit[i], paint_digit, &f);
        }
    }
    int8_t colon_state = (int8_t)(!colon_lit ? 0 : ink == C_MUTED ? 2 : 1);
    if (u->colon_shown != colon_state) {
        u->colon_shown = colon_state;
        face_paint_t f = { .r = &u->colon, .lit = colon_lit, .ink = ink };
        lg_draw_region(&u->colon, paint_colon, &f);
    }
    if (u->gps_shown != (int8_t)gps) {
        u->gps_shown = (int8_t)gps;
        face_paint_t f = { .r = &u->gps, .gps = (uint8_t)gps };
        lg_draw_region(&u->gps, paint_gps, &f);
    }
}

/* After a full repaint every part of the face is drawn again. */
static void face_forget(void)
{
    memset(u->shown, 0, sizeof(u->shown));
    u->colon_shown = -1;
    u->gps_shown = -1;
}

static void paint_action(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    const lg_rect_t *r = &u->action;
    lg_paint_panel(c, r, r, u->action_bg, u->ground, u->action_bg, 0, 2 * UI_GAP);
    const lg_font_t *f = lg_draw_text_width(F_HUGE, NULL, u->action_word) <= r->w - 2 * UI_PAD ? F_HUGE : F_TITLE;
    lg_paint_text(c, r, (int16_t)(r->x + (r->w - lg_draw_text_width(f, NULL, u->action_word)) / 2),
                  (int16_t)(r->y + (r->h - f->line_height) / 2), f, NULL, u->action_ink, u->action_word,
                  strlen(u->action_word));
}

static void draw_action(void)
{
    u->progress_w = 0;
    lg_draw_region(&u->action, paint_action, NULL);
}

/* The hold's progress: a bar along the bottom of the action, grown in steps, cleared by a repaint. */
static lg_rect_t progress_rect(int16_t w)
{
    int16_t inset = PROGRESS_INSET;
    int16_t bar_h = UI_PAD;
    return (lg_rect_t){ (int16_t)(u->action.x + inset), (int16_t)(u->action.y + u->action.h - inset - bar_h), w, bar_h };
}

static void draw_progress(uint32_t held_ms)
{
    int16_t full = (int16_t)(u->action.w - 2 * PROGRESS_INSET);
    int16_t w = held_ms == 0 ? 0 : (int16_t)((int32_t)full * (int32_t)(held_ms > HOLD_MS ? HOLD_MS : held_ms) / (int32_t)HOLD_MS);
    if (w == u->progress_w) {
        return;
    }
    if (w < u->progress_w) {
        draw_action();   /* let go: the bar goes */
        return;
    }
    lg_rect_t r = progress_rect(w);
    lg_draw_fill(&r, u->action_ink);
    u->progress_w = w;
}

static const char *hold_hint(const char *what)
{
    static char text[WORDS_MAX];
    bool button = lg_board_button_actions(u->board) & (LG_ACT_SOS | LG_ACT_SAFE);
    if (u->touch && button) {
        snprintf(text, sizeof(text), "Hold %s or button 3 s", what);
    } else if (button) {
        snprintf(text, sizeof(text), "Hold button 3 s: %s", what);
    } else {
        snprintf(text, sizeof(text), "Hold %s 3 s", what);
    }
    return text;
}

/* The action button, btn_h tall, and its hint across the bottom; returns the top of the action. */
static int16_t place_bottom(int16_t btn_h)
{
    int16_t hint_h = line_h(F_SMALL);
    u->action = (lg_rect_t){ UI_PAD, (int16_t)(u->h - UI_PAD - hint_h - UI_GAP - btn_h), (int16_t)(u->w - 2 * UI_PAD),
                             btn_h };
    u->hint = box(0, (int16_t)(u->h - UI_PAD - hint_h), (int16_t)u->w, hint_h, F_SMALL, C_MUTED, u->ground,
                  LG_ALIGN_CENTER);
    u->hint.pad = 0;
    return u->action.y;
}

/*
 * The watch face (D67), laid out from the panel's size: the header (the LocalGrid mark, the name,
 * the battery, a hairline), then the SOS button and the notice lines from the bottom up, then the AP
 * lines and the date above those, and the digits take what is left between: as large as the width
 * beside the GPS mark allows, at most twice as tall as wide. The button is a fifth of the height here
 * (a quarter on the SOS screens) so the time can be large on a 320 px panel.
 */
static void screen_idle(void)
{
    u->ground = C_BG;
    int16_t w = (int16_t)u->w;
    int16_t batt_w = (int16_t)(w / 4);
    u->logo = (lg_rect_t){ UI_PAD, (int16_t)((UI_HEAD_H - UI_LOGO_SIZE) / 2), UI_LOGO_SIZE, UI_LOGO_SIZE };
    int16_t name_x = (int16_t)(u->logo.x + u->logo.w);
    u->name = box(name_x, 0, (int16_t)(w - batt_w - name_x), UI_HEAD_H, F_BODY, C_TEXT, C_BG, LG_ALIGN_LEFT);
    u->batt = box((int16_t)(w - batt_w), 0, batt_w, UI_HEAD_H, F_SMALL, C_MUTED, C_BG, LG_ALIGN_RIGHT);
    u->rule = (lg_rect_t){ UI_PAD, UI_HEAD_H, (int16_t)(w - 2 * UI_PAD), 1 };

    int16_t top = place_bottom((int16_t)(u->h / 5));
    u->notice2 = box(0, (int16_t)(top - 2 * UI_GAP - line_h(F_SMALL)), w, line_h(F_SMALL), F_SMALL, C_ACCENT, C_BG,
                     LG_ALIGN_CENTER);
    u->notice = box(0, (int16_t)(u->notice2.rect.y - line_h(F_SMALL)), w, line_h(F_SMALL), F_SMALL, C_ACCENT, C_BG,
                    LG_ALIGN_CENTER);
    int16_t y = (int16_t)(u->notice.rect.y - UI_GAP - 2 * line_h(F_SMALL));
    u->ap_note = box(0, y, w, line_h(F_SMALL), F_SMALL, C_MUTED, C_BG, LG_ALIGN_CENTER);
    u->ap_note2 = box(0, (int16_t)(y + line_h(F_SMALL)), w, line_h(F_SMALL), F_SMALL, C_MUTED, C_BG, LG_ALIGN_CENTER);
    y = (int16_t)(y - line_h(F_BODY));
    u->ap = box(0, y, w, line_h(F_BODY), F_BODY, C_ACCENT, C_BG, LG_ALIGN_CENTER);
    int16_t face_bottom = (int16_t)(y - 2 * UI_GAP - line_h(F_BODY));
    u->date = box(0, face_bottom, w, line_h(F_BODY), F_BODY, C_MUTED, C_BG, LG_ALIGN_CENTER);

    /* Digits d, gaps d/5, a colon d/2: 4d + 4(d/5) + d/2 = 5.3d wide, 2d tall; the GPS mark beside. */
    int16_t face_top = (int16_t)(UI_HEAD_H + 2 * UI_GAP);
    int16_t mark = (int16_t)F_BODY->line_height;
    int16_t room_w = (int16_t)(w - 2 * UI_PAD - UI_GAP - mark);
    int16_t room_h = (int16_t)(face_bottom - UI_GAP - face_top);
    int16_t dw = (int16_t)(room_w * 10 / 53);
    if (2 * dw > room_h) {
        dw = (int16_t)(room_h / 2);
    }
    dw = dw < 8 ? 8 : dw;
    int16_t dh = (int16_t)(2 * dw);
    int16_t gap = (int16_t)(dw / 5);
    int16_t cw = (int16_t)(dw / 2);
    int16_t total = (int16_t)(4 * dw + 4 * gap + cw + UI_GAP + mark);
    int16_t x = (int16_t)((w - total) / 2);
    int16_t fy = (int16_t)(face_top + (room_h - dh) / 2);
    u->seg = dw / 5.0f < 2.5f ? 2.5f : dw / 5.0f;
    for (int i = 0; i < 4; i++) {
        u->digit[i] = (lg_rect_t){ x, fy, dw, dh };
        x = (int16_t)(x + dw + gap);
        if (i == 1) {
            u->colon = (lg_rect_t){ x, fy, cw, dh };
            x = (int16_t)(x + cw + gap);
        }
    }
    u->gps = (lg_rect_t){ (int16_t)(x - gap + UI_GAP), fy, mark, mark };
    face_forget();

    u->action_bg = C_ERROR;
    u->action_ink = C_BG;
    u->action_word = "SOS";
    snprintf(u->hint.text, sizeof(u->hint.text), "%s", hold_hint("SOS"));
}

static void screen_countdown(void)
{
    u->ground = C_ERROR;
    int16_t w = (int16_t)u->w;
    int16_t y = (int16_t)(UI_HEAD_H / 2);
    u->title = box(0, y, w, line_h(F_TITLE), F_TITLE, C_BG, C_ERROR, LG_ALIGN_CENTER);
    snprintf(u->title.text, sizeof(u->title.text), "SOS in");
    y = (int16_t)(y + line_h(F_TITLE) + UI_GAP);
    u->line1 = box(0, y, w, line_h(F_HUGE) * 2, F_HUGE, C_BG, C_ERROR, LG_ALIGN_CENTER);   /* the count */
    y = (int16_t)(y + line_h(F_HUGE) * 2);
    u->line2 = box(0, y, w, line_h(F_SMALL), F_SMALL, C_BG, C_ERROR, LG_ALIGN_CENTER);
    snprintf(u->line2.text, sizeof(u->line2.text), "Then to everyone");
    place_bottom((int16_t)(u->h / 4));
    u->hint.fg = C_BG;
    snprintf(u->hint.text, sizeof(u->hint.text), "%s",
             u->touch ? "Tap Cancel or press button" : "Press button to cancel");
    u->action_bg = C_BG;
    u->action_ink = C_ERROR;
    u->action_word = "Cancel";
}

static void screen_active(void)
{
    u->ground = C_BG;
    int16_t w = (int16_t)u->w;
    u->title = box(0, 0, w, (int16_t)(UI_HEAD_H + 2 * UI_GAP), F_TITLE, C_BG, C_ERROR, LG_ALIGN_CENTER);
    int16_t y = (int16_t)(UI_HEAD_H + 4 * UI_GAP);
    u->line1 = box(0, y, w, line_h(F_SMALL), F_SMALL, C_TEXT, C_BG, LG_ALIGN_CENTER);   /* how it is going */
    y = (int16_t)(y + line_h(F_SMALL) + 2 * UI_GAP);
    u->line2 = box(0, y, w, line_h(F_BODY), F_BODY, C_MUTED, C_BG, LG_ALIGN_CENTER);
    snprintf(u->line2.text, sizeof(u->line2.text), "Seen by");
    y = (int16_t)(y + line_h(F_BODY));
    u->seen = box(0, y, w, line_h(F_HUGE), F_HUGE, C_ACCENT, C_BG, LG_ALIGN_CENTER);
    y = (int16_t)(y + line_h(F_HUGE));
    u->names = box(0, y, w, line_h(F_SMALL), F_SMALL, C_TEXT, C_BG, LG_ALIGN_CENTER);
    place_bottom((int16_t)(u->h / 4));
    u->action_bg = C_ACCENT;
    u->action_ink = C_ACCENT_INK;
    u->action_word = "I'm safe";
    snprintf(u->hint.text, sizeof(u->hint.text), "%s", hold_hint("I'm safe"));
}

static void refresh(uint32_t now);

static void draw_all(void)
{
    lg_draw_scroll_area(0, 0);
    lg_rect_t all = { 0, 0, (int16_t)u->w, (int16_t)u->h };
    lg_draw_fill(&all, u->ground);
    switch (u->state) {
    case U_IDLE:
        lg_draw_region(&u->logo, paint_logo, NULL);
        lg_draw_box(&u->name);
        lg_draw_box(&u->batt);
        lg_draw_fill(&u->rule, C_OUTLINE);
        face_forget();   /* the face is painted by the refresh that follows */
        lg_draw_box(&u->date);
        lg_draw_box(&u->ap);
        lg_draw_box(&u->ap_note);
        lg_draw_box(&u->ap_note2);
        lg_draw_box(&u->notice);
        lg_draw_box(&u->notice2);
        break;
    case U_COUNTDOWN:
        lg_draw_box(&u->title);
        lg_draw_box(&u->line1);
        lg_draw_box(&u->line2);
        break;
    case U_ACTIVE:
        lg_draw_box(&u->title);
        lg_draw_box(&u->line1);
        lg_draw_box(&u->line2);
        lg_draw_box(&u->seen);
        lg_draw_box(&u->names);
        break;
    }
    draw_action();
    lg_draw_box(&u->hint);
}

/* Changes screen. With draw false (an alert covers the panel) only the layout changes, and the
 * screen is painted when the alert goes (ui_unit_redraw). */
static void go(ustate_t state, uint32_t now, bool draw)
{
    u->state = state;
    u->touch_hold = false;
    u->btn_down = -1;
    switch (state) {
    case U_IDLE:      screen_idle(); break;
    case U_COUNTDOWN: screen_countdown(); break;
    case U_ACTIVE:    screen_active(); break;
    }
    u->refresh_ms = 0;
    if (draw) {
        draw_all();
        refresh(now);
    }
}

/* ---- sending ---- */

/* all_clear: "I'm safe", which receivers show calmly and which takes down this unit's SOS there. */
static bool urgent_send(sent_t *t, const char *text, uint32_t now, bool all_clear)
{
    uint32_t newest = hh_service_message(0, &u->m) ? u->m.id : 0;
    memset(t, 0, sizeof(*t));
    snprintf(t->text, sizeof(t->text), "%s", text);
    esp_err_t err = all_clear ? hh_service_send_all_clear(t->text)
                              : hh_service_send(LG_SCOPE_BROADCAST, LG_TARGET_ALL, true, t->text);
    if (err != ESP_OK) {
        t->text[0] = '\0';
        return false;
    }
    t->queued = true;
    t->after_id = newest;
    t->state = HH_MSG_PENDING;
    u->last_urgent_ms = now;
    u->sent_any = true;
    return true;
}

static bool gap_ok(uint32_t now)
{
    return !u->sent_any || now - u->last_urgent_ms >= URGENT_GAP_MS;
}

/* " near MAIN at 38.86593, -94.68011", " near NORTH", ", last near SOUTH", or "". */
static void where_words(const hh_status_t *st, char *out, size_t cap)
{
    out[0] = '\0';
    if (st->link == HH_LINK_ONLINE && st->node_ssid[0] != '\0' && st->node >= 0) {
        hh_position_t p;
        char coords[32];
        if (hh_service_position(LG_NODE_ID_BASE | (uint32_t)st->node, &p) && p.valid) {
            ui_geo_coord_text(p.lat_u, p.lon_u, coords, sizeof(coords));
            snprintf(out, cap, " near %s at %s", st->node_ssid, coords);
        } else {
            snprintf(out, cap, " near %s", st->node_ssid);
        }
    } else if (u->last_ap[0] != '\0') {
        snprintf(out, cap, ", last near %s", u->last_ap);
    }
}

static void sos_send(uint32_t now)
{
    const hh_status_t *st = ui_status();
    char where[HH_SSID_MAX + 48];
    where_words(st, where, sizeof(where));
    char text[HH_TEXT_MAX + 1];
    if (u->repeat == 0) {
        snprintf(text, sizeof(text), "SOS from %s%s", st->name, where);
    } else {
        snprintf(text, sizeof(text), "SOS (repeat %u) from %s%s", u->repeat, st->name, where);
    }
    if (!urgent_send(&u->last, text, now, false)) {
        u->next_ms = now + 1000u;   /* the service's queue was full: try again shortly */
        ESP_LOGW(TAG, "[MSG] SOS could not be queued; trying again");
        return;
    }
    u->next_ms = 0;
    ESP_LOGI(TAG, "[MSG] SOS queued: %s", text);
}

static void track(uint32_t id)
{
    u->ids[u->ids_next] = id;
    u->ids_next = (uint8_t)((u->ids_next + 1u) % SOS_TRACK);
    if (u->n_ids < SOS_TRACK) {
        u->n_ids++;
    }
}

/* Finds our messages in the service's list: their ids, states, and readers. */
static void scan(void)
{
    for (size_t i = 0; i < HH_MESSAGES && hh_service_message(i, &u->m); i++) {
        const hh_message_t *m = &u->m;
        if (!m->mine || !m->urgent || m->scope != LG_SCOPE_BROADCAST) {
            continue;
        }
        sent_t *both[2] = { &u->last, &u->safe };
        for (int k = 0; k < 2; k++) {
            sent_t *t = both[k];
            if (t->queued && m->id > t->after_id && strcmp(m->text, t->text) == 0) {
                t->queued = false;
                t->id = m->id;
                if (t == &u->last && u->sos_active) {
                    track(m->id);
                }
            }
            if (t->id != 0 && t->id == m->id) {
                t->state = m->state;
                t->reject = m->reject;
                t->read_mask = m->read_mask;
            }
        }
        for (uint8_t k = 0; k < u->n_ids; k++) {
            if (u->ids[k] == m->id) {
                u->mask |= m->read_mask;   /* readers stay counted after the message leaves the list */
            }
        }
    }
}

static void sos_engine(uint32_t now)
{
    if (!u->sos_active || u->last.queued) {
        return;
    }
    if (u->mask != 0) {
        u->next_ms = 0;   /* someone has read it: no more repeats */
        return;
    }
    if (u->last.id != 0 && (u->sched_id != u->last.id || u->sched_state != u->last.state)) {
        u->sched_id = u->last.id;
        u->sched_state = u->last.state;
        switch (u->last.state) {
        case HH_MSG_PENDING:
            u->next_ms = 0;   /* no AP has it yet; the client keeps offering it */
            break;
        case HH_MSG_REJECTED:
            u->retry_same = true;
            if (u->last.reject == LG_ACK_REJ_RATE && u->rate_retries < RATE_RETRIES_MAX) {
                u->rate_retries++;
                u->next_ms = now + URGENT_GAP_MS;
                ESP_LOGW(TAG, "[MSG] The AP refused the SOS for rate; again in %u ms", (unsigned)URGENT_GAP_MS);
            } else {
                u->next_ms = now + SOS_REPEAT_MS;
                ESP_LOGW(TAG, "[MSG] The AP refused the SOS (status %u); again in %u s", u->last.reject,
                         (unsigned)(SOS_REPEAT_MS / 1000u));
            }
            break;
        case HH_MSG_REFUSED:
            u->retry_same = true;
            u->next_ms = now + SOS_REPEAT_MS;
            break;
        default:   /* an AP took it */
            u->accepted_any = true;
            u->retry_same = false;
            u->rate_retries = 0;
            u->next_ms = now + SOS_REPEAT_MS;
            break;
        }
    }
    if (u->next_ms != 0 && (int32_t)(now - u->next_ms) >= 0 && gap_ok(now)) {
        if (!u->retry_same) {
            u->repeat++;
        }
        u->retry_same = false;
        sos_send(now);
    }
}

static void safe_engine(uint32_t now)
{
    const sent_t *t = &u->safe;
    if (!t->queued && t->id != 0 && (u->safe_sched_id != t->id || u->safe_sched_state != t->state)) {
        u->safe_sched_id = t->id;
        u->safe_sched_state = t->state;
        if (t->state == HH_MSG_REJECTED && t->reject == LG_ACK_REJ_RATE && u->safe_retries < RATE_RETRIES_MAX) {
            u->safe_retries++;
            u->safe_due_ms = now + URGENT_GAP_MS;
        }
    }
    if (u->safe_due_ms != 0 && (int32_t)(now - u->safe_due_ms) >= 0 && gap_ok(now)) {
        char text[HH_TEXT_MAX + 1];
        snprintf(text, sizeof(text), "%s is safe", ui_status()->name);
        if (urgent_send(&u->safe, text, now, true)) {
            u->safe_due_ms = 0;
            ESP_LOGI(TAG, "[MSG] Safe message queued: %s", text);
        } else {
            u->safe_due_ms = now + 1000u;
        }
    }
}

/* ---- the actions ---- */

static void start_countdown(uint32_t now)
{
    u->count_start = now;
    ESP_LOGI(TAG, "[MSG] SOS countdown started");
    go(U_COUNTDOWN, now, true);
}

static void cancel_countdown(uint32_t now)
{
    ESP_LOGI(TAG, "[MSG] SOS cancelled before it was sent");
    go(U_IDLE, now, true);
}

static void raise_sos(uint32_t now, bool draw)
{
    u->sos_active = true;
    u->repeat = 0;
    u->n_ids = 0;
    u->ids_next = 0;
    u->mask = 0;
    u->rate_retries = 0;
    u->sched_id = 0;
    memset(&u->last, 0, sizeof(u->last));
    u->retry_same = true;   /* the first send is number 0 */
    u->next_ms = now;       /* at once, unless an urgent went out too recently */
    u->accepted_any = false;
    ESP_LOGI(TAG, "[MSG] SOS raised");
    go(U_ACTIVE, now, draw);
}

static void say_safe(uint32_t now)
{
    u->sos_active = false;
    u->next_ms = 0;
    memset(&u->safe, 0, sizeof(u->safe));
    u->safe_retries = 0;
    u->safe_sched_id = 0;
    u->safe_due_ms = now;
    ESP_LOGI(TAG, "[MSG] I'm safe: the SOS is over, %u read it", (unsigned)__builtin_popcount(u->mask));
    go(U_IDLE, now, true);
}

/* A hold or a press with these action bits, on whatever is showing. */
static void act(uint8_t bits, bool hold, uint32_t now)
{
    switch (u->state) {
    case U_IDLE:
        if (hold && (bits & LG_ACT_SOS)) {
            start_countdown(now);
        }
        break;
    case U_COUNTDOWN:
        if (!hold && (bits & LG_ACT_CANCEL)) {
            cancel_countdown(now);
        }
        break;
    case U_ACTIVE:
        if (hold && (bits & LG_ACT_SAFE)) {
            say_safe(now);
        }
        break;
    }
}

/* ---- refresh ---- */

/*
 * The watch face: local time in the grid's zone (D67) from the system clock, which the service keeps
 * on grid time, so the colon blinks on the second rather than on the service's once-a-second snapshot.
 * The snapshot says whether the time may be shown at all (grid time known) and where it came from.
 */
static void refresh_face(const hh_status_t *st)
{
    static const char *const DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now = time(NULL);
    if (st->grid_time == 0 || now < (time_t)LG_POS_TIME_MIN) {
        face_show("----", true, C_MUTED, FACE_GPS_NONE);
        put_font(&u->date, F_SMALL, C_WARNING, "no grid time: urgent only");
        return;
    }
    struct tm lt;
    hh_local_time((uint32_t)now, &lt);
    char digits[4] = { (char)('0' + lt.tm_hour / 10), (char)('0' + lt.tm_hour % 10), (char)('0' + lt.tm_min / 10),
                       (char)('0' + lt.tm_min % 10) };
    face_show(digits, lt.tm_sec % 2 == 0, C_TEXT, st->time_from_gps ? FACE_GPS_ON : FACE_GPS_OFF);
    char date[32];
    snprintf(date, sizeof(date), "%s %d %s%s", DAYS[lt.tm_wday % 7], lt.tm_mday, MONTHS[lt.tm_mon % 12],
             st->time_zone[0] != '\0' ? "" : " UTC");   /* no zone from the grid yet: say so */
    put_font(&u->date, F_BODY, C_MUTED, date);
}

static void refresh_idle(const hh_status_t *st)
{
    char text[WORDS_MAX + HH_SSID_MAX];
    put(&u->name, C_TEXT, st->name);
    int8_t pct = hh_service_battery_percent();
    if (pct < 0) {
        put(&u->batt, C_MUTED, "");
    } else {
        snprintf(text, sizeof(text), "%d%%", pct);
        put(&u->batt, pct <= HH_BATTERY_LOW_PERCENT ? C_ERROR : C_MUTED, text);
    }
    refresh_face(st);
    switch (st->link) {
    case HH_LINK_ONLINE:
        snprintf(text, sizeof(text), "On %s", st->node_ssid);
        put(&u->ap, C_ACCENT, text);
        snprintf(text, sizeof(text), "%d dBm", st->rssi);
        put2(&u->ap_note, &u->ap_note2, C_MUTED, text);
        break;
    case HH_LINK_CONNECTING:
    case HH_LINK_REGISTERING:
        snprintf(text, sizeof(text), "Joining %s", st->node_ssid);
        put(&u->ap, C_WARNING, text);
        put2(&u->ap_note, &u->ap_note2, C_MUTED, "");
        break;
    default:
        put(&u->ap, C_WARNING, st->n_nodes == 0 ? "No AP in range" : "Looking for an AP");
        put2(&u->ap_note, &u->ap_note2, st->problem[0] ? C_ERROR : C_MUTED, st->problem);
        break;
    }
    /* The last "I'm safe": how it went. */
    const sent_t *t = &u->safe;
    if (t->text[0] == '\0' && u->safe_due_ms == 0) {
        put2(&u->notice, &u->notice2, C_ACCENT, "");
    } else if (u->safe_due_ms != 0 || t->queued || t->state == HH_MSG_PENDING) {
        put2(&u->notice, &u->notice2, C_WARNING, "Telling everyone you're safe...");
    } else if (t->state == HH_MSG_REJECTED || t->state == HH_MSG_REFUSED) {
        put2(&u->notice, &u->notice2, C_ERROR, "Could not tell everyone you're safe");
    } else {
        /* No reader count here: under "I'm safe" a number read as people who had seen the person, when
         * it counted readers of the message (owner, 2026-09-18). An AP took it, so it went to everyone. */
        put2(&u->notice, &u->notice2, C_ACCENT, "You're marked safe. Everyone was told.");
    }
}

static void refresh_countdown(uint32_t now)
{
    uint32_t gone = (now - u->count_start) / 1000u;
    if (gone >= COUNTDOWN_S) {
        raise_sos(now, true);
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%u", (unsigned)(COUNTDOWN_S - gone));
    put(&u->line1, C_BG, text);
}

static void refresh_active(uint32_t now)
{
    char text[WORDS_MAX];
    unsigned seen = (unsigned)__builtin_popcount(u->mask);
    const sent_t *t = &u->last;
    put(&u->title, C_BG, u->accepted_any ? "SOS sent" : "SOS");
    if (seen > 0) {
        put(&u->line1, C_ACCENT, "Read: repeats stopped");
    } else if (t->queued || (t->state == HH_MSG_PENDING && t->id != 0)) {
        put(&u->line1, C_WARNING, "Waiting for an AP");
    } else if (t->state == HH_MSG_REJECTED && t->reject == LG_ACK_REJ_RATE) {
        put(&u->line1, C_WARNING, "AP busy: sending again");
    } else if (t->state == HH_MSG_REJECTED || t->state == HH_MSG_REFUSED) {
        uint32_t left = u->next_ms > now ? (u->next_ms - now + 999u) / 1000u : 0;
        snprintf(text, sizeof(text), "Refused: again in %" PRIu32 " s", left);
        put(&u->line1, C_ERROR, text);
    } else if (u->next_ms != 0) {
        uint32_t left = u->next_ms > now ? (u->next_ms - now + 999u) / 1000u : 0;
        snprintf(text, sizeof(text), "Sent %u, again in %" PRIu32 " s", u->repeat + 1u, left);
        put(&u->line1, C_TEXT, text);
    } else {
        put(&u->line1, C_TEXT, "Sending");
    }
    snprintf(text, sizeof(text), "%u", seen);
    put(&u->seen, seen ? C_ACCENT : C_MUTED, text);
    char names[LG_BOX_TEXT_MAX];
    uint8_t named = hh_service_reader_names(u->mask, names, sizeof(names));
    if (named < seen && seen > 0) {
        snprintf(names, sizeof(names), "%u handhelds", seen);   /* the names did not fit */
    }
    put(&u->names, C_TEXT, names);
}

static void refresh(uint32_t now)
{
    if (u->refresh_ms != 0 && now - u->refresh_ms < REFRESH_MS) {
        return;
    }
    u->refresh_ms = now;
    switch (u->state) {
    case U_IDLE:      refresh_idle(ui_status()); break;
    case U_COUNTDOWN: refresh_countdown(now); break;
    case U_ACTIVE:    refresh_active(now); break;
    }
}

/* ---- public ---- */

void ui_unit_start(const lg_board_t *board, uint16_t w, uint16_t h, bool touch)
{
    u = calloc(1, sizeof(*u));
    if (u == NULL) {
        ESP_LOGE(TAG, "[UI] No memory for the alert unit's screens");
        return;
    }
    u->board = board;
    u->w = w;
    u->h = h;
    u->touch = touch;
    u->btn_down = -1;
    if (lg_bsp_button_start(board, HOLD_MS) != ESP_OK && !touch) {
        ESP_LOGE(TAG, "[UI] This alert unit has neither touch nor a button: SOS only from the console");
    }
    ESP_LOGI(TAG, "[UI] Alert unit: %s, %u button(s)", touch ? "touch" : "no touch", lg_bsp_button_count());
    go(U_IDLE, 0, true);
}

void ui_unit_redraw(void)
{
    if (u != NULL) {
        draw_all();
        u->refresh_ms = 0;
    }
}

void ui_unit_tick(uint32_t now, bool covered)
{
    if (u == NULL) {
        return;
    }
    const hh_status_t *st = ui_status();
    if (st->link == HH_LINK_ONLINE && st->node_ssid[0] != '\0') {
        snprintf(u->last_ap, sizeof(u->last_ap), "%s", st->node_ssid);
    }
    if (now - u->scan_ms >= SCAN_MS) {
        u->scan_ms = now;
        scan();
        sos_engine(now);
        safe_engine(now);
    }
    if (covered) {
        /* An alert owns the panel. The countdown still runs out and the SOS still goes; its
         * screen is painted when the alert is read. */
        if (u->state == U_COUNTDOWN && now - u->count_start >= COUNTDOWN_S * 1000u) {
            raise_sos(now, false);
        }
        return;
    }
    if (u->state == U_COUNTDOWN) {
        refresh_countdown(now);   /* every loop, so the send is not late */
        if (u->state != U_COUNTDOWN) {
            return;
        }
    }
    uint32_t held = 0;
    if (u->touch_hold) {
        held = now - u->touch_since;
        if (held >= HOLD_MS) {
            u->touch_hold = false;
            u->need_lift = true;
            act(LG_ACT_SOS | LG_ACT_SAFE, true, now);
            return;
        }
    } else if (u->btn_down >= 0) {
        held = lg_bsp_button_held_ms((uint8_t)u->btn_down, now);
    }
    if (u->state != U_COUNTDOWN) {
        draw_progress(held);
    }
    refresh(now);
}

void ui_unit_touch(int16_t x, int16_t y, bool down, uint32_t now)
{
    if (u == NULL) {
        return;
    }
    if (!down) {
        u->need_lift = false;
        u->touch_hold = false;
        return;
    }
    if (u->need_lift) {
        return;
    }
    bool in = lg_rect_hit(&u->action, x, y);
    if (u->state == U_COUNTDOWN) {
        if (in) {
            u->need_lift = true;     /* the same finger must not start a hold on the SOS under it */
            cancel_countdown(now);   /* on the press itself: nobody should have to aim twice */
        }
        return;
    }
    if (in && !u->touch_hold) {
        u->touch_hold = true;
        u->touch_since = now;
    } else if (!in) {
        u->touch_hold = false;   /* slid off: that is not a hold */
    }
}

void ui_unit_button_down(uint8_t id, bool down, uint32_t now)
{
    (void)now;
    if (u == NULL || id >= u->board->n_buttons) {
        return;
    }
    uint8_t hold_bits = u->board->buttons[id].on_hold;
    bool matters = (u->state == U_IDLE && (hold_bits & LG_ACT_SOS)) || (u->state == U_ACTIVE && (hold_bits & LG_ACT_SAFE));
    if (down && matters && !ui_overlay_covering()) {
        u->btn_down = (int8_t)id;
    } else if (!down && u->btn_down == (int8_t)id) {
        u->btn_down = -1;
    }
}

void ui_unit_button(uint8_t id, bool hold, uint32_t now)
{
    if (u == NULL || id >= u->board->n_buttons) {
        return;
    }
    const lg_button_profile_t *b = &u->board->buttons[id];
    uint8_t bits = hold ? b->on_hold : b->on_press;
    ESP_LOGI(TAG, "[UI] Button %u %s", id, hold ? "held" : "pressed");
    if (ui_overlay_alert_showing()) {
        if (!hold && (bits & LG_ACT_READ)) {
            (void)ui_overlay_ack();   /* read, as the Read button would */
        } else if (hold && (bits & LG_ACT_SOS) && u->state == U_IDLE) {
            (void)ui_overlay_ack();   /* an emergency of our own comes first; the alert was seen */
            start_countdown(now);
        }
        return;
    }
    if (ui_overlay_covering()) {
        return;
    }
    act(bits, hold, now);
}

void ui_unit_log(void)
{
    if (u == NULL) {
        return;
    }
    ESP_LOGI(TAG, "[UI] Alert unit: screen %d, SOS %s, repeat %u, seen by %u, next in %" PRId32 " ms, last state %u "
             "reject %u, rate retries %u",
             (int)u->state, u->sos_active ? "on" : "off", u->repeat, (unsigned)__builtin_popcount(u->mask),
             u->next_ms ? (int32_t)(u->next_ms - (uint32_t)(esp_timer_get_time() / 1000)) : -1, u->last.state,
             u->last.reject,
             u->rate_retries);
}
