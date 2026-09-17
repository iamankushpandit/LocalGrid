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
#include <stdio.h>
#include <string.h>

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
#include "spike_chat.h"

static const char *TAG = "UI";

/* The Terminal theme's roles (lg_theme.c), as RGB565: the spike keeps its own copy of the table. */
#define C_BG       lg_rgb(0x000000)
#define C_SURFACE  lg_rgb(0x0A140F)
#define C_OUTLINE  lg_rgb(0x1F3A2A)
#define C_TEXT     lg_rgb(0xD2F5DE)
#define C_MUTED    lg_rgb(0x7FA78F)
#define C_ACCENT   lg_rgb(0x5FD38D)
#define C_WARNING  lg_rgb(0xF0B64A)
#define C_ERROR    lg_rgb(0xFF6B6B)

#define TICK_MS      20
#define REFRESH_MS   250
#define STATS_MS     10000
#define PAD          8
#define GAP          5

typedef enum { SCREEN_LAUNCHER, SCREEN_STATUS, SCREEN_CHAT } screen_t;

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
} s;

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
    s.heading = text_box(0, PAD, (int16_t)s.w, 26, &lv_font_montserrat_20, C_ACCENT, C_BG, LG_ALIGN_LEFT, "LocalGrid");
    s.heading.pad = PAD;
    s.subline = text_box(0, PAD + 26, (int16_t)s.w, 18, &lv_font_montserrat_12, C_MUTED, C_BG, LG_ALIGN_LEFT, "Starting");
    s.subline.pad = PAD;

    static const char *icons[TILE_COUNT] = { LV_SYMBOL_ENVELOPE, LV_SYMBOL_LIST, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS };
    static const char *titles[TILE_COUNT] = { "Messages", "Groups", "Status", "Settings" };
    int16_t top = (int16_t)(PAD + 26 + 18 + GAP);
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
    if (s.screen == SCREEN_CHAT) {
        spike_chat_close();
    }
    s.screen = SCREEN_LAUNCHER;
    lg_rect_t all = { 0, 0, (int16_t)s.w, (int16_t)s.h };
    lg_draw_fill(&all, C_BG);
    lg_draw_box(&s.heading);
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
    if (s.screen == SCREEN_CHAT) {
        spike_chat_close();
    }
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
    hh_service_status(&s.st);
    const hh_status_t *st = &s.st;
    if (s.screen == SCREEN_CHAT) {
        spike_chat_refresh(st);
        return;
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
        snprintf(text, sizeof(text), "%u known", st->n_people);
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

static void on_tap(int16_t x, int16_t y, bool down)
{
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
            if (was == TILE_STATUS && hit == TILE_STATUS) {
                show_status();
                refresh();
            } else if (was == TILE_MESSAGES && hit == TILE_MESSAGES) {
                hh_service_status(&s.st);
                s.screen = SCREEN_CHAT;
                spike_chat_open(s.w, s.h, &s.st);
            } else {
                draw_tile(&s.tiles[was], false);
            }
        }
    } else if (s.screen == SCREEN_CHAT) {
        if (spike_chat_touch(x, y, down)) {
            show_launcher();
            refresh();
        }
    } else {
        bool on_back = lg_rect_hit(&s.back.rect, x, y);
        if (down && s.pressed < 0 && on_back) {
            s.pressed = -2;
        } else if (!down && s.pressed == -2) {
            s.pressed = -1;
            if (on_back) {
                show_launcher();
                refresh();
            }
        }
    }
}

static QueueHandle_t s_requests;

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
    case SPIKE_CHAT:
        hh_service_status(&s.st);
        s.screen = SCREEN_CHAT;
        spike_chat_open(s.w, s.h, &s.st);
        break;
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
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int16_t x = 0;
        int16_t y = 0;
        bool down = lg_draw_touch(&x, &y);
        if (down || was_down) {
            on_tap(x, y, down);
        }
        was_down = down;
        spike_req_t req;
        while (xQueueReceive(s_requests, &req, 0) == pdTRUE) {
            handle_request(&req);
        }
        if (s.screen == SCREEN_CHAT) {
            spike_chat_tick(now);
        }
        if (now - last_refresh >= REFRESH_MS) {
            last_refresh = now;
            refresh();
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
    show_launcher();
    hh_mem_mark("after spike launcher");
    if (xTaskCreate(spike_task, "spike_ui", 4096, NULL, 4, NULL) != pdPASS) {
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
