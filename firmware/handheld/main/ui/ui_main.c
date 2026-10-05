/*
 * The handheld UI (D55): the UI task, the launcher, and navigation, drawn with lg_draw. Status is
 * a list screen (ui_screens.c), so it scrolls to hold the location rows (D65).
 *
 * Every piece of text is its own box, and a box is redrawn only when its text changes. Touch is
 * polled on this task, hit-tested against the tiles, and a pressed tile repaints itself.
 *
 * Every 10 s it logs how many boxes it drew, how many pixels it sent, and how long that took,
 * next to what a full-screen repaint would have cost.
 */
#include "ui_main.h"

#include <inttypes.h>
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
#include "hh_voice.h"
#include "lg_bsp_button.h"
#include "lg_bsp_touch.h"
#include "lg_draw.h"
#include "lg_emoji.h"
#include "lg_envelope.h"
#include "ui_chat.h"
#include "ui_nav.h"
#include "ui_list.h"
#include "ui_geo.h"
#include "ui_lock.h"
#include "ui_overlay.h"
#include "ui_theme.h"
#include "ui_unit.h"

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
    SCREEN_LORA_POWER,
    SCREEN_TRAIL,
    SCREEN_RENAME,
} screen_t;

enum { TILE_MESSAGES, TILE_GROUPS, TILE_STATUS, TILE_SETTINGS, TILE_COUNT };

typedef struct {
    lg_box_t panel;
    lg_box_t icon;
    lg_box_t title;
    lg_box_t detail;
} tile_t;

static struct {
    uint16_t    w;
    uint16_t    h;
    screen_t    screen;
    lg_box_t    heading;
    lg_box_t    subline;
    lg_box_t    clock;            /* the time, in a box of its own */
    tile_t      tiles[TILE_COUNT];
    int         pressed;          /* launcher tile, -1 none */
    hh_status_t st;
    /* A screen change asked for by a screen, done after its touch or refresh returns. */
    bool        nav_pending;
    ui_nav_t nav_to;
    uint8_t     nav_scope;
    uint32_t    nav_target;
    char        nav_title[40];
    bool        unit;             /* D66: an alert unit, with its own screens instead of the launcher */
} s;

const hh_status_t *ui_status(void)
{
    hh_service_status(&s.st);
    return &s.st;
}

void ui_go(ui_nav_t to, uint8_t scope, uint32_t target, const char *title)
{
    s.nav_pending = true;
    s.nav_to = to;
    s.nav_scope = scope;
    s.nav_target = target;
    snprintf(s.nav_title, sizeof(s.nav_title), "%s", title != NULL ? title : "");
}

static lg_box_t text_box(int16_t x, int16_t y, int16_t w, int16_t h, const lg_font_t *font, lg_color_t fg,
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

static void paint_heading(const lg_canvas_t *c, void *ctx)
{
    const lg_box_t *b = ctx;
    lg_paint_panel(c, &b->rect, &b->rect, C_BG, C_BG, C_BG, 0, 0);
    int16_t logo_y = (int16_t)(b->rect.y + (b->rect.h - UI_LOGO_SIZE) / 2);
    ui_paint_logo(c, PAD, logo_y);
    lg_paint_text(c, &b->rect, (int16_t)(PAD + UI_LOGO_SIZE + 6),
                  (int16_t)(b->rect.y + (b->rect.h - b->font->line_height) / 2), b->font, NULL, b->fg, b->text,
                  strlen(b->text));
    ui_bar_paint(c);   /* battery and padlock at the right of the heading row (D62) */
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
    s.heading = text_box(0, PAD - 2, (int16_t)s.w, 30, &lg_font_montserrat_20, C_ACCENT, C_BG, LG_ALIGN_LEFT, "LocalGrid");
    s.heading.pad = PAD;
    s.subline = text_box(0, PAD + 28, (int16_t)s.w, 18, &lg_font_montserrat_12, C_MUTED, C_BG, LG_ALIGN_LEFT, "Starting");
    s.subline.pad = PAD;
    s.clock = text_box(0, 0, 0, UI_CLOCK_H, F_BODY, C_TEXT, C_SURFACE, LG_ALIGN_CENTER, "");
    s.clock.outside = C_BG;
    s.clock.border = C_OUTLINE;
    s.clock.border_w = 1;
    s.clock.radius = 6;

    static const char *icons[TILE_COUNT] = { LG_SYMBOL_ENVELOPE, LG_SYMBOL_LIST, LG_SYMBOL_WIFI, LG_SYMBOL_SETTINGS };
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
        t->icon = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 - 34), inner, 32, &lg_font_montserrat_28, C_ACCENT,
                           C_SURFACE, LG_ALIGN_CENTER, icons[i]);
        t->title = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 + 2), inner, 16, &lg_font_montserrat_12, C_TEXT,
                            C_SURFACE, LG_ALIGN_CENTER, titles[i]);
        t->detail = text_box((int16_t)(x + 6), (int16_t)(y + th / 2 + 20), inner, 16, &lg_font_montserrat_12, C_MUTED,
                             C_SURFACE, LG_ALIGN_CENTER, "");
    }
}

static void show_launcher(void)
{
    lg_draw_scroll_area(0, 0);   /* whatever came before may have scrolled the panel */
    s.screen = SCREEN_LAUNCHER;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_fill(&all, C_BG);
    int16_t bar_left = ui_bar_place((int16_t)(s.w - PAD), s.heading.rect.y, s.heading.rect.h);
    /*
     * The clock sits beside the title when the screen is wide enough for both and the battery; on a
     * narrow one it goes at the right of the line below, and that line gives up the room. Measured,
     * not assumed: the widest time, the title as drawn, and wherever the bar says it begins.
     */
    int16_t clock_w = (int16_t)(lg_draw_text_width(s.clock.font, NULL, "12:00 AM") + 2 * UI_CLOCK_PAD);
    int16_t title_end = (int16_t)(PAD + UI_LOGO_SIZE + 6 + lg_draw_text_width(s.heading.font, NULL, s.heading.text));
    s.subline.rect.w = (int16_t)s.w;
    if (title_end + 2 * GAP + clock_w <= bar_left) {
        s.clock.rect = (lg_rect_t){ (int16_t)(bar_left - GAP - clock_w),
                                    (int16_t)(s.heading.rect.y + (s.heading.rect.h - UI_CLOCK_H) / 2), clock_w,
                                    UI_CLOCK_H };
    } else {
        s.clock.rect = (lg_rect_t){ (int16_t)(s.w - PAD - clock_w),
                                    (int16_t)(s.subline.rect.y + (s.subline.rect.h - UI_CLOCK_H) / 2), clock_w,
                                    UI_CLOCK_H };
        s.subline.rect.w = (int16_t)(s.clock.rect.x - GAP);
    }
    lg_draw_region(&s.heading.rect, paint_heading, &s.heading);
    lg_draw_box(&s.subline);
    lg_draw_box(&s.clock);
    for (int i = 0; i < TILE_COUNT; i++) {
        draw_tile(&s.tiles[i], false);
    }
}

/*
 * While someone is being played, a mark in the header's spare slot (left of the battery) says so on
 * every screen, so a talk from another conversation is not a voice from nowhere (D61). The chat for
 * that conversation also names them in its title. Painted every refresh while it is up, because a
 * screen's own header paint clears the slot.
 */
/*
 * The mark itself: a speaker with waves coming out of it, drawn rather than lettered.
 *
 * It used to be LG_SYMBOL_WIFI, which was wrong in the way that matters - the owner read it as a Wi-Fi
 * indicator, because that is what it is. Nothing in this font is a speaker (Montserrat here carries 24
 * symbols and none of 0xF026-0xF028), so it is drawn from rectangles: a small block for the body, three
 * stacked steps for the cone, and two arcs to its right.
 *
 * The arcs **move**. A static icon says "voice exists somewhere"; one with waves climbing out says
 * "sound is coming out of this right now", which is the thing a person needs to know before they hear
 * a stranger's voice. `wave` counts 0, 1, 2 and the arcs light up to it in turn.
 */
static uint8_t s_voice_wave;

static void voice_paint(const lg_canvas_t *c, void *ctx)
{
    const lg_rect_t *r = (const lg_rect_t *)ctx;
    lg_color_t fg = (lg_color_t)(uintptr_t)0;   /* replaced below; kept to silence a stale warning */
    hh_voice_state_t vs;
    hh_voice_state(&vs);
    fg = vs.talking ? C_ERROR : C_ACCENT;

    lg_paint_panel(c, r, r, C_BG, C_BG, C_BG, 0, 0);   /* the slot's own background first */

    /* The speaker, centred in the left half of the slot. */
    int16_t h = r->h, cx = (int16_t)(r->x + 2), cy = (int16_t)(r->y + h / 2);
    lg_rect_t body = { cx, (int16_t)(cy - 2), 3, 5 };
    lg_paint_panel(c, r, &body, fg, fg, fg, 0, 0);
    for (int16_t i = 0; i < 3; i++) {   /* the cone: three steps, taller as they go out */
        lg_rect_t step = { (int16_t)(cx + 3 + i), (int16_t)(cy - 3 - i * 2), 1, (int16_t)(7 + i * 4) };
        lg_paint_panel(c, r, &step, fg, fg, fg, 0, 0);
    }
    /* Two arcs, lit in turn, so the icon is never still while a voice is coming out. */
    for (uint8_t k = 0; k < 2u; k++) {
        if (k > s_voice_wave) {
            break;
        }
        int16_t x = (int16_t)(cx + 8 + k * 3);
        int16_t half = (int16_t)(4 + k * 3);
        lg_rect_t arc = { x, (int16_t)(cy - half), 1, (int16_t)(half * 2) };
        if (arc.x + arc.w <= r->x + r->w) {
            lg_paint_panel(c, r, &arc, fg, fg, fg, 0, 0);
        }
    }
}

static void voice_mark(void)
{
    static bool shown;
    static uint8_t last_wave = 0xFF;
    hh_voice_state_t vs;
    hh_voice_state(&vs);
    bool on = vs.heard != 0 || vs.talking;
    if (!on && !shown) {
        return;
    }
    lg_rect_t r = ui_bar_spare_rect();
    if (r.w <= 0) {
        return;
    }
    if (on) {
        s_voice_wave = (uint8_t)((s_voice_wave + 1u) % 3u);
    }
    if (on == shown && s_voice_wave == last_wave) {
        return;   /* nothing has changed: do not repaint the bar for its own sake */
    }
    shown = on;
    last_wave = s_voice_wave;
    if (!on) {
        lg_draw_fill(&r, C_BG);   /* gone: leave the slot as it was found */
        return;
    }
    static lg_rect_t slot;
    slot = r;
    lg_draw_region(&r, voice_paint, &slot);
}

static void refresh(void)
{
    if (s.unit) {
        return;   /* the unit's screens refresh themselves (ui_unit_tick) */
    }
    const hh_status_t *st = ui_status();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    (void)ui_bar_refresh();   /* the badge repaints only when the charge shown changes */
    voice_mark();
    switch (s.screen) {
    case SCREEN_CHAT:       ui_chat_refresh(st); return;
    case SCREEN_CONVS:      ui_convs_refresh(); return;
    case SCREEN_GROUPS:     ui_groups_refresh(); return;
    case SCREEN_GROUP_EDIT: ui_group_edit_refresh(now); return;
    case SCREEN_SETTINGS:   ui_settings_refresh(); return;
    case SCREEN_WHICH_AP:   ui_which_ap_refresh(); return;
    case SCREEN_LORA_POWER: ui_lora_power_refresh(); return;
    case SCREEN_TRAIL:      ui_trail_refresh(); return;
    case SCREEN_STATUS:     ui_status_refresh(); return;
    case SCREEN_RENAME:     return;
    default:                break;
    }
    char text[128];   /* lg_draw_set_text keeps what fits in a box */
    struct tm lt;
    hh_local_time(st->grid_time, &lt);   /* D67: the grid's zone */
    uint32_t day = (uint32_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    if (s.screen == SCREEN_LAUNCHER) {
        /* The time has a box of its own, with AM or PM: beside a name and an AP it read as part of them. */
        char clock[16];
        if (st->grid_time == 0) {
            snprintf(clock, sizeof(clock), "--:--");
        } else {
            unsigned hour = (unsigned)(day / 3600u);
            snprintf(clock, sizeof(clock), "%u:%02u %s", hour % 12u == 0u ? 12u : hour % 12u, (unsigned)(day / 60u % 60u),
                     hour < 12u ? "AM" : "PM");
        }
        lg_draw_set_text(&s.clock, clock);
        if (st->link == HH_LINK_ONLINE) {
            /* How long this handheld has been on that AP: what a number after "on Fuji" is taken to mean. */
            uint32_t on_s = st->on_ap_since_ms != 0 ? (now - st->on_ap_since_ms) / 1000u : 0u;
            if (on_s < 60u) {
                snprintf(text, sizeof(text), "%s on %s", st->name, st->node_ssid);
            } else {
                char how_long[16];
                ui_geo_age_text(on_s, how_long, sizeof(how_long));
                snprintf(text, sizeof(text), "%s on %s for %s", st->name, st->node_ssid, how_long);
                /* Where the clock shares this line there is less of it: say less rather than lose the
                 * end, which is the part that changes. This handheld's own name goes first. */
                int16_t room = (int16_t)(s.subline.rect.w - 2 * s.subline.pad);
                if (lg_draw_text_width(s.subline.font, s.subline.fallback, text) > room) {
                    snprintf(text, sizeof(text), "On %s for %s", st->node_ssid, how_long);
                }
                if (lg_draw_text_width(s.subline.font, s.subline.fallback, text) > room) {
                    snprintf(text, sizeof(text), "%s, %s", st->node_ssid, how_long);
                }
            }
            s.subline.fg = st->time_restricted ? C_WARNING : C_ACCENT;
        } else if (st->link == HH_LINK_LORA) {
            /*
             * Not "looking for an AP": it has one, over the other radio, and messages are crossing.
             * Named, like the Wi-Fi line above it, because which AP is carrying them is the next
             * question - and on LoRa it may be a different AP from the one Wi-Fi would have chosen.
             */
            if (st->lora_peer_ssid[0] != '\0') {
                snprintf(text, sizeof(text), "%s on LoRa via %s", st->name, st->lora_peer_ssid);
            } else {
                snprintf(text, sizeof(text), "%s on LoRa", st->name);
            }
            s.subline.fg = C_WARNING;
        } else {
            snprintf(text, sizeof(text), "%s", st->problem[0] ? st->problem : "Looking for an AP");
            s.subline.fg = st->problem[0] ? C_ERROR : C_WARNING;
        }
        lg_draw_set_text(&s.subline, text);
        uint32_t unread = ui_notify_unread_total();
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
    }
}

bool ui_chat_showing(uint8_t scope, uint32_t target)
{
    return s.screen == SCREEN_CHAT && !ui_lock_active() && ui_chat_is(scope, target);
}

/*
 * The chat screen (ui_chat.c) places the bar in its header (house, battery, padlock) but paints
 * only its title and back arrow, so the bar is drawn here whenever it has repainted the screen.
 */
static void chat_bar(void)
{
    ui_bar_draw();
}

static void chat_bar_if_repainted(uint64_t pixels_before)
{
    if (s.screen == SCREEN_CHAT && !ui_lock_active() &&
        lg_draw_stats()->pixels - pixels_before >= (uint64_t)s.w * s.h / 2u) {
        ui_bar_draw();
    }
}

void ui_redraw_current(void)
{
    if (s.unit) {
        ui_unit_redraw();
        return;
    }
    if (ui_lock_active()) {
        ui_lock_draw();   /* after an alert or the saver: back to the lock, never the screen under it */
        return;
    }
    switch (s.screen) {
    case SCREEN_LAUNCHER:   show_launcher(); refresh(); break;
    case SCREEN_CHAT:       ui_chat_redraw(); chat_bar(); break;
    case SCREEN_GROUP_EDIT: ui_group_edit_redraw(); break;
    case SCREEN_RENAME:     ui_rename_redraw(); break;
    default:                slist_redraw(); break;   /* status, conversations, groups, settings, which AP */
    }
}

static void apply_nav(void)
{
    if (!s.nav_pending || ui_lock_active()) {
        return;   /* while locked a screen change waits, and is made when the lock is released */
    }
    /*
     * A screen never draws under what covers the panel. The loop already holds the refresh back while
     * the saver or an alert is up, but a screen *change* came through here regardless - asked for by
     * the console, it painted the new screen straight over the rain, and the rain then ran on top of
     * it (owner, 2026-10-05: "we can still see the remnants of the front screen"). The saver gives way
     * to it; an alert does not, and the change waits until the alert is answered.
     */
    if (!ui_overlay_make_way((uint32_t)(esp_timer_get_time() / 1000))) {
        return;
    }
    if (s.unit) {
        s.nav_pending = false;   /* an alert unit has only its own screens */
        return;
    }
    s.nav_pending = false;
    s.pressed = -1;
    /* One name for each ui_nav_t, in its order: a screen added there is added here. */
    static const char *const NAMES[] = { "home", "status", "conversations", "chat", "groups", "group editor",
                                         "settings", "which AP", "LoRa power", "trail", "rename", "calibration" };
    _Static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == NAV_CALIBRATE + 1, "a name for every screen");
    ESP_LOGI(TAG, "[UI] Screen: %s%s%s; free %" PRIu32 " KB", NAMES[s.nav_to], s.nav_title[0] ? " " : "",
             s.nav_title, (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
    lg_draw_scroll_area(0, 0);   /* every screen starts unscrolled; list screens set their own area */
    switch (s.nav_to) {
    case NAV_HOME:
        show_launcher();
        refresh();
        break;
    case NAV_STATUS:
        s.screen = SCREEN_STATUS;
        ui_status_open(s.w, s.h);
        break;
    case NAV_CONVERSATIONS:
        s.screen = SCREEN_CONVS;
        ui_convs_open(s.w, s.h);
        break;
    case NAV_CHAT:
        s.screen = SCREEN_CHAT;
        ui_chat_open(s.w, s.h, s.nav_scope, s.nav_target, s.nav_title);
        chat_bar();
        break;
    case NAV_GROUPS:
        s.screen = SCREEN_GROUPS;
        ui_groups_open(s.w, s.h);
        break;
    case NAV_GROUP_EDIT:
        s.screen = SCREEN_GROUP_EDIT;
        ui_group_edit_open(s.w, s.h, (uint16_t)s.nav_target);
        break;
    case NAV_SETTINGS:
        s.screen = SCREEN_SETTINGS;
        ui_settings_open(s.w, s.h, (uint8_t)s.nav_target);
        break;
    case NAV_WHICH_AP:
        s.screen = SCREEN_WHICH_AP;
        ui_which_ap_open(s.w, s.h);
        break;
    case NAV_LORA_POWER:
        s.screen = SCREEN_LORA_POWER;
        ui_lora_power_open(s.w, s.h);
        break;
    case NAV_TRAIL:
        s.screen = SCREEN_TRAIL;
        ui_trail_open(s.w, s.h, s.nav_target);
        break;
    case NAV_RENAME:
        s.screen = SCREEN_RENAME;
        ui_rename_open(s.w, s.h);
        break;
    case NAV_CALIBRATE:
        ui_calibrate_run(s.w, s.h);   /* asks for Settings when it is done */
        apply_nav();
        break;
    }
}

static void on_tap(int16_t x, int16_t y, bool down)
{
    if (s.unit) {
        ui_unit_touch(x, y, down, (uint32_t)(esp_timer_get_time() / 1000));
        return;
    }
    if (ui_lock_active() || ui_bar_touch(x, y, down)) {
        return;   /* the lock takes every touch; a press on the padlock is the bar's */
    }
    switch (s.screen) {
    case SCREEN_CONVS:      ui_convs_touch(x, y, down); return;
    case SCREEN_GROUPS:     ui_groups_touch(x, y, down); return;
    case SCREEN_GROUP_EDIT: ui_group_edit_touch(x, y, down); return;
    case SCREEN_SETTINGS:   ui_settings_touch(x, y, down); return;
    case SCREEN_WHICH_AP:   ui_which_ap_touch(x, y, down); return;
    case SCREEN_LORA_POWER: ui_lora_power_touch(x, y, down); return;
    case SCREEN_TRAIL:      ui_trail_touch(x, y, down); return;
    case SCREEN_STATUS:     ui_status_touch(x, y, down); return;
    case SCREEN_RENAME:     ui_rename_touch(x, y, down); return;
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
                static const ui_nav_t TILE_NAV[TILE_COUNT] = { NAV_CONVERSATIONS, NAV_GROUPS, NAV_STATUS,
                                                                  NAV_SETTINGS };
                ui_go(TILE_NAV[hit], 0, 0, NULL);
            } else {
                draw_tile(&s.tiles[was], false);
            }
        }
    } else if (s.screen == SCREEN_CHAT) {
        if (ui_chat_touch(x, y, down)) {
            ui_go(NAV_CONVERSATIONS, 0, 0, NULL);
        }
    }
}

static QueueHandle_t s_requests;
static TaskHandle_t  s_task;
static uint32_t s_last_touch;

static void handle_request(const ui_req_t *r)
{
    switch (r->cmd) {
    case UI_HOME:
        ui_go(NAV_HOME, 0, 0, NULL);   /* through navigation, so it waits while locked */
        break;
    case UI_STATUS:
        ui_go(NAV_STATUS, 0, 0, NULL);
        break;
    case UI_CHAT:
        ui_go(NAV_CHAT, r->scope, r->target, r->text);
        break;
    case UI_GO:
        ui_go((ui_nav_t)r->arg, 0, r->target, NULL);
        break;
    case UI_TAP: {
        int16_t x = (int16_t)(r->arg >> 16);
        int16_t y = (int16_t)(r->arg & 0xFFFF);
        /* A tap as the finger makes it: the overlay sees it first, then the screen, and it counts
         * as activity for the screen saver. */
        s_last_touch = (uint32_t)(esp_timer_get_time() / 1000);
        uint64_t before = lg_draw_stats()->pixels;
        if (!ui_overlay_touch(x, y, true)) {
            on_tap(x, y, true);
        }
        apply_nav();
        if (!ui_overlay_touch(x, y, false)) {
            on_tap(x, y, false);
        }
        chat_bar_if_repainted(before);
        break;
    }
    case UI_SCROLL:
        if (s.screen == SCREEN_CHAT && !ui_lock_active()) {
            ui_chat_scroll_by((int16_t)r->arg);
        }
        break;
    case UI_KEYBOARD:
        if (s.screen == SCREEN_CHAT && !ui_lock_active()) {
            ui_chat_keyboard(r->arg != 0);
            chat_bar();
        }
        break;
    case UI_TYPE:
        if (s.screen == SCREEN_CHAT && !ui_lock_active()) {
            uint64_t before = lg_draw_stats()->pixels;
            ui_chat_type(r->text);
            chat_bar_if_repainted(before);
        }
        break;
    case UI_BUTTON:
        if (s.unit) {
            /* arg: button index << 1, plus 1 for a hold (the console's `ui button`) */
            ui_unit_button((uint8_t)(r->arg >> 1), (r->arg & 1) != 0, (uint32_t)(esp_timer_get_time() / 1000));
        }
        break;
    case UI_LOG:
        if (s.unit) {
            ui_unit_log();
        }
        if (s.screen == SCREEN_LAUNCHER) {
            /* the home screen's two lines, with where each sits, so a layout can be checked over serial */
            printf("HOME %ux%u title ends %d, bar from %d; clock [%s] at %d,%d %dx%d; line [%s] at %d,%d %dx%d needs %d\n",
                   s.w, s.h,
                   (int)(PAD + UI_LOGO_SIZE + 6 + lg_draw_text_width(s.heading.font, NULL, s.heading.text)),
                   (int)ui_bar_spare_rect().x, s.clock.text, s.clock.rect.x, s.clock.rect.y, s.clock.rect.w,
                   s.clock.rect.h, s.subline.text, s.subline.rect.x, s.subline.rect.y, s.subline.rect.w,
                   s.subline.rect.h,
                   (int)(lg_draw_text_width(s.subline.font, s.subline.fallback, s.subline.text) + 2 * s.subline.pad));
        }
        ui_chat_log();
        slist_log();
        ui_overlay_log((uint32_t)(esp_timer_get_time() / 1000), s_last_touch);
        break;
    case UI_PAGE:
        if (s.screen == SCREEN_CHAT && !ui_lock_active()) {
            ui_chat_page(r->arg);
        }
        break;
    }
}

static void ui_task(void *arg)
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
        bool taken = false;   /* the overlay had this sample: the lock must not see it */
        if (down || was_down) {
            uint64_t before = lg_draw_stats()->pixels;
            taken = ui_overlay_touch(x, y, down);
            if (!taken) {
                on_tap(x, y, down);
                chat_bar_if_repainted(before);
                if (lg_draw_stats()->pixels != before) {
                    ui_overlay_screen_painted();
                }
            }
            apply_nav();
        }
        was_down = down;
        /* The board's buttons (D66): the screens decide what a press or a hold does. */
        lg_btn_event_t ev;
        while (lg_bsp_button_poll(now, &ev)) {
            s_last_touch = now;
            if (!s.unit) {
                continue;   /* no full-handheld screen uses a button yet */
            }
            if (ev.kind == LG_BTN_DOWN || ev.kind == LG_BTN_UP || ev.kind == LG_BTN_SHORT) {
                ui_unit_button_down(ev.id, ev.kind == LG_BTN_DOWN, now);
            }
            if (ev.kind == LG_BTN_SHORT || ev.kind == LG_BTN_HOLD) {
                ui_unit_button(ev.id, ev.kind == LG_BTN_HOLD, now);
            }
            apply_nav();
        }
        ui_overlay_tick(now, s_last_touch);
        if (s.unit) {
            ui_unit_tick(now, ui_overlay_covering());   /* the SOS runs even while an alert covers it */
        }
        ui_req_t req;
        while (xQueueReceive(s_requests, &req, 0) == pdTRUE) {
            handle_request(&req);
            apply_nav();
        }
        if (ui_overlay_covering()) {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            continue;   /* an alert or the saver owns the panel: screens do not paint */
        }
        if (ui_lock_active()) {
            if (ui_lock_tick(now, down && !taken, x, y)) {
                s_last_touch = now;
                if (s.nav_pending) {
                    apply_nav();   /* a screen asked for while locked */
                } else {
                    ui_redraw_current();
                }
            }
        } else {
            if (s.screen == SCREEN_CHAT) {
                ui_chat_tick(now);
            }
            if (now - last_refresh >= REFRESH_MS) {
                last_refresh = now;
                uint64_t before = lg_draw_stats()->pixels;
                refresh();
                if (lg_draw_stats()->pixels != before) {
                    ui_overlay_screen_painted();
                }
                apply_nav();
            }
        }
        if (now - last_stats >= STATS_MS) {
            last_stats = now;
            const lg_draw_stats_t *d = lg_draw_stats();
            uint64_t px = d->pixels - prev.pixels;
            ESP_LOGI(TAG, "[UI] Last 10 s: %" PRIu32 " boxes, %" PRIu64 " px sent (%.1f full screens), %" PRIu64
                     " ms drawing; free %" PRIu32 " KB, lowest %" PRIu32 " KB",
                     d->boxes - prev.boxes, px, (double)px / ((double)s.w * s.h),
                     (d->draw_us - prev.draw_us) / 1000u, esp_get_free_heap_size() / 1024u,
                     esp_get_minimum_free_heap_size() / 1024u);
            prev = *d;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t ui_start(const lg_board_t *board)
{
    esp_err_t err = lg_draw_start(board, C_BG, &s.w, &s.h);
    if (err != ESP_OK) {
        return err;
    }
    hh_mem_mark("after lg_draw, panel, touch");
    s.pressed = -1;
    s_requests = xQueueCreate(8, sizeof(ui_req_t));
    if (lg_board_is_alert_unit(board)) {
        /* D66: alerts, SOS, and status only. No launcher, lock, chats, or keyboard are built. */
        s.unit = true;
        bool touch = lg_bsp_touch_present();
        bool button = (lg_board_button_actions(board) & LG_ACT_READ) != 0;
        ui_overlay_start(s.w, s.h);
        ui_overlay_set_alert_unit(button ? (touch ? "or press the button" : "Press button: Read") : NULL);
        ui_unit_start(board, s.w, s.h, touch);
        hh_mem_mark("after alert unit screens");
        if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, &s_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "[UI] Launcher ready: %ux%u (alert unit: %s%s%s)", s.w, s.h, touch ? "touch" : "no touch",
                 button ? ", " : "", button ? "button" : "");
        return ESP_OK;
    }
    /* A resistive panel maps no touches until it is calibrated, so nothing else can be pressed. */
    while (lg_bsp_touch_needs_calibration()) {
        ui_calibrate_run(s.w, s.h);
    }
    s.nav_pending = false;   /* calibration asks for Settings; at boot the launcher comes first */
    build_launcher();
    ui_overlay_start(s.w, s.h);
    ui_lock_start(s.w, s.h);
    show_launcher();
    hh_mem_mark("after launcher");
    if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[UI] Launcher ready: %ux%u", s.w, s.h);
    return ESP_OK;
}

/* Console hooks: requests cross tasks through a queue, and the UI task does the drawing, so
 * lg_draw is used by one task alone. */

void ui_request_cmd(ui_cmd_t cmd, int arg, const char *text)
{
    if (s_requests == NULL) {
        return;
    }
    ui_req_t req = { .cmd = cmd, .arg = arg };
    if (text != NULL) {
        snprintf(req.text, sizeof(req.text), "%s", text);
    }
    (void)xQueueSend(s_requests, &req, 0);
}

void ui_request(bool status)
{
    ui_request_cmd(status ? UI_STATUS : UI_HOME, 0, NULL);
}

void ui_open_chat(uint8_t scope, uint32_t target, const char *title)
{
    if (s_requests == NULL) {
        return;
    }
    ui_req_t req = { .cmd = UI_CHAT, .scope = scope, .target = target };
    snprintf(req.text, sizeof(req.text), "%s", title != NULL ? title : "");
    (void)xQueueSend(s_requests, &req, 0);
}

void ui_request_trail(uint32_t device)
{
    if (s_requests == NULL) {
        return;
    }
    ui_req_t req = { .cmd = UI_GO, .arg = (int)NAV_TRAIL, .target = device };
    (void)xQueueSend(s_requests, &req, 0);
}

uint32_t ui_stack_headroom(void)
{
    return s_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0u;
}
