/*
 * The no-LVGL spike (owner, 2026-09-17): the launcher and the Status screen, drawn with lg_draw.
 *
 * Every piece of text is its own box, and a box is redrawn only when its text changes, so the
 * Status screen's once-a-second clock repaints about 100 by 22 pixels, not the panel. Touch is
 * polled on this task, hit-tested against the tiles, and a pressed tile repaints itself.
 *
 * Every 10 s it logs how many boxes it drew, how many pixels it sent, and how long that took,
 * next to what a full-screen repaint would have cost, for the comparison with LVGL.
 */
#include "spike_ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hh_mem.h"
#include "hh_service.h"
#include "lg_draw.h"
#include "lg_emoji.h"
#include "lg_envelope.h"
#include "spike_chat.h"
#include "spike_nav.h"
#include "spike_list.h"
#include "spike_overlay.h"
#include "spike_theme.h"

static const char *TAG = "UI";


#define TICK_MS      20
#define REFRESH_MS   250
#define STATS_MS     10000
#define PAD          8
#define GAP          5

typedef enum {
    SCREEN_LAUNCHER,
    SCREEN_STATUS,
    SCREEN_CHAT,
    SCREEN_CONVS,
    SCREEN_GROUPS,
    SCREEN_GROUP_EDIT,
    SCREEN_SETTINGS,
    SCREEN_WHICH_AP,
    SCREEN_RENAME,
} screen_t;

enum { TILE_MESSAGES, TILE_GROUPS, TILE_STATUS, TILE_SETTINGS, TILE_COUNT };

typedef struct {
    lg_box_t panel;
    lg_box_t icon;
    lg_box_t title;
    lg_box_t detail;
} tile_t;

#define STATUS_ROWS 11

static struct {
    uint16_t    w;
    uint16_t    h;
    screen_t    screen;
    lg_box_t    heading;
    lg_box_t    subline;
    tile_t      tiles[TILE_COUNT];
    lg_box_t    back;
    lg_box_t    labels[STATUS_ROWS];
    lg_box_t    values[STATUS_ROWS];
    int         pressed;          /* tile or -2 for back, -1 none */
    hh_status_t st;
    bool        marked_status;
    /* A screen change asked for by a screen, done after its touch or refresh returns. */
    bool        nav_pending;
    spike_nav_t nav_to;
    uint8_t     nav_scope;
    uint32_t    nav_target;
    char        nav_title[40];
} s;

const hh_status_t *spike_status(void)
{
    hh_service_status(&s.st);
    return &s.st;
}

void spike_go(spike_nav_t to, uint8_t scope, uint32_t target, const char *title)
{
    s.nav_pending = true;
    s.nav_to = to;
    s.nav_scope = scope;
    s.nav_target = target;
    snprintf(s.nav_title, sizeof(s.nav_title), "%s", title != NULL ? title : "");
}

static lg_box_t text_box(int16_t x, int16_t y, int16_t w, int16_t h, const lv_font_t *font, lg_color_t fg,
                         lg_color_t bg, uint8_t align, const char *text)
{
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ x, y, w, h };
    b.bg = bg;
    b.outside = bg;
    b.font = font;
    b.fallback = &lg_font_emoji_14;
    b.fg = fg;
    b.align = align;
    b.pad = 0;
    snprintf(b.text, sizeof(b.text), "%s", text);
    return b;
}

/*
 * The LocalGrid mark (assets/brand/localgrid-icon.svg, as on the admin page), drawn pixel by
 * pixel from its shapes in the SVG's 64-unit grid: a rounded tile, two radio waves over the top
 * node, the tent of links between three nodes, and a handheld in the middle joined to each. Edges
 * are smoothed from each pixel's distance to the shape, so it reads at 26 px.
 */
#define LOGO_SIZE 26

static float clampf(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax;
    float vy = by - ay;
    float t = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy));
    float dx = px - (ax + t * vx);
    float dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static void paint_logo(const lg_canvas_t *c, int16_t x0, int16_t y0)
{
    lg_rect_t box = { x0, y0, LOGO_SIZE, LOGO_SIZE };
    lg_paint_panel(c, &box, &box, C_BAR, C_BG, C_BAR, 0, (uint8_t)(14 * LOGO_SIZE / 64));
    float unit = 64.0f / LOGO_SIZE;   /* grid units per pixel */
    for (int16_t py = 0; py < LOGO_SIZE; py++) {
        int16_t sy = (int16_t)(y0 + py);
        if (sy < c->band.y || sy >= c->band.y + c->band.h) {
            continue;
        }
        for (int16_t px = 0; px < LOGO_SIZE; px++) {
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

static void paint_heading(const lg_canvas_t *c, void *ctx)
{
    const lg_box_t *b = ctx;
    lg_paint_panel(c, &b->rect, &b->rect, C_BG, C_BG, C_BG, 0, 0);
    int16_t logo_y = (int16_t)(b->rect.y + (b->rect.h - LOGO_SIZE) / 2);
    paint_logo(c, PAD, logo_y);
    lg_paint_text(c, &b->rect, (int16_t)(PAD + LOGO_SIZE + 6),
                  (int16_t)(b->rect.y + (b->rect.h - b->font->line_height) / 2), b->font, NULL, b->fg, b->text,
                  strlen(b->text));
}

static void draw_tile(tile_t *t, bool pressed)
{
    lg_color_t bg = pressed ? C_OUTLINE : C_SURFACE;
    t->panel.bg = bg;
    t->icon.bg = t->title.bg = t->detail.bg = bg;
    lg_draw_box(&t->panel);
    lg_draw_box(&t->icon);
    lg_draw_box(&t->title);
    lg_draw_box(&t->detail);
}

static void build_launcher(void)
{
    s.heading = text_box(0, PAD - 2, (int16_t)s.w, 30, &lv_font_montserrat_20, C_ACCENT, C_BG, LG_ALIGN_LEFT, "LocalGrid");
    s.heading.pad = PAD;
    s.subline = text_box(0, PAD + 28, (int16_t)s.w, 18, &lv_font_montserrat_12, C_MUTED, C_BG, LG_ALIGN_LEFT, "Starting");
    s.subline.pad = PAD;

    static const char *icons[TILE_COUNT] = { LV_SYMBOL_ENVELOPE, LV_SYMBOL_LIST, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS };
    static const char *titles[TILE_COUNT] = { "Messages", "Groups", "Status", "Settings" };
    int16_t top = (int16_t)(PAD + 28 + 18 + GAP);
    int16_t tw = (int16_t)((s.w - 2 * PAD - GAP) / 2);
    int16_t th = (int16_t)((s.h - top - PAD - GAP) / 2);
    for (int i = 0; i < TILE_COUNT; i++) {
        tile_t *t = &s.tiles[i];
        int16_t x = (int16_t)(PAD + (i % 2) * (tw + GAP));
        int16_t y = (int16_t)(top + (i / 2) * (th + GAP));
        t->panel = text_box(x, y, tw, th, NULL, C_TEXT, C_SURFACE, LG_ALIGN_CENTER, "");
        t->panel.outside = C_BG;
        t->panel.border = C_ACCENT;
        t->panel.border_w = 1;
        t->panel.radius = 6;
        int16_t inner = (int16_t)(tw - 12);
        t->icon = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 - 34), inner, 32, &lv_font_montserrat_28, C_ACCENT,
                           C_SURFACE, LG_ALIGN_CENTER, icons[i]);
        t->title = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 + 2), inner, 16, &lv_font_montserrat_12, C_TEXT,
                            C_SURFACE, LG_ALIGN_CENTER, titles[i]);
        t->detail = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 + 20), inner, 16, &lv_font_montserrat_12, C_MUTED,
                             C_SURFACE, LG_ALIGN_CENTER, "");
    }
}

static void show_launcher(void)
{
    lg_draw_scroll_area(0, 0);   /* whatever came before may have scrolled the panel */
    s.screen = SCREEN_LAUNCHER;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_fill(&all, C_BG);
    lg_draw_region(&s.heading.rect, paint_heading, &s.heading);
    lg_draw_box(&s.subline);
    for (int i = 0; i < TILE_COUNT; i++) {
        draw_tile(&s.tiles[i], false);
    }
}

static const char *ROW_NAMES[STATUS_ROWS] = {
    "Name", "Link", "AP", "Signal", "Address", "Grid time", "People", "Groups", "Free memory", "Lowest", "Boxes drawn",
};

static void build_status(void)
{
    s.back = text_box((int16_t)(s.w - 44), PAD, 36, 28, &lv_font_montserrat_20, C_ACCENT, C_BG, LG_ALIGN_CENTER,
                      LV_SYMBOL_LEFT);
    for (int i = 0; i < STATUS_ROWS; i++) {
        int16_t y = (int16_t)(44 + i * 24);
        s.labels[i] = text_box(PAD, y, 100, 22, &lv_font_montserrat_12, C_MUTED, C_BG, LG_ALIGN_LEFT, ROW_NAMES[i]);
        s.values[i] = text_box(108, y, (int16_t)(s.w - 108 - PAD), 22, &lv_font_montserrat_14, C_TEXT, C_BG,
                               LG_ALIGN_RIGHT, "");
    }
}

static void show_status(void)
{
    lg_draw_scroll_area(0, 0);
    s.screen = SCREEN_STATUS;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_fill(&all, C_BG);
    lg_box_t title = text_box(0, PAD, 140, 28, &lv_font_montserrat_20, C_ACCENT, C_BG, LG_ALIGN_LEFT, "Status");
    title.pad = PAD;
    lg_draw_box(&title);
    lg_draw_box(&s.back);
    for (int i = 0; i < STATUS_ROWS; i++) {
        s.values[i].text[0] = '\0';   /* force every value to draw once */
        lg_draw_box(&s.labels[i]);
    }
}

static const char *link_word(hh_link_t link)
{
    switch (link) {
    case HH_LINK_SEARCHING:   return "searching";
    case HH_LINK_CONNECTING:  return "connecting";
    case HH_LINK_REGISTERING: return "registering";
    case HH_LINK_ONLINE:      return "online";
    default:                  return "stopped";
    }
}

static void refresh(void)
{
    const hh_status_t *st = spike_status();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    switch (s.screen) {
    case SCREEN_CHAT:       spike_chat_refresh(st); return;
    case SCREEN_CONVS:      spike_convs_refresh(); return;
    case SCREEN_GROUPS:     spike_groups_refresh(); return;
    case SCREEN_GROUP_EDIT: spike_group_edit_refresh(now); return;
    case SCREEN_SETTINGS:   spike_settings_refresh(); return;
    case SCREEN_WHICH_AP:   spike_which_ap_refresh(); return;
    case SCREEN_RENAME:     return;
    default:                break;
    }
    char text[128];   /* lg_draw_set_text keeps what fits in a box */
    uint32_t day = st->grid_time % 86400u;
    if (s.screen == SCREEN_LAUNCHER) {
        if (st->link == HH_LINK_ONLINE) {
            snprintf(text, sizeof(text), "%s on %s, %02u:%02u", st->name, st->node_ssid, (unsigned)(day / 3600u),
                     (unsigned)(day / 60u % 60u));
            s.subline.fg = st->time_restricted ? C_WARNING : C_ACCENT;
        } else {
            snprintf(text, sizeof(text), "%s", st->problem[0] ? st->problem : "Looking for an AP");
            s.subline.fg = st->problem[0] ? C_ERROR : C_WARNING;
        }
        lg_draw_set_text(&s.subline, text);
        uint32_t unread = spike_notify_unread_total();
        if (unread > 0) {
            snprintf(text, sizeof(text), "%" PRIu32 " new", unread);
            s.tiles[TILE_MESSAGES].detail.fg = C_ACCENT;
        } else {
            snprintf(text, sizeof(text), "%u known", st->n_people);
            s.tiles[TILE_MESSAGES].detail.fg = C_MUTED;
        }
        lg_draw_set_text(&s.tiles[TILE_MESSAGES].detail, text);
        snprintf(text, sizeof(text), "%u groups", st->n_groups);
        lg_draw_set_text(&s.tiles[TILE_GROUPS].detail, text);
        snprintf(text, sizeof(text), "%u APs, %" PRIu32 " KB", st->n_nodes, esp_get_free_heap_size() / 1024u);
        lg_draw_set_text(&s.tiles[TILE_STATUS].detail, text);
        lg_draw_set_text(&s.tiles[TILE_SETTINGS].detail, st->preferred_node < 0 ? "auto AP" : "fixed AP");
        return;
    }
    const char *v[STATUS_ROWS];
    char b[STATUS_ROWS][LG_BOX_TEXT_MAX];
    snprintf(b[0], sizeof(b[0]), "%s", st->name);
    snprintf(b[1], sizeof(b[1]), "%s", link_word(st->link));
    snprintf(b[2], sizeof(b[2]), "%s", st->node >= 0 ? st->node_ssid : "--");
    snprintf(b[3], sizeof(b[3]), "%d dBm", st->rssi);
    snprintf(b[4], sizeof(b[4]), "%u.%u.%u.%u", st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
    if (st->grid_time == 0) {
        snprintf(b[5], sizeof(b[5]), "not set");
    } else {
        snprintf(b[5], sizeof(b[5]), "%02u:%02u:%02u", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u),
                 (unsigned)(day % 60u));
    }
    snprintf(b[6], sizeof(b[6]), "%u", st->n_people);
    snprintf(b[7], sizeof(b[7]), "%u", st->n_groups);
    snprintf(b[8], sizeof(b[8]), "%" PRIu32 " KB", esp_get_free_heap_size() / 1024u);
    snprintf(b[9], sizeof(b[9]), "%" PRIu32 " KB", esp_get_minimum_free_heap_size() / 1024u);
    snprintf(b[10], sizeof(b[10]), "%" PRIu32, lg_draw_stats()->boxes);
    for (int i = 0; i < STATUS_ROWS; i++) {
        v[i] = b[i];
        lg_draw_set_text(&s.values[i], v[i]);
    }
    if (!s.marked_status) {
        s.marked_status = true;
        hh_mem_mark("spike Status screen drawn");
    }
}

bool spike_chat_showing(uint8_t scope, uint32_t target)
{
    return s.screen == SCREEN_CHAT && spike_chat_is(scope, target);
}

void spike_redraw_current(void)
{
    switch (s.screen) {
    case SCREEN_LAUNCHER:   show_launcher(); refresh(); break;
    case SCREEN_STATUS:     show_status(); refresh(); break;
    case SCREEN_CHAT:       spike_chat_redraw(); break;
    case SCREEN_GROUP_EDIT: spike_group_edit_redraw(); break;
    case SCREEN_RENAME:     spike_rename_redraw(); break;
    default:                slist_redraw(); break;   /* conversations, groups, settings, which AP */
    }
}

static void apply_nav(void)
{
    if (!s.nav_pending) {
        return;
    }
    s.nav_pending = false;
    s.pressed = -1;
    static const char *const NAMES[] = { "home", "status", "conversations", "chat", "groups", "group editor",
                                         "settings", "which AP", "rename", "calibration" };
    ESP_LOGI(TAG, "[UI] Spike screen: %s%s%s; free %" PRIu32 " KB", NAMES[s.nav_to], s.nav_title[0] ? " " : "",
             s.nav_title, (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
    lg_draw_scroll_area(0, 0);   /* every screen starts unscrolled; list screens set their own area */
    switch (s.nav_to) {
    case NAV_HOME:
        show_launcher();
        refresh();
        break;
    case NAV_STATUS:
        show_status();
        refresh();
        break;
    case NAV_CONVERSATIONS:
        s.screen = SCREEN_CONVS;
        spike_convs_open(s.w, s.h);
        break;
    case NAV_CHAT:
        s.screen = SCREEN_CHAT;
        spike_chat_open(s.w, s.h, s.nav_scope, s.nav_target, s.nav_title);
        break;
    case NAV_GROUPS:
        s.screen = SCREEN_GROUPS;
        spike_groups_open(s.w, s.h);
        break;
    case NAV_GROUP_EDIT:
        s.screen = SCREEN_GROUP_EDIT;
        spike_group_edit_open(s.w, s.h, (uint16_t)s.nav_target);
        break;
    case NAV_SETTINGS:
        s.screen = SCREEN_SETTINGS;
        spike_settings_open(s.w, s.h, (uint8_t)s.nav_target);
        break;
    case NAV_WHICH_AP:
        s.screen = SCREEN_WHICH_AP;
        spike_which_ap_open(s.w, s.h);
        break;
    case NAV_RENAME:
        s.screen = SCREEN_RENAME;
        spike_rename_open(s.w, s.h);
        break;
    case NAV_CALIBRATE:
        spike_calibrate_run(s.w, s.h);   /* asks for Settings when it is done */
        apply_nav();
        break;
    }
}

static void on_tap(int16_t x, int16_t y, bool down)
{
    switch (s.screen) {
    case SCREEN_CONVS:      spike_convs_touch(x, y, down); return;
    case SCREEN_GROUPS:     spike_groups_touch(x, y, down); return;
    case SCREEN_GROUP_EDIT: spike_group_edit_touch(x, y, down); return;
    case SCREEN_SETTINGS:   spike_settings_touch(x, y, down); return;
    case SCREEN_WHICH_AP:   spike_which_ap_touch(x, y, down); return;
    case SCREEN_RENAME:     spike_rename_touch(x, y, down); return;
    default:                break;
    }
    if (s.screen == SCREEN_LAUNCHER) {
        int hit = -1;
        for (int i = 0; i < TILE_COUNT; i++) {
            if (lg_rect_hit(&s.tiles[i].panel.rect, x, y)) {
                hit = i;
            }
        }
        if (down && s.pressed < 0 && hit >= 0) {
            s.pressed = hit;
            draw_tile(&s.tiles[hit], true);   /* the pressed look, and nothing else repainted */
        } else if (!down && s.pressed >= 0) {
            int was = s.pressed;
            s.pressed = -1;
            if (was == hit) {
                static const spike_nav_t TILE_NAV[TILE_COUNT] = { NAV_CONVERSATIONS, NAV_GROUPS, NAV_STATUS,
                                                                  NAV_SETTINGS };
                spike_go(TILE_NAV[hit], 0, 0, NULL);
            } else {
                draw_tile(&s.tiles[was], false);
            }
        }
    } else if (s.screen == SCREEN_CHAT) {
        if (spike_chat_touch(x, y, down)) {
            spike_go(NAV_CONVERSATIONS, 0, 0, NULL);
        }
    } else {
        bool on_back = lg_rect_hit(&s.back.rect, x, y);
        if (down && s.pressed < 0 && on_back) {
            s.pressed = -2;
        } else if (!down && s.pressed == -2) {
            s.pressed = -1;
            if (on_back) {
                spike_go(NAV_HOME, 0, 0, NULL);
            }
        }
    }
}

static QueueHandle_t s_requests;
static uint32_t s_last_touch;

static void handle_request(const spike_req_t *r)
{
    switch (r->cmd) {
    case SPIKE_HOME:
        show_launcher();
        refresh();
        break;
    case SPIKE_STATUS:
        show_status();
        refresh();
        break;
    case SPIKE_CHAT: {
        const hh_status_t *st = spike_status();
        if (st->n_people > 0) {
            spike_go(NAV_CHAT, LG_SCOPE_DIRECT, st->people[0].device, st->people[0].name);
        } else {
            spike_go(NAV_CHAT, LG_SCOPE_BROADCAST, LG_TARGET_ALL, "Everyone");
        }
        break;
    }
    case SPIKE_GO:
        spike_go((spike_nav_t)r->arg, 0, 0, NULL);
        break;
    case SPIKE_TAP: {
        int16_t x = (int16_t)(r->arg >> 16);
        int16_t y = (int16_t)(r->arg & 0xFFFF);
        /* A tap as the finger makes it: the overlay sees it first, then the screen, and it counts
         * as activity for the screen saver. */
        s_last_touch = (uint32_t)(esp_timer_get_time() / 1000);
        if (!spike_overlay_touch(x, y, true)) {
            on_tap(x, y, true);
        }
        apply_nav();
        if (!spike_overlay_touch(x, y, false)) {
            on_tap(x, y, false);
        }
        break;
    }
    case SPIKE_SCROLL:
        if (s.screen == SCREEN_CHAT) {
            spike_chat_scroll_by((int16_t)r->arg);
        }
        break;
    case SPIKE_KEYBOARD:
        if (s.screen == SCREEN_CHAT) {
            spike_chat_keyboard(r->arg != 0);
        }
        break;
    case SPIKE_TYPE:
        if (s.screen == SCREEN_CHAT) {
            spike_chat_type(r->text);
        }
        break;
    case SPIKE_LOG:
        spike_chat_log();
        spike_overlay_log((uint32_t)(esp_timer_get_time() / 1000), s_last_touch);
        break;
    case SPIKE_PAGE:
        if (s.screen == SCREEN_CHAT) {
            spike_chat_page(r->arg);
        }
        break;
    }
}

static void spike_task(void *arg)
{
    (void)arg;
    uint32_t last_refresh = 0;
    uint32_t last_stats = 0;
    lg_draw_stats_t prev = *lg_draw_stats();
    bool was_down = false;
    s_last_touch = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int16_t x = 0;
        int16_t y = 0;
        bool down = lg_draw_touch(&x, &y);
        if (down) {
            s_last_touch = now;
        }
        if (down || was_down) {
            uint64_t before = lg_draw_stats()->pixels;
            if (!spike_overlay_touch(x, y, down)) {
                on_tap(x, y, down);
                if (lg_draw_stats()->pixels != before) {
                    spike_overlay_screen_painted();
                }
            }
            apply_nav();
        }
        was_down = down;
        spike_overlay_tick(now, s_last_touch);
        spike_req_t req;
        while (xQueueReceive(s_requests, &req, 0) == pdTRUE) {
            handle_request(&req);
            apply_nav();
        }
        if (spike_overlay_covering()) {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            continue;   /* an alert or the saver owns the panel: screens do not paint */
        }
        if (s.screen == SCREEN_CHAT) {
            spike_chat_tick(now);
        }
        if (now - last_refresh >= REFRESH_MS) {
            last_refresh = now;
            uint64_t before = lg_draw_stats()->pixels;
            refresh();
            if (lg_draw_stats()->pixels != before) {
                spike_overlay_screen_painted();
            }
            apply_nav();
        }
        if (now - last_stats >= STATS_MS) {
            last_stats = now;
            const lg_draw_stats_t *d = lg_draw_stats();
            uint64_t px = d->pixels - prev.pixels;
            ESP_LOGI(TAG, "[UI] Spike, last 10 s: %" PRIu32 " boxes, %" PRIu64 " px sent (%.1f full screens), %" PRIu64
                     " ms drawing; free %" PRIu32 " KB, lowest %" PRIu32 " KB",
                     d->boxes - prev.boxes, px, (double)px / ((double)s.w * s.h),
                     (d->draw_us - prev.draw_us) / 1000u, esp_get_free_heap_size() / 1024u,
                     esp_get_minimum_free_heap_size() / 1024u);
            prev = *d;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t spike_ui_start(const lg_board_t *board)
{
    esp_err_t err = lg_draw_start(board, C_BG, &s.w, &s.h);
    if (err != ESP_OK) {
        return err;
    }
    hh_mem_mark("after lg_draw, panel, touch (no LVGL)");
    s.pressed = -1;
    s_requests = xQueueCreate(8, sizeof(spike_req_t));
    build_launcher();
    build_status();
    spike_overlay_start(s.w, s.h);
    show_launcher();
    hh_mem_mark("after spike launcher");
    if (xTaskCreate(spike_task, "spike_ui", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[UI] Spike launcher ready: %ux%u, no LVGL", s.w, s.h);
    return ESP_OK;
}

/* Console hooks: requests cross tasks through a queue, and the spike task does the drawing, so
 * lg_draw is used by one task alone. */

void spike_ui_request_cmd(spike_cmd_t cmd, int arg, const char *text)
{
    if (s_requests == NULL) {
        return;
    }
    spike_req_t req = { .cmd = cmd, .arg = arg };
    if (text != NULL) {
        snprintf(req.text, sizeof(req.text), "%s", text);
    }
    (void)xQueueSend(s_requests, &req, 0);
}

void spike_ui_request(bool status)
{
    spike_ui_request_cmd(status ? SPIKE_STATUS : SPIKE_HOME, 0, NULL);
}
