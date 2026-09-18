/*
 * The top bar's battery badge and padlock, and the lock screen (D62).
 *
 * The behaviour and the numbers follow the owner's Braino project (see THIRD_PARTY.md), restated
 * for lg_draw: the badge is a battery shell carrying the charge as digits, with a two-pixel gauge
 * along its inside bottom and a terminal nub on the right, drawn in the error colour at 15 % and
 * below; the padlock is a shackle, a body, and a keyhole. The lock is released by holding one
 * button for 0.9 s; a finger that lifts for up to 150 ms (a resistive panel does, mid-press)
 * does not restart the hold; after 12 s without anyone unlocking, the screen saver takes over.
 * The progress bar under the button is the only thing the lock screen repaints while it is up,
 * and only when its width changes.
 *
 * Nothing here is allocated, and every size is taken from the panel at run time.
 */
#include "ui_lock.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "ui_nav.h"
#include "ui_overlay.h"
#include "ui_theme.h"

static const char *TAG = "UI";

#define LOCK_HOLD_MS      900
#define LOCK_GRACE_MS     150
#define LOCK_TIMEOUT_MS   12000
#define HIT_SLOP          6      /* the hold button answers a little outside its edge */

/* The bar's cluster, right to left: the padlock's touch target, a gap, the battery slot. */
#define BAR_LOCK_W        32
#define BAR_GAP           4
#define BAR_SPARE         22     /* kept free left of the badge for the talking indicator */
#define BAR_HOME_W        32     /* the house at the header's left edge */
#define PADLOCK_W         14
#define PADLOCK_H         18

/* The battery badge. */
#define BATT_H            15     /* shell height, outline included */
#define BATT_PAD          3      /* inside the shell, each side of the digits */
#define BATT_NUB_W        2
#define BATT_MIN_INNER    11
#define BATT_TRACK_H      4      /* the gauge: a one-pixel border round two rows of fill */
#define BATT_AMBER_PCT    40

/* The lock screen. */
#define HEAD_H            44     /* the hairline under the wordmark and battery */
#define BUTTON_H          58
#define BUTTON_W_MAX      200
#define BAR_H             10
#define GLYPH             24     /* the padlock beside "Locked" */

static float clampf(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ---- the brand mark ---- */

static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax;
    float vy = by - ay;
    float t = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy));
    float dx = px - (ax + t * vx);
    float dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

/*
 * The LocalGrid mark (assets/brand/localgrid-icon.svg, as on the admin page), drawn pixel by
 * pixel from its shapes in the SVG's 64-unit grid: a rounded tile, two radio waves over the top
 * node, the tent of links between three nodes, and a handheld in the middle joined to each. Edges
 * are smoothed from each pixel's distance to the shape, so it reads at 26 px.
 */
void ui_paint_logo(const lg_canvas_t *c, int16_t x0, int16_t y0)
{
    lg_rect_t box = { x0, y0, UI_LOGO_SIZE, UI_LOGO_SIZE };
    lg_paint_panel(c, &box, &box, C_BAR, C_BG, C_BAR, 0, (uint8_t)(14 * UI_LOGO_SIZE / 64));
    float unit = 64.0f / UI_LOGO_SIZE;   /* grid units per pixel */
    for (int16_t py = 0; py < UI_LOGO_SIZE; py++) {
        int16_t sy = (int16_t)(y0 + py);
        if (sy < c->band.y || sy >= c->band.y + c->band.h) {
            continue;
        }
        for (int16_t px = 0; px < UI_LOGO_SIZE; px++) {
            float u = (px + 0.5f) * unit;
            float v = (py + 0.5f) * unit;
            int16_t sx = (int16_t)(x0 + px);
            /* Waves: the upper quarter of two circles around the top node. */
            float dx = u - 32.0f;
            float dy = v - 22.0f;
            if (dy < 0.0f && fabsf(dx) <= -dy) {
                float r = sqrtf(dx * dx + dy * dy);
                float a = clampf((1.5f - fabsf(r - 11.0f)) / unit + 0.5f) * 0.9f;
                a += clampf((1.5f - fabsf(r - 18.0f)) / unit + 0.5f) * 0.5f;
                lg_paint_pixel(c, &box, sx, sy, C_ACCENT, (uint8_t)(clampf(a) * 255.0f));
            }
            /* The tent: links between the three nodes. */
            float d = segment_distance(u, v, 32, 22, 14, 50);
            float e = segment_distance(u, v, 14, 50, 50, 50);
            float f = segment_distance(u, v, 50, 50, 32, 22);
            d = d < e ? d : e;
            d = d < f ? d : f;
            lg_paint_pixel(c, &box, sx, sy, C_ACCENT, (uint8_t)(clampf((1.5f - d) / unit + 0.5f) * 255.0f));
            /* The handheld's links to each node, fainter. */
            d = segment_distance(u, v, 32, 22, 32, 39);
            e = segment_distance(u, v, 14, 50, 32, 39);
            f = segment_distance(u, v, 50, 50, 32, 39);
            d = d < e ? d : e;
            d = d < f ? d : f;
            lg_paint_pixel(c, &box, sx, sy, C_ACCENT, (uint8_t)(clampf((1.0f - d) / unit + 0.5f) * 0.6f * 255.0f));
            /* Nodes. */
            static const float NODES[3][2] = { { 32, 22 }, { 14, 50 }, { 50, 50 } };
            for (int n = 0; n < 3; n++) {
                float nx = u - NODES[n][0];
                float ny = v - NODES[n][1];
                float dn = sqrtf(nx * nx + ny * ny);
                lg_paint_pixel(c, &box, sx, sy, C_ACCENT, (uint8_t)(clampf((4.5f - dn) / unit + 0.5f) * 255.0f));
            }
            /* The handheld: a hollow ring. */
            float hx = u - 32.0f;
            float hy = v - 39.0f;
            float dh = sqrtf(hx * hx + hy * hy);
            lg_paint_pixel(c, &box, sx, sy, C_BAR, (uint8_t)(clampf((3.5f - dh) / unit + 0.5f) * 255.0f));
            lg_paint_pixel(c, &box, sx, sy, C_ACCENT, (uint8_t)(clampf((1.0f - fabsf(dh - 3.5f)) / unit + 0.5f) * 255.0f));
        }
    }
}

/* ---- the padlock ---- */

/*
 * A shackle (an arch whose legs run down into the body), a body with rounded corners, and a
 * keyhole cut in the background colour. Proportions come off the rect, so one glyph serves the
 * bar and the lock screen.
 */
static void paint_padlock(const lg_canvas_t *c, const lg_rect_t *clip, const lg_rect_t *r, lg_color_t ink,
                          lg_color_t ground)
{
    int16_t body_w = (int16_t)(r->w - r->w / 4);
    body_w = body_w < 8 ? 8 : body_w;
    int16_t body_h = (int16_t)(r->h * 5 / 11);
    body_h = body_h < 7 ? 7 : body_h;
    int16_t body_x = (int16_t)(r->x + (r->w - body_w) / 2);
    int16_t body_y = (int16_t)(r->y + r->h - body_h - r->h / 10);
    float cx = (float)body_x + body_w / 2.0f;
    float stroke = r->w / 6.0f < 2.0f ? 2.0f : r->w / 6.0f;
    float radius = (body_w - body_w / 3) / 2.0f - stroke / 2.0f;   /* the shackle's centre line */
    float arc_cy = (float)r->y + stroke / 2.0f + radius;
    for (int16_t py = r->y; py <= body_y + 1; py++) {
        if (py < c->band.y || py >= c->band.y + c->band.h) {
            continue;
        }
        for (int16_t px = r->x; px < r->x + r->w; px++) {
            float u = px + 0.5f;
            float v = py + 0.5f;
            float d;
            if (v < arc_cy) {
                d = fabsf(sqrtf((u - cx) * (u - cx) + (v - arc_cy) * (v - arc_cy)) - radius);
            } else {
                float left = fabsf(u - (cx - radius));
                float right = fabsf(u - (cx + radius));
                d = left < right ? left : right;
            }
            lg_paint_pixel(c, clip, px, py, ink, (uint8_t)(clampf(stroke / 2.0f + 0.5f - d) * 255.0f));
        }
    }
    lg_rect_t body = { body_x, body_y, body_w, body_h };
    lg_paint_panel(c, clip, &body, ink, ground, ink, 0, 2);
    float key_r = body_w / 8.0f < 1.2f ? 1.2f : body_w / 8.0f;
    float key_cy = (float)body_y + body_h / 2.0f - key_r / 2.0f;
    for (int16_t py = body_y; py < body_y + body_h; py++) {
        for (int16_t px = body_x; px < body_x + body_w; px++) {
            float u = px + 0.5f - cx;
            float v = py + 0.5f - key_cy;
            float a = clampf(key_r + 0.5f - sqrtf(u * u + v * v));
            if (v >= 0.0f && v <= body_h / 3.0f + 0.5f) {
                a = fmaxf(a, clampf(key_r / 2.0f + 0.5f - fabsf(u)));   /* the slot under the hole */
            }
            if (a > 0.0f) {
                lg_paint_pixel(c, clip, px, py, ground, (uint8_t)(a * 255.0f));
            }
        }
    }
}

/* ---- the battery badge ---- */

static void battery_text(char *out, size_t cap, int8_t pct)
{
    if (pct < 0) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%d", pct > 100 ? 100 : pct);   /* no % sign: iOS leaves it out of the shell too */
}

static int16_t shell_width(const char *text)
{
    int16_t inner = (int16_t)(2 * BATT_PAD + lg_draw_text_width(F_TINY, NULL, text));
    return (int16_t)((inner < BATT_MIN_INNER ? BATT_MIN_INNER : inner) + 2);
}

/* The widest badge, "100": the slot every layout reserves, so nothing moves when a digit goes. */
static int16_t battery_slot_width(void)
{
    return (int16_t)(shell_width("100") + BATT_NUB_W);
}

/* Fills slot and draws the badge right-aligned in it, vertically centred; nothing for -1. */
static void paint_battery(const lg_canvas_t *c, const lg_rect_t *slot, int8_t pct)
{
    lg_paint_panel(c, slot, slot, C_BG, C_BG, C_BG, 0, 0);
    if (pct < 0) {
        return;   /* no badge at all: an empty shell would read as a flat battery */
    }
    char text[8];
    battery_text(text, sizeof(text), pct);
    bool low = pct <= HH_BATTERY_LOW_PERCENT;
    lg_color_t ink = low ? C_ERROR : C_TEXT;   /* red only when it is low, so it still means something */
    lg_color_t level = low ? C_ERROR : (pct <= BATT_AMBER_PCT ? C_WARNING : C_ACCENT);
    int16_t shell_w = shell_width(text);
    int16_t bx = (int16_t)(slot->x + slot->w - shell_w - BATT_NUB_W);
    int16_t cy = (int16_t)(slot->y + slot->h / 2);
    int16_t by = (int16_t)(cy - BATT_H / 2);
    lg_rect_t shell = { bx, by, shell_w, BATT_H };
    lg_paint_panel(c, slot, &shell, C_BG, C_BG, ink, 1, 2);
    lg_rect_t nub = { (int16_t)(bx + shell_w), (int16_t)(cy - 3), 1, 7 };
    lg_paint_panel(c, slot, &nub, ink, ink, ink, 0, 0);
    nub = (lg_rect_t){ (int16_t)(bx + shell_w + 1), (int16_t)(cy - 2), 1, 5 };
    lg_paint_panel(c, slot, &nub, ink, ink, ink, 0, 0);
    lg_rect_t track = { (int16_t)(bx + 2), (int16_t)(by + BATT_H - 1 - BATT_TRACK_H), (int16_t)(shell_w - 4),
                        BATT_TRACK_H };
    lg_paint_panel(c, slot, &track, C_BG, C_BG, C_OUTLINE, 1, 0);
    if (pct > 0) {
        int16_t room = (int16_t)(track.w - 2);
        int16_t fill_w = (int16_t)(pct * room / 100);
        lg_rect_t fill = { (int16_t)(track.x + 1), (int16_t)(track.y + 1), fill_w > 0 ? fill_w : 1,
                           (int16_t)(BATT_TRACK_H - 2) };
        lg_paint_panel(c, slot, &fill, level, level, level, 0, 0);
    }
    /* The digits sit above the gauge: the font's base line lands on the track's top border. */
    int16_t line_top = (int16_t)(track.y - (F_TINY->line_height - F_TINY->base_line));
    lg_paint_text(c, slot, (int16_t)(bx + 1 + BATT_PAD), line_top, F_TINY, NULL, ink, text, strlen(text));
}

/* ---- the top bar: home at the left, the battery and padlock at the right ---- */

typedef enum { PRESS_NONE, PRESS_LOCK, PRESS_HOME } bar_press_t;

static struct {
    bool      placed;
    bool      home_placed;
    uint8_t   pressed;      /* bar_press_t: a press that started on one of the bar's buttons */
    bool      touching;     /* any press in progress, to tell where it started */
    int8_t    shown;        /* the charge the badge shows */
    lg_rect_t all;
    lg_rect_t battery;
    lg_rect_t lock;
    lg_rect_t home;
} s_bar;

int16_t ui_bar_place(int16_t right, int16_t y, int16_t h)
{
    int16_t slot_w = battery_slot_width();
    s_bar.lock = (lg_rect_t){ (int16_t)(right - BAR_LOCK_W), y, BAR_LOCK_W, h };
    s_bar.battery = (lg_rect_t){ (int16_t)(s_bar.lock.x - BAR_GAP - slot_w), y, slot_w, h };
    s_bar.all = (lg_rect_t){ s_bar.battery.x, y, (int16_t)(right - s_bar.battery.x), h };
    s_bar.placed = true;
    s_bar.home_placed = false;   /* each screen asks for home again, after placing the bar */
    s_bar.pressed = PRESS_NONE;
    s_bar.touching = false;
    return (int16_t)(s_bar.all.x - BAR_SPARE);
}

int16_t ui_bar_home(int16_t y, int16_t h)
{
    s_bar.home = (lg_rect_t){ 0, y, BAR_HOME_W, h };
    s_bar.home_placed = true;
    return BAR_HOME_W;
}

lg_rect_t ui_bar_spare_rect(void)
{
    if (!s_bar.placed) {
        return (lg_rect_t){ 0, 0, 0, 0 };
    }
    return (lg_rect_t){ (int16_t)(s_bar.all.x - BAR_SPARE), s_bar.all.y, BAR_SPARE, s_bar.all.h };
}

/* A button's pressed look: a rounded face in the outline colour. Returns the ground under it. */
static lg_color_t paint_face(const lg_canvas_t *c, const lg_rect_t *r, bool pressed)
{
    lg_paint_panel(c, r, r, C_BG, C_BG, C_BG, 0, 0);
    if (!pressed) {
        return C_BG;
    }
    lg_rect_t face = { (int16_t)(r->x + 2), (int16_t)(r->y + 2), (int16_t)(r->w - 4), (int16_t)(r->h - 4) };
    lg_paint_panel(c, r, &face, C_OUTLINE, C_BG, C_OUTLINE, 0, 5);
    return C_OUTLINE;
}

static void paint_lock_button(const lg_canvas_t *c)
{
    lg_rect_t gap = { (int16_t)(s_bar.battery.x + s_bar.battery.w), s_bar.all.y, BAR_GAP, s_bar.all.h };
    lg_paint_panel(c, &gap, &gap, C_BG, C_BG, C_BG, 0, 0);
    lg_color_t ground = paint_face(c, &s_bar.lock, s_bar.pressed == PRESS_LOCK);
    lg_rect_t glyph = { (int16_t)(s_bar.lock.x + (s_bar.lock.w - PADLOCK_W) / 2),
                        (int16_t)(s_bar.lock.y + (s_bar.lock.h - PADLOCK_H) / 2), PADLOCK_W, PADLOCK_H };
    paint_padlock(c, &s_bar.lock, &glyph, C_ACCENT, ground);
}

static void paint_home_button(const lg_canvas_t *c)
{
    (void)paint_face(c, &s_bar.home, s_bar.pressed == PRESS_HOME);
    const char *house = LG_SYMBOL_HOME;
    int16_t tw = lg_draw_text_width(F_ICON, NULL, house);
    lg_paint_text(c, &s_bar.home, (int16_t)(s_bar.home.x + (s_bar.home.w - tw) / 2),
                  (int16_t)(s_bar.home.y + (s_bar.home.h - F_ICON->line_height) / 2), F_ICON, NULL, C_ACCENT, house,
                  strlen(house));
}

void ui_bar_paint(const lg_canvas_t *c)
{
    if (!s_bar.placed) {
        return;
    }
    s_bar.shown = hh_service_battery_percent();
    paint_battery(c, &s_bar.battery, s_bar.shown);
    paint_lock_button(c);
    if (s_bar.home_placed) {
        paint_home_button(c);
    }
}

static void bar_painter(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    ui_bar_paint(c);
}

void ui_bar_draw(void)
{
    if (!s_bar.placed) {
        return;
    }
    lg_draw_region(&s_bar.all, bar_painter, NULL);
    if (s_bar.home_placed) {
        lg_draw_region(&s_bar.home, bar_painter, NULL);   /* the painter clips to each band */
    }
}

static void battery_painter(const lg_canvas_t *c, void *ctx)
{
    paint_battery(c, (const lg_rect_t *)ctx, s_bar.shown);
}

static void button_painter(const lg_canvas_t *c, void *ctx)
{
    if (ctx == &s_bar.home) {
        paint_home_button(c);
    } else {
        paint_lock_button(c);
    }
}

/* Repaints the lock (with the gap beside it) or the home button alone. */
static void draw_button(bar_press_t which)
{
    if (which == PRESS_HOME) {
        lg_draw_region(&s_bar.home, button_painter, &s_bar.home);
    } else {
        lg_rect_t r = { (int16_t)(s_bar.battery.x + s_bar.battery.w), s_bar.all.y, (int16_t)(BAR_GAP + BAR_LOCK_W),
                        s_bar.all.h };
        lg_draw_region(&r, button_painter, NULL);
    }
}

bool ui_bar_refresh(void)
{
    int8_t pct = hh_service_battery_percent();
    if (!s_bar.placed || ui_lock_active() || pct == s_bar.shown) {
        return false;
    }
    s_bar.shown = pct;
    lg_draw_region(&s_bar.battery, battery_painter, &s_bar.battery);
    return true;
}

bool ui_bar_touch(int16_t x, int16_t y, bool down)
{
    if (!s_bar.placed || ui_lock_active()) {
        s_bar.touching = down;
        return false;
    }
    if (down) {
        if (!s_bar.touching) {
            s_bar.touching = true;
            if (lg_rect_hit(&s_bar.lock, x, y)) {
                s_bar.pressed = PRESS_LOCK;
            } else if (s_bar.home_placed && lg_rect_hit(&s_bar.home, x, y)) {
                s_bar.pressed = PRESS_HOME;
            }
            if (s_bar.pressed != PRESS_NONE) {
                draw_button((bar_press_t)s_bar.pressed);   /* the pressed look */
            }
        }
        return s_bar.pressed != PRESS_NONE;
    }
    s_bar.touching = false;
    bar_press_t was = (bar_press_t)s_bar.pressed;
    if (was == PRESS_NONE) {
        return false;
    }
    s_bar.pressed = PRESS_NONE;
    if (was == PRESS_LOCK && lg_rect_hit(&s_bar.lock, x, y)) {
        ui_lock_engage();
    } else if (was == PRESS_HOME && lg_rect_hit(&s_bar.home, x, y)) {
        ESP_LOGI(TAG, "[UI] Home tapped");
        ui_go(NAV_HOME, 0, 0, NULL);   /* the launcher repaints the whole panel */
    } else {
        draw_button(was);   /* slid off: back to the plain look */
    }
    return true;
}

/* ---- the lock screen ---- */

static const char *const FOOTERS[] = {
    "Nothing under here can be touched while locked",
    "Nothing under here can be touched",
    "Nothing below can be touched",
};

static struct {
    uint16_t  w;
    uint16_t  h;
    bool      locked;
    bool      blocked;       /* the finger that was down when the screen was painted has not lifted */
    bool      holding;
    uint32_t  hold_start;
    uint32_t  contact;       /* the last sample with the finger on the button */
    uint32_t  activity;      /* the last touch, for the hand-over to the saver */
    bool      fresh;         /* just painted: the next tick starts the 12 s from its own clock */
    int16_t   painted_pct;   /* the progress bar's fill as painted; -1 forces a paint */
    int8_t    battery_shown;
    lg_rect_t header_battery;
    lg_rect_t button;
    lg_rect_t bar;
} s_lock;

void ui_lock_start(uint16_t w, uint16_t h)
{
    s_lock.w = w;
    s_lock.h = h;
    int16_t bw = (int16_t)(w - 48 < BUTTON_W_MAX ? w - 48 : BUTTON_W_MAX);
    /* Centred a little below the middle, so the heading and hint fit above it on any panel. */
    s_lock.button = (lg_rect_t){ (int16_t)((w - bw) / 2), (int16_t)((h - BUTTON_H) / 2 + 37), bw, BUTTON_H };
    s_lock.bar = (lg_rect_t){ s_lock.button.x, (int16_t)(s_lock.button.y + BUTTON_H + 10), bw, BAR_H };
    int16_t slot_w = battery_slot_width();
    s_lock.header_battery = (lg_rect_t){ (int16_t)(w - UI_PAD - 2 - slot_w), UI_PAD, slot_w, 30 };
}

bool ui_lock_active(void)
{
    return s_lock.locked;
}

static void centred(const lg_canvas_t *c, const lg_rect_t *clip, const lg_font_t *f, lg_color_t fg, int16_t y,
                    const char *text)
{
    int16_t tw = lg_draw_text_width(f, NULL, text);
    lg_paint_text(c, clip, (int16_t)((s_lock.w - tw) / 2), y, f, NULL, fg, text, strlen(text));
}

static void paint_lock_screen(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t all = { 0, 0, (int16_t)s_lock.w, (int16_t)s_lock.h };
    lg_paint_panel(c, &all, &all, C_BG, C_BG, C_BG, 0, 0);
    /* The header: what this is and whether it is about to go flat, the two things anyone who finds
     * it locked wants to know without unlocking it. */
    int16_t row = s_lock.header_battery.y;
    ui_paint_logo(c, UI_PAD, (int16_t)(row + (30 - UI_LOGO_SIZE) / 2));
    lg_paint_text(c, &all, (int16_t)(UI_PAD + UI_LOGO_SIZE + 6), (int16_t)(row + (30 - F_TITLE->line_height) / 2),
                  F_TITLE, NULL, C_ACCENT, "LocalGrid", 9);
    paint_battery(c, &s_lock.header_battery, s_lock.battery_shown);
    lg_rect_t hair = { UI_PAD, HEAD_H, (int16_t)(s_lock.w - 2 * UI_PAD), 1 };
    lg_paint_panel(c, &all, &hair, C_OUTLINE, C_OUTLINE, C_OUTLINE, 0, 0);
    /* The padlock and the word as one centred group. */
    const char *word = "Locked";
    int16_t word_w = lg_draw_text_width(F_TITLE, NULL, word);
    int16_t group_x = (int16_t)((s_lock.w - GLYPH - 8 - word_w) / 2);
    int16_t locked_y = (int16_t)(s_lock.button.y - 44);
    lg_rect_t glyph = { group_x, (int16_t)(locked_y + (F_TITLE->line_height - GLYPH) / 2), GLYPH, GLYPH };
    paint_padlock(c, &all, &glyph, C_MUTED, C_BG);
    lg_paint_text(c, &all, (int16_t)(group_x + GLYPH + 8), locked_y, F_TITLE, NULL, C_TEXT, word, strlen(word));
    centred(c, &all, F_SMALL, C_MUTED, (int16_t)(s_lock.button.y - 18), "Press and hold the button");
    lg_paint_panel(c, &all, &s_lock.button, C_ACCENT, C_BG, C_ACCENT, 0, 8);
    const char *label = "Hold to unlock";
    lg_paint_text(c, &s_lock.button,
                  (int16_t)(s_lock.button.x + (s_lock.button.w - lg_draw_text_width(F_BODY, NULL, label)) / 2),
                  (int16_t)(s_lock.button.y + (s_lock.button.h - F_BODY->line_height) / 2), F_BODY, NULL,
                  C_ACCENT_INK, label, strlen(label));
    lg_paint_panel(c, &all, &s_lock.bar, C_SURFACE, C_BG, C_OUTLINE, 1, 4);
    /* The longest wording that fits whole: a sentence cut short reads as a different sentence. */
    size_t n = sizeof(FOOTERS) / sizeof(FOOTERS[0]);
    const char *footer = FOOTERS[n - 1u];
    for (size_t i = 0; i < n; i++) {
        if (lg_draw_text_width(F_SMALL, NULL, FOOTERS[i]) <= s_lock.w - 16) {
            footer = FOOTERS[i];
            break;
        }
    }
    centred(c, &all, F_SMALL, C_MUTED, (int16_t)(s_lock.h - 8 - F_SMALL->line_height), footer);
}

static lg_rect_t progress_inner(void)
{
    return (lg_rect_t){ (int16_t)(s_lock.bar.x + 2), (int16_t)(s_lock.bar.y + 2), (int16_t)(s_lock.bar.w - 4),
                        (int16_t)(s_lock.bar.h - 4) };
}

static void paint_progress(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t inner = progress_inner();
    lg_paint_panel(c, &inner, &inner, C_SURFACE, C_SURFACE, C_SURFACE, 0, 0);
    int16_t fill_w = (int16_t)(inner.w * s_lock.painted_pct / 100);
    if (fill_w > 0) {
        lg_rect_t fill = { inner.x, inner.y, fill_w, inner.h };
        lg_paint_panel(c, &inner, &fill, C_ACCENT, C_ACCENT, C_ACCENT, 0, 0);
    }
}

static void header_battery_painter(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    paint_battery(c, &s_lock.header_battery, s_lock.battery_shown);
}

void ui_lock_draw(void)
{
    s_lock.holding = false;
    s_lock.blocked = true;   /* cleared by the first sample with no finger down */
    s_lock.fresh = true;
    s_lock.painted_pct = 0;
    s_lock.battery_shown = hh_service_battery_percent();
    lg_draw_scroll_area(0, 0);   /* the lock owns the whole panel, unscrolled */
    lg_rect_t all = { 0, 0, (int16_t)s_lock.w, (int16_t)s_lock.h };
    lg_draw_region(&all, paint_lock_screen, NULL);
}

void ui_lock_engage(void)
{
    s_lock.locked = true;
    s_bar.pressed = PRESS_NONE;
    ui_overlay_drop_banner();   /* a banner would be a way past the lock: tapping it opens a chat */
    ESP_LOGI(TAG, "[UI] Locked");
    ui_lock_draw();
}

bool ui_lock_tick(uint32_t now, bool down, int16_t x, int16_t y)
{
    if (!s_lock.locked) {
        return false;
    }
    /*
     * The 12 s is counted on the caller's clock only. The lock screen is painted in the middle of
     * a loop pass (the saver's waking touch, Read on an alert, the padlock), after that pass read
     * its `now`; stamping the paint with a later clock made now - activity negative, which as an
     * unsigned number is past any timeout, so the saver came straight back over the lock screen
     * it had just been woken to: the two took turns on every touch.
     */
    if (s_lock.fresh) {
        s_lock.fresh = false;
        s_lock.activity = now;
    }
    if (!down) {
        s_lock.blocked = false;
    } else {
        s_lock.activity = now;
    }
    lg_rect_t target = { (int16_t)(s_lock.button.x - HIT_SLOP), (int16_t)(s_lock.button.y - HIT_SLOP),
                         (int16_t)(s_lock.button.w + 2 * HIT_SLOP), (int16_t)(s_lock.button.h + 2 * HIT_SLOP) };
    if (down && !s_lock.blocked && lg_rect_hit(&target, x, y)) {
        if (!s_lock.holding) {
            s_lock.holding = true;
            s_lock.hold_start = now;
        }
        s_lock.contact = now;
    } else if (s_lock.holding && now - s_lock.contact > LOCK_GRACE_MS) {
        s_lock.holding = false;   /* a gap longer than a resistive panel's dropout: the hold is over */
    }
    if (s_lock.holding && now - s_lock.hold_start >= LOCK_HOLD_MS) {
        s_lock.holding = false;
        s_lock.locked = false;
        ESP_LOGI(TAG, "[UI] Unlocked");
        return true;
    }
    if ((int32_t)(now - s_lock.activity) > LOCK_TIMEOUT_MS) {
        s_lock.activity = now;   /* with the saver turned off, the lock screen simply stays */
        if (ui_overlay_saver_now(now)) {
            return false;
        }
    }
    uint32_t held = s_lock.holding ? now - s_lock.hold_start : 0u;
    int16_t pct = (int16_t)(held >= LOCK_HOLD_MS ? 100 : held * 100u / LOCK_HOLD_MS);
    if (pct != s_lock.painted_pct) {
        s_lock.painted_pct = pct;
        lg_rect_t inner = progress_inner();
        lg_draw_region(&inner, paint_progress, NULL);   /* the one thing this screen repaints */
    }
    int8_t battery = hh_service_battery_percent();
    if (battery != s_lock.battery_shown) {
        s_lock.battery_shown = battery;
        lg_draw_region(&s_lock.header_battery, header_battery_painter, NULL);
    }
    return false;
}
