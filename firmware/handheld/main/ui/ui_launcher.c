/*
 * The launcher.
 *
 * Tiles are measured by dividing the panel that is actually there, the way Braino's
 * launcher does: the header's height comes off first, then the gaps. Messages takes a full
 * width row because it is what the handheld is for; Status and Settings share the row under
 * it. Each tile is an icon over a small label, and the label never wraps: at title size
 * "Self test" broke onto two lines and left its tile.
 */
#include "ui_snapshot.h"
#include "ui_launcher.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_display.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "hh_mem.h"
#include "ui_alert.h"
#include "ui_chat.h"
#include "ui_home.h"
#include "ui_notify.h"
#include "ui_settings.h"

static const char *TAG = "UI";

#define REFRESH_MS 500
#define TILE_ROWS  2

typedef struct {
    lv_obj_t *tile;
    lv_obj_t *title;
    lv_obj_t *detail;
} tile_t;

enum { TILE_MESSAGES, TILE_STATUS, TILE_SETTINGS, TILE_COUNT };

static struct {
    lv_obj_t *screen;
    lv_obj_t *status;
    tile_t    tiles[TILE_COUNT];
    bool      built;
    uint32_t  shown_version;
    uint32_t  shown_unread;
} s_ui;

static void on_messages(lv_event_t *e)
{
    (void)e;
    ui_chat_open_list();
}

static void on_status(lv_event_t *e)
{
    (void)e;
    ui_home_open();
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

static lv_obj_t *make_tile(lv_obj_t *parent, int16_t width, int16_t height, const char *icon, const char *title,
                           lv_event_cb_t on_click, tile_t *out)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *tile = lv_button_create(parent);
    lg_theme_style_button(tile);
    lv_obj_set_size(tile, width, height);
    lv_obj_set_style_pad_all(tile, t->gap, 0);
    lv_obj_set_style_pad_row(tile, t->gap / 2, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, NULL);

    /* lg_ui_text, not lg_ui_label: these stay on one line. The icon carries the tile, so the
     * words under it are small, and the detail line clips with a dot rather than wrapping. */
    out->tile = tile;
    /* Tiles keep the larger glyph: here the icon is the tile's subject, not a control in a
     * row, so it is not held to the one size the bars and controls share (D36). */
    lg_ui_text(tile, t->font_huge, t->accent, icon);
    out->title = lg_ui_text(tile, t->font_small, t->text, title);
    out->detail = lg_ui_text(tile, t->font_small, t->muted, "");
    lv_obj_set_width(out->detail, LV_PCT(100));
    lv_label_set_long_mode(out->detail, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(out->detail, LV_TEXT_ALIGN_CENTER, 0);
    return tile;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui.built || lv_screen_active() != s_ui.screen) {
        return;
    }
    const hh_status_t *st = ui_status();
    uint32_t unread = ui_notify_unread_total();
    /* Signature over what the tiles show, not the snapshot's version, which changes every
     * second: otherwise every tile's text is rewritten once a second for nothing. */
    uint32_t signature = (uint32_t)st->link * 7u + unread * 31u + st->n_people * 101u + st->n_nodes * 1009u +
                         (st->free_heap / 1024u) * 3u + (uint32_t)(st->preferred_node + 2) * 17u +
                         (st->time_restricted ? 5u : 0u) + (st->grid_time / 60u) * 13u;
    if (signature == s_ui.shown_version) {
        return;
    }
    s_ui.shown_version = signature;
    s_ui.shown_unread = unread;
    const lg_theme_t *t = lg_theme();
    char text[128];   /* a roster name plus an SSID plus a clock reading */

    /* Header: who this handheld is and how it stands on the grid. */
    if (st->link == HH_LINK_ONLINE) {
        if (st->time_restricted) {
            snprintf(text, sizeof(text), "%s on %s, no grid time", st->name, st->node_ssid);
            lv_obj_set_style_text_color(s_ui.status, t->warning, 0);
        } else {
            uint32_t day = st->grid_time % 86400u;
            snprintf(text, sizeof(text), "%s on %s, %02u:%02u", st->name, st->node_ssid,
                     (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u));
            lv_obj_set_style_text_color(s_ui.status, t->success, 0);
        }
    } else if (st->problem[0] != '\0') {
        snprintf(text, sizeof(text), "%s", st->problem);
        lv_obj_set_style_text_color(s_ui.status, t->error, 0);
    } else {
        snprintf(text, sizeof(text), "Looking for an AP");
        lv_obj_set_style_text_color(s_ui.status, t->warning, 0);
    }
    lg_ui_set_text(s_ui.status, text);

    if (unread > 0) {
        snprintf(text, sizeof(text), "%" PRIu32 " new", unread);
        lv_obj_set_style_text_color(s_ui.tiles[TILE_MESSAGES].detail, t->accent, 0);
    } else {
        snprintf(text, sizeof(text), "%u known", st->n_people);
        lv_obj_set_style_text_color(s_ui.tiles[TILE_MESSAGES].detail, t->muted, 0);
    }
    lg_ui_set_text(s_ui.tiles[TILE_MESSAGES].detail, text);

    snprintf(text, sizeof(text), "%u AP%s, %" PRIu32 " KB free", st->n_nodes, st->n_nodes == 1 ? "" : "s",
             st->free_heap / 1024u);
    lg_ui_set_text(s_ui.tiles[TILE_STATUS].detail, text);

    snprintf(text, sizeof(text), "%s", st->preferred_node < 0 ? "auto AP" : "fixed AP");
    lg_ui_set_text(s_ui.tiles[TILE_SETTINGS].detail, text);
}

void ui_launcher_start(const lg_identity_t *identity)
{
    const lg_theme_t *t = lg_theme();
    lg_display_lock(1000);
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    s_ui.screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.screen);
    lv_obj_remove_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.screen, t->gap, 0);

    lv_obj_t *header = lg_ui_column(s_ui.screen, 0);
    /* The same mark as the admin page, a little taller than the name beside it. */
    lv_obj_t *brand = lg_ui_row(header);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lg_ui_logo(brand, lv_font_get_line_height(t->font_title) * 3 / 2);
    lg_ui_text(brand, t->font_title, t->accent, "LocalGrid");
    s_ui.status = lg_ui_label(header, t->font_small, t->muted, "Starting");

    /* Divide what is there: one more gap than tiles, and the header taken off first. */
    lv_obj_update_layout(s_ui.screen);
    int16_t header_h = (int16_t)lv_obj_get_height(header);
    int16_t full_w = (int16_t)(w - t->pad * 2);
    int16_t half_w = (int16_t)((full_w - t->gap) / 2);
    int16_t tile_h = (int16_t)((h - t->pad * 2 - header_h - t->gap * (TILE_ROWS + 1)) / TILE_ROWS);

    lv_obj_t *grid = lg_ui_column(s_ui.screen, t->gap);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, t->gap, 0);
    lv_obj_set_style_pad_row(grid, t->gap, 0);

    make_tile(grid, full_w, tile_h, LV_SYMBOL_ENVELOPE, "Messages", on_messages, &s_ui.tiles[TILE_MESSAGES]);
    make_tile(grid, half_w, tile_h, LV_SYMBOL_WIFI, "Status", on_status, &s_ui.tiles[TILE_STATUS]);
    make_tile(grid, half_w, tile_h, LV_SYMBOL_SETTINGS, "Settings", on_settings, &s_ui.tiles[TILE_SETTINGS]);

    s_ui.built = true;
    lv_screen_load(s_ui.screen);
    lv_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL);

    /* Still under the lock: these create timers and top-layer widgets, and this is the main
     * task, not the drawing task, which is already running the launcher. Building them after
     * the unlock raced LVGL's event and timer lists, and a later screen deletion spun forever
     * in lv_event_mark_deleted on both handhelds. */
    hh_mem_mark("after theme and launcher screen");
    ui_home_build(identity);   /* the Status screen, reached from its tile */
    hh_mem_mark("after Status screen");
    ui_settings_build(identity);
    hh_mem_mark("after Settings screen");
    ui_alert_start();   /* the announcement flash and the emergency takeover */
    hh_mem_mark("after alert layer");
    ui_notify_start();
    hh_mem_mark("after notifications");
    lg_display_unlock();
    ESP_LOGI(TAG, "[UI] Launcher ready: %dx%d panel, tiles %dx%d and %dx%d", (int)w, (int)h, full_w, tile_h,
             half_w, tile_h);
}

void ui_launcher_open(void)
{
    if (s_ui.screen == NULL) {
        return;
    }
    if (!lg_display_lock(3000)) {
        /* Never draw without the lock: two tasks in LVGL at once corrupt its event list, and
         * before the display starts there is no lock at all. */
        ESP_LOGW(TAG, "[UI] Display busy or not started; launcher not opened");
        return;
    }
    lv_screen_load(s_ui.screen);
    s_ui.shown_version = 0;   /* repaint on the next tick */
    lg_display_unlock();
}

lv_obj_t *ui_launcher_screen(void)
{
    return s_ui.screen;
}
