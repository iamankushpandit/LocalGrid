/*
 * The no-LVGL spike, part two: a 1:1 chat (owner, 2026-09-17).
 *
 * - The message list is the panel's hardware scroll area: dragging moves it with one command and
 *   paints only the rows that scroll into view, so scrolling costs a few pixel rows per step.
 * - Bubbles carry the delivery icons of D42: a clock while this handheld holds a message, an up
 *   arrow once an AP took it, a down arrow once the other handheld has it, an eye once it was
 *   shown there. The colours are the theme's marker roles.
 * - A message that arrives while the reader is scrolled up does not move the list. A down arrow
 *   flashes slowly in the middle of the list instead, and goes when the reader reaches the end.
 * - The keyboard has letters with shift (tap once for one capital, twice for caps lock), a
 *   numbers and symbols page, and two emoji pages. A press repaints one key.
 *
 * RAM holds a layout per message (id, position, height, side), never its text: a bubble fetches
 * its message from the service when painted.
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

/* Theme roles (lg_theme.c), as RGB565: the spike's copy of the table. */
#define C_BG         lg_rgb(0x000000)
#define C_SURFACE    lg_rgb(0x0A140F)
#define C_OUTLINE    lg_rgb(0x1F3A2A)
#define C_TEXT       lg_rgb(0xD2F5DE)
#define C_MUTED      lg_rgb(0x7FA78F)
#define C_ACCENT     lg_rgb(0x5FD38D)
#define C_MARK_WAIT  lg_rgb(0x7FA78F)
#define C_MARK_NODE  lg_rgb(0x4A8FD4)
#define C_MARK_DELIV lg_rgb(0x5FD38D)
#define C_MARK_READ  lg_rgb(0xB98CFF)
#define C_ERROR      lg_rgb(0xFF6B6B)

#define PAD          6
#define GAP          4
#define HEAD_H       30
#define INPUT_H      30
#define KEY_ROW_H    32
#define KEY_ROWS     4
#define LINES_MAX    12
#define DRAG_START   6       /* pixels a finger moves before a press becomes a scroll */
#define KEYS_MAX     40
#define FLASH_MS     700     /* the new-message arrow: on this long, off this long */
#define ARROW_SIZE   36
#define EMOJI_PER_PAGE 24u

#define MARK_WAIT      "\xF0\x9F\x95\x93"   /* U+1F553 clock: this handheld still holds it */

typedef struct {
    uint32_t id;
    int16_t  y;           /* top, in list coordinates (0 = above the first message) */
    int16_t  h;
    bool     mine;
} bubble_t;

typedef enum {
    K_TEXT,
    K_BACKSPACE,
    K_HIDE,
    K_SHIFT,
    K_PAGE_LETTERS,
    K_PAGE_NUMBERS,
    K_PAGE_EMOJI,
    K_EMOJI_PREV,
    K_EMOJI_NEXT,
} key_action_t;

typedef struct {
    lg_rect_t rect;
    char      label[8];
    char      text[8];    /* UTF-8 inserted by K_TEXT */
    uint8_t   action;
} key_t;

typedef enum { PAGE_LETTERS, PAGE_NUMBERS, PAGE_EMOJI } page_t;

static struct {
    uint16_t   w;
    uint16_t   h;
    uint32_t   peer;
    char       title[HH_NAME_MAX];
    lg_rect_t  list;
    lg_rect_t  field;
    lg_rect_t  send;
    lg_rect_t  back;
    lg_rect_t  arrow;
    bool       keyboard;
    page_t     page;
    uint8_t    shift;            /* 0 off, 1 next letter only, 2 caps lock */
    uint8_t    emoji_page;
    key_t      keys[KEYS_MAX];
    uint8_t    n_keys;
    int        pressed_key;      /* -1 none */
    bubble_t   bubbles[HH_MESSAGES];
    uint8_t    n_bubbles;
    int16_t    content_h;
    int16_t    scroll;           /* list coordinate at the top of the view */
    uint32_t   shown_messages;
    uint32_t   newest_id;
    bool       new_below;        /* a message arrived while the reader was scrolled up */
    bool       arrow_on;         /* the flash phase */
    bool       arrow_drawn;      /* the arrow is on the panel now */
    uint32_t   flash_ms;
    char       input[HH_TEXT_MAX + 1];
    size_t     input_len;
    bool       was_down;
    int16_t    down_x;
    int16_t    down_y;
    int16_t    down_scroll;
    bool       dragging;
    uint32_t   list_paints;
    uint64_t   list_us;
    uint32_t   scroll_steps;
    uint64_t   scroll_us;
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
        b->h = (int16_t)(PAD + (lines ? lines : 1) * line_h + lv_font_montserrat_12.line_height + PAD / 2);
        y = (int16_t)(y + b->h + GAP);
    }
    s.content_h = y;
}

static int16_t max_scroll(void)
{
    return s.content_h > s.list.h ? (int16_t)(s.content_h - s.list.h) : 0;
}

static bool at_bottom(void)
{
    return s.scroll >= max_scroll() - 2;
}

/* The D42 marker for our own message: glyph and colour. */
static const char *marker(const hh_message_t *m, lg_color_t *colour)
{
    switch (m->state) {
    case HH_MSG_PENDING:   *colour = C_MARK_WAIT;  return MARK_WAIT;
    case HH_MSG_ACCEPTED:  *colour = C_MARK_NODE;  return LV_SYMBOL_UPLOAD;
    case HH_MSG_DELIVERED: *colour = C_MARK_DELIV; return LV_SYMBOL_DOWNLOAD;
    case HH_MSG_READ:      *colour = C_MARK_READ;  return LV_SYMBOL_EYE_OPEN;
    default:               *colour = C_ERROR;      return LV_SYMBOL_WARNING;
    }
}

static void paint_arrow(const lg_canvas_t *c)
{
    lg_paint_panel(c, &s.list, &s.arrow, C_OUTLINE, C_BG, C_ACCENT, 1, ARROW_SIZE / 2);
    const lv_font_t *f = &lv_font_montserrat_16;
    int16_t tw = lg_draw_text_width(f, NULL, LV_SYMBOL_DOWN);
    lg_paint_text(c, &s.arrow, (int16_t)(s.arrow.x + (s.arrow.w - tw) / 2),
                  (int16_t)(s.arrow.y + (s.arrow.h - f->line_height) / 2), f, NULL, C_ACCENT, LV_SYMBOL_DOWN,
                  strlen(LV_SYMBOL_DOWN));
}

/* Paints what belongs in the list's rows inside the canvas band; with_arrow adds the flashing
 * arrow when it is in its on phase. */
static void paint_list_content(const lg_canvas_t *c, bool with_arrow)
{
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
        char clock[8];
        uint32_t day = m.grid_time % 86400u;
        snprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u));
        lg_color_t mark_colour = C_MUTED;
        const char *mark = m.mine ? marker(&m, &mark_colour) : NULL;
        int16_t note_w = (int16_t)(lg_draw_text_width(&lv_font_montserrat_10, NULL, clock) +
                                   (mark ? 4 + lg_draw_text_width(&lv_font_montserrat_12, &lg_font_emoji_14, mark) : 0));
        int16_t bw = (int16_t)((widest > note_w ? widest : note_w) + 2 * PAD);
        bw = bw > bw_max ? bw_max : bw;
        lg_rect_t box = { m.mine ? (int16_t)(s.list.x + s.list.w - PAD - bw) : (int16_t)(s.list.x + PAD), top, bw,
                          b->h };
        lg_paint_panel(c, &s.list, &box, m.mine ? C_OUTLINE : C_SURFACE, C_BG, C_OUTLINE, 1, 6);
        for (uint8_t l = 0; l < lines; l++) {
            lg_paint_text(c, &s.list, (int16_t)(box.x + PAD), (int16_t)(top + PAD + l * line_h), body_font(),
                          &lg_font_emoji_14, C_TEXT, m.text + starts[l], (size_t)(starts[l + 1] - starts[l]));
        }
        int16_t note_top = (int16_t)(top + PAD + (lines ? lines : 1) * line_h);
        int16_t nx = (int16_t)(box.x + box.w - PAD - note_w);
        lg_paint_text(c, &s.list, nx, (int16_t)(note_top + 1), &lv_font_montserrat_10, NULL, C_MUTED, clock,
                      strlen(clock));
        if (mark != NULL) {
            lg_paint_text(c, &s.list, (int16_t)(nx + note_w - lg_draw_text_width(&lv_font_montserrat_12,
                                                                                &lg_font_emoji_14, mark)),
                          note_top, &lv_font_montserrat_12, &lg_font_emoji_14, mark_colour, mark, strlen(mark));
        }
    }
    if (with_arrow) {
        paint_arrow(c);
    }
}

static void paint_list(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    paint_list_content(c, s.new_below && s.arrow_on);
}

static void paint_list_plain(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    paint_list_content(c, false);
}

static void draw_list(void)
{
    int64_t t0 = esp_timer_get_time();
    lg_draw_region(&s.list, paint_list, NULL);
    s.arrow_drawn = s.new_below && s.arrow_on;
    s.list_paints++;
    s.list_us += (uint64_t)(esp_timer_get_time() - t0);
}

/* Redraws only the arrow's square: with the arrow, or with the list that belongs under it. */
static void draw_arrow(bool on)
{
    lg_draw_region(&s.arrow, on ? paint_list : paint_list_plain, NULL);
    s.arrow_drawn = on;
}

/* Scrolls the list to `want`: the panel moves the rows, and only the rows that come into view
 * are painted. The arrow is taken off first and put back after, so it never scrolls away. */
static void scroll_to(int16_t want)
{
    want = want < 0 ? 0 : (want > max_scroll() ? max_scroll() : want);
    int16_t d = (int16_t)(want - s.scroll);
    if (d == 0) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    if (d >= s.list.h || -d >= s.list.h) {
        s.scroll = want;
        draw_list();
    } else {
        bool had_arrow = s.arrow_drawn;
        if (had_arrow) {
            draw_arrow(false);
        }
        lg_draw_scroll(d);
        s.scroll = want;
        lg_rect_t strip = d > 0 ? (lg_rect_t){ s.list.x, (int16_t)(s.list.y + s.list.h - d), s.list.w, d }
                                : (lg_rect_t){ s.list.x, s.list.y, s.list.w, (int16_t)-d };
        lg_draw_region(&strip, paint_list_plain, NULL);
        if (had_arrow) {
            draw_arrow(true);
        }
    }
    s.scroll_steps++;
    s.scroll_us += (uint64_t)(esp_timer_get_time() - t0);
    if (s.new_below && at_bottom()) {
        s.new_below = false;   /* the reader reached the new message */
        if (s.arrow_drawn) {
            draw_arrow(false);
        }
        ESP_LOGI(TAG, "[UI] Spike chat: reached the newest message; arrow gone");
    }
}

/* ---- input field and keyboard ---- */

static void paint_field(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t row = { 0, s.field.y, (int16_t)s.w, s.field.h };
    lg_paint_panel(c, &row, &row, C_BG, C_BG, C_BG, 0, 0);
    lg_paint_panel(c, &s.field, &s.field, C_SURFACE, C_BG, C_OUTLINE, 1, 4);
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
    int16_t cursor = (int16_t)(s.field.x + PAD +
                               (s.input_len ? lg_draw_text_width(body_font(), &lg_font_emoji_14, shown) : 0));
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
    bool locked = k->action == K_SHIFT && s.shift == 2;
    lg_paint_panel(c, &k->rect, &k->rect, pressed || locked ? C_OUTLINE : C_SURFACE, C_BG,
                   locked ? C_ACCENT : C_OUTLINE, 1, 4);
    const lv_font_t *f = k->action == K_TEXT ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const lv_font_t *fb = s.page == PAGE_EMOJI ? &lg_font_emoji_20 : &lg_font_emoji_14;
    lg_color_t fg = (k->action == K_SHIFT && s.shift > 0) ? C_ACCENT : C_TEXT;
    int16_t tw = lg_draw_text_width(f, fb, k->label);
    lg_paint_text(c, &k->rect, (int16_t)(k->rect.x + (k->rect.w - tw) / 2),
                  (int16_t)(k->rect.y + (k->rect.h - f->line_height) / 2), f, fb, fg, k->label, strlen(k->label));
}

static void draw_key(const key_t *k)
{
    int64_t t0 = esp_timer_get_time();
    lg_draw_region(&k->rect, paint_key, (void *)k);
    s.key_paints++;
    s.key_us += (uint64_t)(esp_timer_get_time() - t0);
}

static void paint_keyboard(const lg_canvas_t *c, void *ctx)
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

static lg_rect_t keyboard_area(void)
{
    return (lg_rect_t){ 0, (int16_t)(s.field.y + s.field.h), (int16_t)s.w, (int16_t)(s.h - s.field.y - s.field.h) };
}

static void draw_keyboard(void)
{
    lg_rect_t area = keyboard_area();
    lg_draw_region(&area, paint_keyboard, &area);
}

static key_t *add_key(uint8_t action, const char *label, const char *text, int16_t x, int16_t y, int16_t w)
{
    if (s.n_keys >= KEYS_MAX) {
        return NULL;
    }
    key_t *k = &s.keys[s.n_keys++];
    k->rect = (lg_rect_t){ x, y, (int16_t)(w - 2), (int16_t)(KEY_ROW_H - 3) };
    snprintf(k->label, sizeof(k->label), "%s", label);
    snprintf(k->text, sizeof(k->text), "%s", text != NULL ? text : "");
    k->action = action;
    return k;
}

/* One row of single-character keys, upper case when shift is on. */
static void add_chars(const char *chars, int16_t x, int16_t y, int16_t kw, bool upper)
{
    for (const char *p = chars; *p != '\0'; p++) {
        char one[2] = { upper && *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p, '\0' };
        add_key(K_TEXT, one, one, (int16_t)(x + (p - chars) * kw), y, kw);
    }
}

static void build_keys(void)
{
    s.n_keys = 0;
    if (!s.keyboard) {
        return;
    }
    int16_t kw = (int16_t)(s.w / 10);
    int16_t x0 = (int16_t)((s.w - 10 * kw) / 2);
    int16_t y0 = (int16_t)(s.h - KEY_ROWS * KEY_ROW_H);
    int16_t r3 = (int16_t)(y0 + 3 * KEY_ROW_H);
    int16_t wide = (int16_t)(kw * 3 / 2);
    switch (s.page) {
    case PAGE_LETTERS: {
        bool upper = s.shift > 0;
        add_chars("qwertyuiop", x0, y0, kw, upper);
        add_chars("asdfghjkl", (int16_t)(x0 + kw / 2), (int16_t)(y0 + KEY_ROW_H), kw, upper);
        add_key(K_SHIFT, LV_SYMBOL_UP, NULL, x0, (int16_t)(y0 + 2 * KEY_ROW_H), wide);
        add_chars("zxcvbnm", (int16_t)(x0 + wide), (int16_t)(y0 + 2 * KEY_ROW_H), kw, upper);
        add_key(K_BACKSPACE, LV_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 7 * kw), (int16_t)(y0 + 2 * KEY_ROW_H),
                wide);
        add_key(K_PAGE_NUMBERS, "123", NULL, x0, r3, wide);
        break;
    }
    case PAGE_NUMBERS:
        add_chars("1234567890", x0, y0, kw, false);
        add_chars("-/:;()$&@", (int16_t)(x0 + kw / 2), (int16_t)(y0 + KEY_ROW_H), kw, false);
        add_chars("_.,?!'\"", (int16_t)(x0 + wide), (int16_t)(y0 + 2 * KEY_ROW_H), kw, false);
        add_key(K_TEXT, "#", "#", x0, (int16_t)(y0 + 2 * KEY_ROW_H), wide);
        add_key(K_BACKSPACE, LV_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 7 * kw), (int16_t)(y0 + 2 * KEY_ROW_H),
                wide);
        add_key(K_PAGE_LETTERS, "ABC", NULL, x0, r3, wide);
        break;
    case PAGE_EMOJI: {
        uint8_t first = (uint8_t)(s.emoji_page * EMOJI_PER_PAGE);
        int16_t ew = (int16_t)(s.w / 8);
        for (uint8_t i = 0; i < EMOJI_PER_PAGE && first + i < LG_EMOJI_COUNT; i++) {
            add_key(K_TEXT, LG_EMOJI[first + i], LG_EMOJI[first + i], (int16_t)(i % 8 * ew),
                    (int16_t)(y0 + (i / 8) * KEY_ROW_H), ew);
        }
        add_key(K_PAGE_LETTERS, "ABC", NULL, x0, r3, wide);
        add_key(K_EMOJI_PREV, LV_SYMBOL_LEFT, NULL, (int16_t)(x0 + wide), r3, kw);
        add_key(K_EMOJI_NEXT, LV_SYMBOL_RIGHT, NULL, (int16_t)(x0 + wide + kw), r3, kw);
        add_key(K_TEXT, "space", " ", (int16_t)(x0 + wide + 2 * kw), r3, (int16_t)(4 * kw));
        add_key(K_BACKSPACE, LV_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 6 * kw), r3, kw);
        add_key(K_HIDE, LV_SYMBOL_DOWN, NULL, (int16_t)(x0 + wide + 7 * kw), r3, wide);
        return;
    }
    }
    /* The bottom row of the letters and numbers pages. */
    add_key(K_PAGE_EMOJI, LG_EMOJI[0], NULL, (int16_t)(x0 + wide), r3, kw);
    add_key(K_TEXT, ",", ",", (int16_t)(x0 + wide + kw), r3, kw);
    add_key(K_TEXT, "space", " ", (int16_t)(x0 + wide + 2 * kw), r3, (int16_t)(4 * kw));
    add_key(K_TEXT, ".", ".", (int16_t)(x0 + wide + 6 * kw), r3, kw);
    add_key(K_HIDE, LV_SYMBOL_DOWN, NULL, (int16_t)(x0 + wide + 7 * kw), r3, wide);
}

static void place(bool keyboard)
{
    s.keyboard = keyboard;
    int16_t kb_h = keyboard ? (int16_t)(KEY_ROWS * KEY_ROW_H + GAP) : 0;
    int16_t input_y = (int16_t)(s.h - kb_h - INPUT_H - GAP);
    s.back = (lg_rect_t){ (int16_t)(s.w - 40), 0, 40, HEAD_H };
    s.list = (lg_rect_t){ 0, HEAD_H, (int16_t)s.w, (int16_t)(input_y - HEAD_H - GAP) };
    s.arrow = (lg_rect_t){ (int16_t)((s.w - ARROW_SIZE) / 2), (int16_t)(s.list.y + (s.list.h - ARROW_SIZE) / 2),
                           ARROW_SIZE, ARROW_SIZE };
    s.field = (lg_rect_t){ PAD, input_y, (int16_t)(s.w - 2 * PAD - 36), INPUT_H };
    s.send = (lg_rect_t){ (int16_t)(s.w - PAD - 32), input_y, 32, INPUT_H };
    build_keys();
    if (s.scroll > max_scroll()) {
        s.scroll = max_scroll();
    }
    /* The list is the panel's scroll area; the header, field, and keyboard stay fixed. */
    lg_draw_scroll_area(s.list.y, s.list.h);
}

static void draw_all(void)
{
    lg_box_t title;
    memset(&title, 0, sizeof(title));
    title.rect = (lg_rect_t){ 0, 0, (int16_t)s.w, HEAD_H };
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
        draw_keyboard();
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
    s.newest_id = s.n_bubbles ? s.bubbles[s.n_bubbles - 1u].id : 0;
    draw_all();
    ESP_LOGI(TAG, "[UI] Spike chat with %s: %u message(s), content %d px", s.title, s.n_bubbles, s.content_h);
}

void spike_chat_close(void)
{
    lg_draw_scroll_area(0, 0);   /* the next screen is drawn unscrolled */
}

void spike_chat_refresh(const hh_status_t *st)
{
    if (st->messages_version != s.shown_messages) {
        s.shown_messages = st->messages_version;
        bool was_at_bottom = at_bottom();
        layout();
        uint32_t newest = s.n_bubbles ? s.bubbles[s.n_bubbles - 1u].id : 0;
        bool arrived = newest != s.newest_id && s.n_bubbles && !s.bubbles[s.n_bubbles - 1u].mine;
        s.newest_id = newest;
        if (was_at_bottom || !arrived) {
            if (was_at_bottom) {
                s.scroll = max_scroll();   /* the reader was at the end: follow the conversation */
            }
            draw_list();
        } else {
            s.new_below = true;   /* the reader is scrolled up: leave the list, flash the arrow */
            s.arrow_on = true;
            s.flash_ms = (uint32_t)(esp_timer_get_time() / 1000);
            draw_list();
            ESP_LOGI(TAG, "[UI] Spike chat: new message below; arrow flashing");
        }
    }
}

void spike_chat_tick(uint32_t now_ms)
{
    if (!s.new_below || now_ms - s.flash_ms < FLASH_MS) {
        return;
    }
    s.flash_ms = now_ms;
    s.arrow_on = !s.arrow_on;
    draw_arrow(s.arrow_on);
}

static void type_text(const char *text)
{
    size_t n = strlen(text);
    if (s.input_len + n > HH_TEXT_MAX) {
        return;
    }
    memcpy(s.input + s.input_len, text, n);
    s.input_len += n;
    s.input[s.input_len] = '\0';
}

static void press_key(const key_t *k)
{
    switch (k->action) {
    case K_TEXT:
        type_text(k->text);
        if (s.page == PAGE_LETTERS && s.shift == 1) {
            s.shift = 0;   /* one capital, then back to lower case */
            build_keys();
            draw_keyboard();
        }
        draw_field();
        break;
    case K_BACKSPACE:
        while (s.input_len > 0) {
            char c = s.input[--s.input_len];
            if ((c & 0xC0) != 0x80) {
                break;   /* removed the start of a character */
            }
        }
        s.input[s.input_len] = '\0';
        draw_field();
        break;
    case K_SHIFT:
        s.shift = (uint8_t)((s.shift + 1u) % 3u);   /* off, one capital, caps lock */
        build_keys();
        draw_keyboard();
        break;
    case K_PAGE_LETTERS:
    case K_PAGE_NUMBERS:
    case K_PAGE_EMOJI:
        s.page = k->action == K_PAGE_LETTERS ? PAGE_LETTERS : k->action == K_PAGE_NUMBERS ? PAGE_NUMBERS : PAGE_EMOJI;
        build_keys();
        draw_keyboard();
        break;
    case K_EMOJI_PREV:
    case K_EMOJI_NEXT: {
        uint8_t pages = (uint8_t)((LG_EMOJI_COUNT + EMOJI_PER_PAGE - 1u) / EMOJI_PER_PAGE);
        s.emoji_page = (uint8_t)((s.emoji_page + (k->action == K_EMOJI_NEXT ? 1u : pages - 1u)) % pages);
        build_keys();
        draw_keyboard();
        break;
    }
    case K_HIDE:
        place(false);
        draw_all();
        break;
    default:
        break;
    }
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
            scroll_to((int16_t)(s.down_scroll - dy));
        }
        return false;
    }
    if (!s.was_down) {
        return false;
    }
    s.was_down = false;
    if (s.pressed_key >= 0) {
        int idx = s.pressed_key;
        s.pressed_key = -1;
        key_t k = s.keys[idx];
        draw_key(&s.keys[idx]);
        if (lg_rect_hit(&k.rect, x, y)) {
            press_key(&k);
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
    if (s.new_below && lg_rect_hit(&s.arrow, x, y)) {
        scroll_to(max_scroll());   /* tapping the arrow goes to the new message */
    } else if (lg_rect_hit(&s.field, x, y) && !s.keyboard) {
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
    scroll_to((int16_t)(s.scroll + dy));
}

void spike_chat_keyboard(bool show)
{
    place(show);
    draw_all();
}

void spike_chat_type(const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (s.keys[i].action == K_TEXT && s.keys[i].text[0] == *p && s.keys[i].text[1] == '\0') {
                s.pressed_key = i;
                draw_key(&s.keys[i]);
                s.pressed_key = -1;
                key_t k = s.keys[i];
                draw_key(&s.keys[i]);   /* press and release, as a finger would */
                press_key(&k);
                break;
            }
        }
    }
}

void spike_chat_page(int page)
{
    key_t k = { .action = page == 1 ? K_PAGE_NUMBERS : page == 2 ? K_PAGE_EMOJI : page == 3 ? K_SHIFT
                                                                                          : K_PAGE_LETTERS };
    press_key(&k);
}

void spike_chat_log(void)
{
    ESP_LOGI(TAG, "[UI] Spike chat: %u bubbles, content %d px, full list paints %" PRIu32 " (avg %" PRIu64
             " ms), scroll steps %" PRIu32 " (avg %" PRIu64 " ms), key paints %" PRIu32 " (avg %" PRIu64 " ms)",
             s.n_bubbles, s.content_h, s.list_paints, s.list_paints ? s.list_us / s.list_paints / 1000u : 0u,
             s.scroll_steps, s.scroll_steps ? s.scroll_us / s.scroll_steps / 1000u : 0u, s.key_paints,
             s.key_paints ? s.key_us / s.key_paints / 1000u : 0u);
}
