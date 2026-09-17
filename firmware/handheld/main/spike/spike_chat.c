/*
 * The no-LVGL spike, part two: a 1:1 chat (owner, 2026-09-17). Answers whether the hard parts
 * work without a UI library on the Hosyond: a message list that scrolls under a finger, bubbles
 * with wrapped text and emoji, and an on-screen keyboard.
 *
 * What is kept in RAM is a small layout per message (id, position, height, side), never the
 * text: a bubble fetches its message from the service when it is painted. The list is painted
 * as one clipped region; a key press repaints one key; typing repaints the input field.
 */
#include "spike_chat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hh_service.h"
#include "lg_draw.h"
#include "lg_emoji.h"
#include "lg_envelope.h"

static const char *TAG = "UI";

#define C_BG       lg_rgb(0x000000)
#define C_SURFACE  lg_rgb(0x0A140F)
#define C_OUTLINE  lg_rgb(0x1F3A2A)
#define C_TEXT     lg_rgb(0xD2F5DE)
#define C_MUTED    lg_rgb(0x7FA78F)
#define C_ACCENT   lg_rgb(0x5FD38D)
#define C_WARNING  lg_rgb(0xF0B64A)

#define PAD         6
#define GAP         4
#define HEAD_H      30
#define INPUT_H     30
#define KEY_ROW_H   32
#define KEY_ROWS    4
#define LINES_MAX   12
#define DRAG_START  6      /* pixels a finger moves before a press becomes a scroll */
#define KEYS_MAX    40

typedef struct {
    uint32_t id;
    int16_t  y;           /* top, in list coordinates (0 = first message's top) */
    int16_t  h;
    bool     mine;
} bubble_t;

typedef struct {
    lg_rect_t rect;
    char      label[8];
    char      insert;     /* character typed, or a control code below */
} key_t;

enum { K_BACKSPACE = 1, K_HIDE = 2 };

static struct {
    uint16_t   w;
    uint16_t   h;
    uint32_t   peer;
    char       title[HH_NAME_MAX];
    lg_rect_t  list;
    lg_rect_t  field;
    lg_rect_t  send;
    lg_rect_t  back;
    bool       keyboard;
    key_t      keys[KEYS_MAX];
    uint8_t    n_keys;
    int        pressed_key;      /* -1 none */
    bubble_t   bubbles[HH_MESSAGES];
    uint8_t    n_bubbles;
    int16_t    content_h;
    int16_t    scroll;           /* list coordinate at the top of the view */
    uint32_t   shown_messages;
    char       input[HH_TEXT_MAX + 1];
    size_t     input_len;
    /* touch */
    bool       was_down;
    int16_t    down_x;
    int16_t    down_y;
    int16_t    down_scroll;
    bool       dragging;
    /* measurements */
    uint32_t   list_paints;
    uint64_t   list_us;
    uint32_t   key_paints;
    uint64_t   key_us;
} s;

static const lv_font_t *body_font(void)
{
    return &lv_font_montserrat_12;
}

static int16_t bubble_text_w(void)
{
    return (int16_t)(s.list.w * 78 / 100 - 2 * PAD);
}

static bool in_conversation(const hh_message_t *m)
{
    return m->scope == LG_SCOPE_DIRECT && (m->mine ? m->target == s.peer : m->author == s.peer);
}

/* Finds a message by id in the service's list. Only the newest few are ever looked at. */
static bool fetch(uint32_t id, hh_message_t *out)
{
    for (size_t i = 0; i < HH_MESSAGES; i++) {
        if (!hh_service_message(i, out)) {
            return false;
        }
        if (out->id == id) {
            return true;
        }
    }
    return false;
}

static void layout(void)
{
    static hh_message_t m;
    uint16_t starts[LINES_MAX + 1];
    int16_t line_h = body_font()->line_height;
    s.n_bubbles = 0;
    int16_t y = GAP;
    /* Oldest first: find how many there are, then walk back from the oldest. */
    size_t total = 0;
    while (total < HH_MESSAGES && hh_service_message(total, &m)) {
        total++;
    }
    for (size_t i = total; i > 0 && s.n_bubbles < HH_MESSAGES; i--) {
        if (!hh_service_message(i - 1u, &m) || !in_conversation(&m)) {
            continue;
        }
        uint8_t lines = lg_text_wrap(body_font(), &lg_font_emoji_14, m.text, bubble_text_w(), starts, LINES_MAX);
        bubble_t *b = &s.bubbles[s.n_bubbles++];
        b->id = m.id;
        b->mine = m.mine;
        b->y = y;
        b->h = (int16_t)(PAD + (lines ? lines : 1) * line_h + lv_font_montserrat_10.line_height + PAD);
        y = (int16_t)(y + b->h + GAP);
    }
    s.content_h = y;
}

static int16_t max_scroll(void)
{
    return s.content_h > s.list.h ? (int16_t)(s.content_h - s.list.h) : 0;
}

static void paint_list(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_paint_panel(c, &s.list, &s.list, C_BG, C_BG, C_BG, 0, 0);
    static hh_message_t m;
    uint16_t starts[LINES_MAX + 1];
    int16_t line_h = body_font()->line_height;
    int16_t bw_max = (int16_t)(s.list.w * 78 / 100);
    for (uint8_t i = 0; i < s.n_bubbles; i++) {
        const bubble_t *b = &s.bubbles[i];
        int16_t top = (int16_t)(s.list.y + b->y - s.scroll);
        if (top >= c->band.y + c->band.h || top + b->h <= c->band.y) {
            continue;   /* not in this band: no fetch, no paint */
        }
        if (!fetch(b->id, &m)) {
            continue;
        }
        uint8_t lines = lg_text_wrap(body_font(), &lg_font_emoji_14, m.text, bubble_text_w(), starts, LINES_MAX);
        int16_t widest = 0;
        for (uint8_t l = 0; l < lines; l++) {
            char line[HH_TEXT_MAX + 1];
            size_t n = (size_t)(starts[l + 1] - starts[l]);
            memcpy(line, m.text + starts[l], n);
            line[n] = '\0';
            int16_t lw = lg_draw_text_width(body_font(), &lg_font_emoji_14, line);
            widest = lw > widest ? lw : widest;
        }
        char note[32];
        uint32_t day = m.grid_time % 86400u;
        snprintf(note, sizeof(note), "%02u:%02u  %s", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u),
                 !m.mine ? "" : m.state == HH_MSG_READ ? "read" : m.state == HH_MSG_DELIVERED ? "delivered"
                             : m.state == HH_MSG_ACCEPTED ? "sent" : m.state == HH_MSG_PENDING ? "waiting" : "not sent");
        int16_t nw = lg_draw_text_width(&lv_font_montserrat_10, NULL, note);
        int16_t bw = (int16_t)((widest > nw ? widest : nw) + 2 * PAD);
        bw = bw > bw_max ? bw_max : bw;
        lg_rect_t box = { b->mine ? (int16_t)(s.list.x + s.list.w - PAD - bw) : (int16_t)(s.list.x + PAD), top, bw,
                          b->h };
        lg_paint_panel(c, &s.list, &box, b->mine ? C_OUTLINE : C_SURFACE, C_BG, C_OUTLINE, 1, 6);
        for (uint8_t l = 0; l < lines; l++) {
            lg_paint_text(c, &s.list, (int16_t)(box.x + PAD), (int16_t)(top + PAD + l * line_h), body_font(),
                          &lg_font_emoji_14, C_TEXT, m.text + starts[l], (size_t)(starts[l + 1] - starts[l]));
        }
        lg_paint_text(c, &s.list, (int16_t)(box.x + PAD), (int16_t)(top + PAD + (lines ? lines : 1) * line_h),
                      &lv_font_montserrat_10, NULL, m.mine ? C_ACCENT : C_MUTED, note, strlen(note));
    }
}

static void draw_list(void)
{
    int64_t t0 = esp_timer_get_time();
    lg_draw_region(&s.list, paint_list, NULL);
    s.list_paints++;
    s.list_us += (uint64_t)(esp_timer_get_time() - t0);
}

/* ---- input field and keyboard ---- */

static void paint_field(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t row = { 0, s.field.y, (int16_t)s.w, s.field.h };
    lg_paint_panel(c, &row, &row, C_BG, C_BG, C_BG, 0, 0);
    lg_paint_panel(c, &s.field, &s.field, C_SURFACE, C_BG, C_OUTLINE, 1, 4);
    /* The tail of what has been typed, so the cursor end is always visible. */
    const char *shown = s.input;
    int16_t room = (int16_t)(s.field.w - 2 * PAD - 8);
    while (*shown != '\0' && lg_draw_text_width(body_font(), &lg_font_emoji_14, shown) > room) {
        shown++;
        while ((*shown & 0xC0) == 0x80) {
            shown++;   /* never start inside a UTF-8 sequence */
        }
    }
    int16_t top = (int16_t)(s.field.y + (s.field.h - body_font()->line_height) / 2);
    if (s.input_len == 0) {
        lg_paint_text(c, &s.field, (int16_t)(s.field.x + PAD), top, body_font(), NULL, C_MUTED, "Message", 7);
    } else {
        lg_paint_text(c, &s.field, (int16_t)(s.field.x + PAD), top, body_font(), &lg_font_emoji_14, C_TEXT, shown,
                      strlen(shown));
    }
    int16_t cursor = (int16_t)(s.field.x + PAD + (s.input_len ? lg_draw_text_width(body_font(), &lg_font_emoji_14,
                                                                                   shown) : 0));
    lg_rect_t bar = { cursor, (int16_t)(top + 1), 1, (int16_t)(body_font()->line_height - 2) };
    lg_paint_panel(c, &s.field, &bar, C_ACCENT, C_ACCENT, C_ACCENT, 0, 0);
    int16_t sx = (int16_t)(s.send.x + (s.send.w - lg_draw_text_width(&lv_font_montserrat_16, NULL, LV_SYMBOL_OK)) / 2);
    lg_paint_text(c, &s.send, sx, (int16_t)(s.send.y + (s.send.h - lv_font_montserrat_16.line_height) / 2),
                  &lv_font_montserrat_16, NULL, C_ACCENT, LV_SYMBOL_OK, strlen(LV_SYMBOL_OK));
}

static void draw_field(void)
{
    lg_rect_t row = { 0, s.field.y, (int16_t)s.w, s.field.h };
    lg_draw_region(&row, paint_field, NULL);
}

static void paint_key(const lg_canvas_t *c, void *ctx)
{
    const key_t *k = ctx;
    bool pressed = s.pressed_key >= 0 && &s.keys[s.pressed_key] == k;
    lg_paint_panel(c, &k->rect, &k->rect, pressed ? C_OUTLINE : C_SURFACE, C_BG, C_OUTLINE, 1, 4);
    const lv_font_t *f = k->insert < ' ' ? &lv_font_montserrat_16 : &lv_font_montserrat_14;
    int16_t tw = lg_draw_text_width(f, NULL, k->label);
    lg_paint_text(c, &k->rect, (int16_t)(k->rect.x + (k->rect.w - tw) / 2),
                  (int16_t)(k->rect.y + (k->rect.h - f->line_height) / 2), f, NULL, C_TEXT, k->label, strlen(k->label));
}

static void draw_key(const key_t *k)
{
    int64_t t0 = esp_timer_get_time();
    lg_draw_region(&k->rect, paint_key, (void *)k);
    s.key_paints++;
    s.key_us += (uint64_t)(esp_timer_get_time() - t0);
}

static void paint_keyboard_bg(const lg_canvas_t *c, void *ctx)
{
    const lg_rect_t *area = ctx;
    lg_paint_panel(c, area, area, C_BG, C_BG, C_BG, 0, 0);
    for (uint8_t i = 0; i < s.n_keys; i++) {
        const key_t *k = &s.keys[i];
        if (k->rect.y < c->band.y + c->band.h && k->rect.y + k->rect.h > c->band.y) {
            paint_key(c, (void *)k);
        }
    }
}

static void add_row(const char *chars, int16_t y, int16_t x0, int16_t key_w)
{
    for (const char *p = chars; *p != '\0' && s.n_keys < KEYS_MAX; p++) {
        key_t *k = &s.keys[s.n_keys++];
        k->rect = (lg_rect_t){ (int16_t)(x0 + (p - chars) * key_w), y, (int16_t)(key_w - 2), (int16_t)(KEY_ROW_H - 3) };
        k->label[0] = *p;
        k->label[1] = '\0';
        k->insert = *p;
    }
}

static void add_key(const char *label, char insert, int16_t x, int16_t y, int16_t w)
{
    if (s.n_keys >= KEYS_MAX) {
        return;
    }
    key_t *k = &s.keys[s.n_keys++];
    k->rect = (lg_rect_t){ x, y, (int16_t)(w - 2), (int16_t)(KEY_ROW_H - 3) };
    snprintf(k->label, sizeof(k->label), "%s", label);
    k->insert = insert;
}

static void place(bool keyboard)
{
    s.keyboard = keyboard;
    int16_t kb_h = keyboard ? (int16_t)(KEY_ROWS * KEY_ROW_H + GAP) : 0;
    int16_t input_y = (int16_t)(s.h - kb_h - INPUT_H - GAP);
    s.back = (lg_rect_t){ (int16_t)(s.w - 40), 0, 40, HEAD_H };
    s.list = (lg_rect_t){ 0, HEAD_H, (int16_t)s.w, (int16_t)(input_y - HEAD_H - GAP) };
    s.field = (lg_rect_t){ PAD, input_y, (int16_t)(s.w - 2 * PAD - 36), INPUT_H };
    s.send = (lg_rect_t){ (int16_t)(s.w - PAD - 32), input_y, 32, INPUT_H };
    s.n_keys = 0;
    if (keyboard) {
        int16_t y0 = (int16_t)(s.h - KEY_ROWS * KEY_ROW_H);
        int16_t kw = (int16_t)(s.w / 10);
        int16_t x0 = (int16_t)((s.w - 10 * kw) / 2);
        add_row("qwertyuiop", y0, x0, kw);
        add_row("asdfghjkl", (int16_t)(y0 + KEY_ROW_H), (int16_t)(x0 + kw / 2), kw);
        add_row("zxcvbnm", (int16_t)(y0 + 2 * KEY_ROW_H), (int16_t)(x0 + kw), kw);
        add_key(LV_SYMBOL_BACKSPACE, K_BACKSPACE, (int16_t)(x0 + 8 * kw), (int16_t)(y0 + 2 * KEY_ROW_H), (int16_t)(2 * kw));
        add_key(LV_SYMBOL_DOWN, K_HIDE, x0, (int16_t)(y0 + 3 * KEY_ROW_H), (int16_t)(2 * kw));
        add_key(",", ',', (int16_t)(x0 + 2 * kw), (int16_t)(y0 + 3 * KEY_ROW_H), kw);
        add_key("space", ' ', (int16_t)(x0 + 3 * kw), (int16_t)(y0 + 3 * KEY_ROW_H), (int16_t)(5 * kw));
        add_key(".", '.', (int16_t)(x0 + 8 * kw), (int16_t)(y0 + 3 * KEY_ROW_H), kw);
        add_key("?", '?', (int16_t)(x0 + 9 * kw), (int16_t)(y0 + 3 * KEY_ROW_H), kw);
    }
    if (s.scroll > max_scroll()) {
        s.scroll = max_scroll();
    }
}

static void draw_all(void)
{
    lg_rect_t head = { 0, 0, (int16_t)s.w, HEAD_H };
    lg_box_t title;
    memset(&title, 0, sizeof(title));
    title.rect = head;
    title.bg = title.outside = C_BG;
    title.font = &lv_font_montserrat_14;
    title.fallback = &lg_font_emoji_14;
    title.fg = C_ACCENT;
    title.pad = PAD;
    snprintf(title.text, sizeof(title.text), "%s", s.title);
    lg_draw_box(&title);
    lg_box_t back = title;
    back.rect = s.back;
    back.font = &lv_font_montserrat_16;
    back.align = LG_ALIGN_CENTER;
    snprintf(back.text, sizeof(back.text), "%s", LV_SYMBOL_LEFT);
    lg_draw_box(&back);
    draw_list();
    lg_rect_t gap = { 0, (int16_t)(s.list.y + s.list.h), (int16_t)s.w, GAP };
    lg_draw_fill(&gap, C_BG);
    draw_field();
    if (s.keyboard) {
        lg_rect_t area = { 0, (int16_t)(s.field.y + s.field.h), (int16_t)s.w, (int16_t)(s.h - s.field.y - s.field.h) };
        lg_draw_region(&area, paint_keyboard_bg, &area);
    }
}

void spike_chat_open(uint16_t w, uint16_t h, const hh_status_t *st)
{
    memset(&s, 0, sizeof(s));
    s.w = w;
    s.h = h;
    s.pressed_key = -1;
    if (st->n_people > 0) {
        s.peer = st->people[0].device;
        snprintf(s.title, sizeof(s.title), "%s", st->people[0].name);
    } else {
        snprintf(s.title, sizeof(s.title), "No one seen yet");
    }
    place(false);
    layout();
    s.scroll = max_scroll();   /* newest at the bottom, in view */
    s.shown_messages = st->messages_version;
    draw_all();
    ESP_LOGI(TAG, "[UI] Spike chat with %s: %u message(s), content %d px", s.title, s.n_bubbles, s.content_h);
}

void spike_chat_refresh(const hh_status_t *st)
{
    if (st->messages_version == s.shown_messages) {
        return;
    }
    s.shown_messages = st->messages_version;
    bool at_bottom = s.scroll >= max_scroll() - 4;
    layout();
    if (at_bottom) {
        s.scroll = max_scroll();   /* follow new messages only if the reader was already at the end */
    }
    draw_list();
}

static void type_key(const key_t *k)
{
    if (k->insert == K_HIDE) {
        place(false);
        draw_all();
        return;
    }
    if (k->insert == K_BACKSPACE) {
        while (s.input_len > 0) {
            char c = s.input[--s.input_len];
            if ((c & 0xC0) != 0x80) {
                break;   /* removed the start of a character */
            }
        }
        s.input[s.input_len] = '\0';
    } else if (s.input_len < HH_TEXT_MAX) {
        s.input[s.input_len++] = k->insert;
        s.input[s.input_len] = '\0';
    }
    draw_field();
}

bool spike_chat_touch(int16_t x, int16_t y, bool down)
{
    if (down && !s.was_down) {
        s.was_down = true;
        s.down_x = x;
        s.down_y = y;
        s.down_scroll = s.scroll;
        s.dragging = false;
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (lg_rect_hit(&s.keys[i].rect, x, y)) {
                s.pressed_key = i;
                draw_key(&s.keys[i]);   /* the pressed look: one key repainted */
            }
        }
        return false;
    }
    if (down) {
        int16_t dy = (int16_t)(y - s.down_y);
        if (!s.dragging && s.pressed_key < 0 && lg_rect_hit(&s.list, s.down_x, s.down_y) &&
            (dy > DRAG_START || dy < -DRAG_START)) {
            s.dragging = true;
        }
        if (s.dragging) {
            int16_t want = (int16_t)(s.down_scroll - dy);
            want = want < 0 ? 0 : (want > max_scroll() ? max_scroll() : want);
            if (want != s.scroll) {
                s.scroll = want;
                draw_list();
            }
        }
        return false;
    }
    if (!s.was_down) {
        return false;
    }
    /* Released. */
    s.was_down = false;
    if (s.pressed_key >= 0) {
        key_t *k = &s.keys[s.pressed_key];
        s.pressed_key = -1;
        draw_key(k);
        if (lg_rect_hit(&k->rect, x, y)) {
            type_key(k);
        }
        return false;
    }
    if (s.dragging) {
        s.dragging = false;
        return false;
    }
    if (lg_rect_hit(&s.back, x, y)) {
        return true;
    }
    if (lg_rect_hit(&s.field, x, y) && !s.keyboard) {
        place(true);
        draw_all();
    } else if (lg_rect_hit(&s.send, x, y) && s.input_len > 0 && s.peer != 0) {
        esp_err_t err = hh_service_send(LG_SCOPE_DIRECT, s.peer, false, s.input);
        ESP_LOGI(TAG, "[UI] Spike chat send: %s", err == ESP_OK ? "queued" : esp_err_to_name(err));
        if (err == ESP_OK) {
            s.input_len = 0;
            s.input[0] = '\0';
            draw_field();
        }
    }
    return false;
}

void spike_chat_scroll_by(int16_t dy)
{
    int16_t want = (int16_t)(s.scroll + dy);
    want = want < 0 ? 0 : (want > max_scroll() ? max_scroll() : want);
    if (want != s.scroll) {
        s.scroll = want;
        draw_list();
    }
}

void spike_chat_keyboard(bool show)
{
    place(show);
    draw_all();
}

void spike_chat_type(const char *text)
{
    for (const char *p = text; *p != '\0' && s.input_len < HH_TEXT_MAX; p++) {
        key_t k = { .insert = *p };
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (s.keys[i].insert == *p) {
                s.pressed_key = i;
                draw_key(&s.keys[i]);
                s.pressed_key = -1;
                draw_key(&s.keys[i]);   /* press and release, as a finger would */
            }
        }
        type_key(&k);
    }
}

void spike_chat_log(void)
{
    ESP_LOGI(TAG, "[UI] Spike chat: %u bubbles, content %d px, list paints %" PRIu32 " (avg %" PRIu64
             " ms), key paints %" PRIu32 " (avg %" PRIu64 " ms)", s.n_bubbles, s.content_h, s.list_paints,
             s.list_paints ? s.list_us / s.list_paints / 1000u : 0u, s.key_paints,
             s.key_paints ? s.key_us / s.key_paints / 1000u : 0u);
}
