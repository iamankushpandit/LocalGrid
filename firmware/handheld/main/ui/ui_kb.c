#include "ui_kb.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "ui_theme.h"

#define KEY_ROW_H      32
#define KEY_ROWS       4
#define KEYS_MAX       40
#define EMOJI_PER_PAGE 24u

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
    uint16_t w;
    uint16_t h;
    char    *buf;
    size_t   cap;
    page_t   page;
    uint8_t  shift;        /* 0 off, 1 next letter only, 2 caps lock */
    uint8_t  emoji_page;
    key_t    keys[KEYS_MAX];
    uint8_t  n_keys;
    int      pressed;      /* -1 none */
    bool     was_down;
    uint32_t paints;
    uint64_t us;
} s = { .pressed = -1 };

int16_t ui_kb_height(void)
{
    return (int16_t)(KEY_ROWS * KEY_ROW_H);
}

static lg_rect_t area(void)
{
    return (lg_rect_t){ 0, (int16_t)(s.h - ui_kb_height()), (int16_t)s.w, ui_kb_height() };
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

static void add_chars(const char *chars, int16_t x, int16_t y, int16_t kw, bool upper)
{
    for (const char *p = chars; *p != '\0'; p++) {
        char one[2] = { upper && *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p, '\0' };
        add_key(K_TEXT, one, one, (int16_t)(x + (p - chars) * kw), y, kw);
    }
}

static void build(void)
{
    s.n_keys = 0;
    int16_t kw = (int16_t)(s.w / 10);
    int16_t x0 = (int16_t)((s.w - 10 * kw) / 2);
    int16_t y0 = area().y;
    int16_t r3 = (int16_t)(y0 + 3 * KEY_ROW_H);
    int16_t wide = (int16_t)(kw * 3 / 2);
    switch (s.page) {
    case PAGE_LETTERS: {
        bool upper = s.shift > 0;
        add_chars("qwertyuiop", x0, y0, kw, upper);
        add_chars("asdfghjkl", (int16_t)(x0 + kw / 2), (int16_t)(y0 + KEY_ROW_H), kw, upper);
        add_key(K_SHIFT, LG_SYMBOL_UP, NULL, x0, (int16_t)(y0 + 2 * KEY_ROW_H), wide);
        add_chars("zxcvbnm", (int16_t)(x0 + wide), (int16_t)(y0 + 2 * KEY_ROW_H), kw, upper);
        add_key(K_BACKSPACE, LG_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 7 * kw), (int16_t)(y0 + 2 * KEY_ROW_H),
                wide);
        add_key(K_PAGE_NUMBERS, "123", NULL, x0, r3, wide);
        break;
    }
    case PAGE_NUMBERS:
        add_chars("1234567890", x0, y0, kw, false);
        add_chars("-/:;()$&@", (int16_t)(x0 + kw / 2), (int16_t)(y0 + KEY_ROW_H), kw, false);
        add_key(K_TEXT, "#", "#", x0, (int16_t)(y0 + 2 * KEY_ROW_H), wide);
        add_chars("_.,?!'\"", (int16_t)(x0 + wide), (int16_t)(y0 + 2 * KEY_ROW_H), kw, false);
        add_key(K_BACKSPACE, LG_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 7 * kw), (int16_t)(y0 + 2 * KEY_ROW_H),
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
        add_key(K_EMOJI_PREV, LG_SYMBOL_LEFT, NULL, (int16_t)(x0 + wide), r3, kw);
        add_key(K_EMOJI_NEXT, LG_SYMBOL_RIGHT, NULL, (int16_t)(x0 + wide + kw), r3, kw);
        add_key(K_TEXT, "space", " ", (int16_t)(x0 + wide + 2 * kw), r3, (int16_t)(4 * kw));
        add_key(K_BACKSPACE, LG_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 6 * kw), r3, kw);
        add_key(K_HIDE, LG_SYMBOL_DOWN, NULL, (int16_t)(x0 + wide + 7 * kw), r3, wide);
        return;
    }
    }
    add_key(K_PAGE_EMOJI, LG_EMOJI[0], NULL, (int16_t)(x0 + wide), r3, kw);
    add_key(K_TEXT, ",", ",", (int16_t)(x0 + wide + kw), r3, kw);
    add_key(K_TEXT, "space", " ", (int16_t)(x0 + wide + 2 * kw), r3, (int16_t)(4 * kw));
    add_key(K_TEXT, ".", ".", (int16_t)(x0 + wide + 6 * kw), r3, kw);
    add_key(K_HIDE, LG_SYMBOL_DOWN, NULL, (int16_t)(x0 + wide + 7 * kw), r3, wide);
}

static void paint_key(const lg_canvas_t *c, void *ctx)
{
    const key_t *k = ctx;
    bool pressed = s.pressed >= 0 && &s.keys[s.pressed] == k;
    bool locked = k->action == K_SHIFT && s.shift == 2;
    lg_paint_panel(c, &k->rect, &k->rect, pressed || locked ? C_OUTLINE : C_SURFACE, C_BG,
                   locked ? C_ACCENT : C_OUTLINE, 1, 4);
    const lg_font_t *f = k->action == K_TEXT ? F_BODY : F_ICON;
    const lg_font_t *fb = s.page == PAGE_EMOJI ? F_EMOJI_KEY : F_EMOJI;
    lg_color_t fg = (k->action == K_SHIFT && s.shift > 0) ? C_ACCENT : C_TEXT;
    int16_t tw = lg_draw_text_width(f, fb, k->label);
    lg_paint_text(c, &k->rect, (int16_t)(k->rect.x + (k->rect.w - tw) / 2),
                  (int16_t)(k->rect.y + (k->rect.h - f->line_height) / 2), f, fb, fg, k->label, strlen(k->label));
}

static void draw_key(const key_t *k)
{
    int64_t t0 = esp_timer_get_time();
    lg_draw_region(&k->rect, paint_key, (void *)k);
    s.paints++;
    s.us += (uint64_t)(esp_timer_get_time() - t0);
}

static void paint_all(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t a = area();
    lg_paint_panel(c, &a, &a, C_BG, C_BG, C_BG, 0, 0);
    for (uint8_t i = 0; i < s.n_keys; i++) {
        const key_t *k = &s.keys[i];
        if (k->rect.y < c->band.y + c->band.h && k->rect.y + k->rect.h > c->band.y) {
            paint_key(c, (void *)k);
        }
    }
}

void ui_kb_open(uint16_t screen_w, uint16_t screen_h, char *buf, size_t cap)
{
    s.w = screen_w;
    s.h = screen_h;
    s.buf = buf;
    s.cap = cap;
    s.pressed = -1;
    s.was_down = false;
    build();
}

void ui_kb_draw(void)
{
    lg_rect_t a = area();
    lg_draw_region(&a, paint_all, NULL);
}

static kb_event_t press(const key_t *k)
{
    size_t len = s.buf != NULL ? strlen(s.buf) : 0;
    switch (k->action) {
    case K_TEXT: {
        size_t n = strlen(k->text);
        if (s.buf != NULL && len + n + 1 <= s.cap) {
            memcpy(s.buf + len, k->text, n + 1);
        }
        if (s.page == PAGE_LETTERS && s.shift == 1) {
            s.shift = 0;   /* one capital, then back to lower case */
            build();
            ui_kb_draw();
        }
        return KB_TEXT_CHANGED;
    }
    case K_BACKSPACE:
        while (len > 0) {
            char ch = s.buf[--len];
            if ((ch & 0xC0) != 0x80) {
                break;   /* removed the start of a character */
            }
        }
        if (s.buf != NULL) {
            s.buf[len] = '\0';
        }
        return KB_TEXT_CHANGED;
    case K_SHIFT:
        s.shift = (uint8_t)((s.shift + 1u) % 3u);   /* off, one capital, caps lock */
        break;
    case K_PAGE_LETTERS:
        s.page = PAGE_LETTERS;
        break;
    case K_PAGE_NUMBERS:
        s.page = PAGE_NUMBERS;
        break;
    case K_PAGE_EMOJI:
        s.page = PAGE_EMOJI;
        break;
    case K_EMOJI_PREV:
    case K_EMOJI_NEXT: {
        uint8_t pages = (uint8_t)((LG_EMOJI_COUNT + EMOJI_PER_PAGE - 1u) / EMOJI_PER_PAGE);
        s.emoji_page = (uint8_t)((s.emoji_page + (k->action == K_EMOJI_NEXT ? 1u : pages - 1u)) % pages);
        break;
    }
    case K_HIDE:
        return KB_HIDE;
    default:
        return KB_NOTHING;
    }
    build();
    ui_kb_draw();
    return KB_NOTHING;
}

bool ui_kb_touch(int16_t x, int16_t y, bool down, kb_event_t *event)
{
    *event = KB_NOTHING;
    if (down && !s.was_down) {
        s.was_down = true;
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (lg_rect_hit(&s.keys[i].rect, x, y)) {
                s.pressed = i;
                draw_key(&s.keys[i]);   /* the pressed look: one key repainted */
                return true;
            }
        }
        return false;
    }
    if (down) {
        return s.pressed >= 0;
    }
    if (!s.was_down) {
        return false;
    }
    s.was_down = false;
    if (s.pressed < 0) {
        return false;
    }
    int idx = s.pressed;
    s.pressed = -1;
    key_t k = s.keys[idx];
    draw_key(&s.keys[idx]);
    if (lg_rect_hit(&k.rect, x, y)) {
        *event = press(&k);
    }
    return true;
}

kb_event_t ui_kb_type(const char *text)
{
    kb_event_t last = KB_NOTHING;
    for (const char *p = text; *p != '\0'; p++) {
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (s.keys[i].action == K_TEXT && s.keys[i].text[0] == *p && s.keys[i].text[1] == '\0') {
                s.pressed = i;
                draw_key(&s.keys[i]);
                s.pressed = -1;
                key_t k = s.keys[i];
                draw_key(&s.keys[i]);   /* press and release, as a finger would */
                if (press(&k) == KB_TEXT_CHANGED) {
                    last = KB_TEXT_CHANGED;
                }
                break;
            }
        }
    }
    return last;
}

void ui_kb_page(int page)
{
    key_t k = { .action = page == 1 ? K_PAGE_NUMBERS : page == 2 ? K_PAGE_EMOJI : page == 3 ? K_SHIFT : K_PAGE_LETTERS };
    (void)press(&k);
}

void ui_kb_stats(uint32_t *paints, uint64_t *us)
{
    *paints = s.paints;
    *us = s.us;
}
