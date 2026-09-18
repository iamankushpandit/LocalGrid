#include "ui_kb.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lg_bsp_settings.h"
#include "ui_theme.h"

#define KEY_ROW_H      32
#define KEY_ROWS       4
#define KEYS_MAX       40
#define EMOJI_PER_PAGE 24u
#define T9_PAUSE_MS    900u   /* long enough to find the next key, short enough not to wait on it */
#define T9_NONE        0xFFu

/*
 * The phone keypad, and why it is the default (owner, 2026-09-17). A qwerty row is ten keys
 * across a 240 px panel: 24 px a key, narrower than a fingertip, and typing "ankush" on the
 * Freenove came out "abjudh" -- every miss one key to the side. The keypad is four columns, 60 px
 * a key, and letters come from tapping a key until the one you want shows, as SMS phones did.
 * Qwerty is still there, one key away, and whichever was used last is remembered (D59).
 */
#define T9_KEYS 10
static const char *const T9_LABEL[T9_KEYS] = {
    "1 .,?", "2 abc", "3 def", "4 ghi", "5 jkl", "6 mno", "7 pqrs", "8 tuv", "9 wxyz", "0 space",
};
static const char *const T9_CYCLE[T9_KEYS] = {
    ".,?1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", " 0",
};

typedef enum {
    K_TEXT,
    K_BACKSPACE,
    K_HIDE,
    K_SHIFT,
    K_T9,              /* a keypad key: text holds the characters it steps through */
    K_PAGE_BACK,       /* back to the letters page in use, without changing which that is */
    K_PAGE_KEYPAD,
    K_PAGE_LETTERS,
    K_PAGE_NUMBERS,
    K_PAGE_EMOJI,
    K_EMOJI_PREV,
    K_EMOJI_NEXT,
} key_action_t;

typedef struct {
    lg_rect_t rect;
    char      label[8];
    char      text[8];    /* UTF-8 inserted by K_TEXT, or the cycle of a K_T9 key */
    uint8_t   action;
} key_t;

typedef enum { PAGE_KEYPAD, PAGE_LETTERS, PAGE_NUMBERS, PAGE_EMOJI } page_t;

#define KB_SETTING "kbqwerty"   /* true: the full keyboard, false: the phone keypad */

static struct {
    uint16_t w;
    uint16_t h;
    char    *buf;
    size_t   cap;
    page_t   page;
    uint8_t  shift;        /* 0 off, 1 next letter only, 2 caps lock */
    uint8_t  emoji_page;
    uint8_t  t9_key;       /* the keypad key a run of taps is on, T9_NONE between runs */
    uint8_t  t9_tap;       /* how far through that key's characters the run has stepped */
    bool     t9_upper;     /* the run started with shift on, so it stays upper case */
    uint32_t t9_ms;        /* when the last tap landed */
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
    case PAGE_KEYPAD: {
        /* Four columns: the digits people already know, and the controls down the right. */
        int16_t cw = (int16_t)(s.w / 4);
        for (uint8_t i = 0; i < T9_KEYS - 1u; i++) {
            add_key(K_T9, T9_LABEL[i], T9_CYCLE[i], (int16_t)((i % 3) * cw), (int16_t)(y0 + (i / 3) * KEY_ROW_H), cw);
        }
        add_key(K_BACKSPACE, LG_SYMBOL_BACKSPACE, NULL, (int16_t)(3 * cw), y0, cw);
        add_key(K_SHIFT, LG_SYMBOL_UP, NULL, (int16_t)(3 * cw), (int16_t)(y0 + KEY_ROW_H), cw);
        add_key(K_PAGE_EMOJI, LG_EMOJI[0], NULL, (int16_t)(3 * cw), (int16_t)(y0 + 2 * KEY_ROW_H), cw);
        add_key(K_PAGE_NUMBERS, "123", NULL, 0, r3, cw);
        add_key(K_T9, T9_LABEL[T9_KEYS - 1u], T9_CYCLE[T9_KEYS - 1u], cw, r3, cw);
        add_key(K_PAGE_LETTERS, "abc", NULL, (int16_t)(2 * cw), r3, cw);
        add_key(K_HIDE, LG_SYMBOL_DOWN, NULL, (int16_t)(3 * cw), r3, cw);
        return;
    }
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
        add_key(K_PAGE_BACK, "abc", NULL, x0, r3, wide);
        break;
    case PAGE_EMOJI: {
        uint8_t first = (uint8_t)(s.emoji_page * EMOJI_PER_PAGE);
        int16_t ew = (int16_t)(s.w / 8);
        for (uint8_t i = 0; i < EMOJI_PER_PAGE && first + i < LG_EMOJI_COUNT; i++) {
            add_key(K_TEXT, LG_EMOJI[first + i], LG_EMOJI[first + i], (int16_t)(i % 8 * ew),
                    (int16_t)(y0 + (i / 8) * KEY_ROW_H), ew);
        }
        add_key(K_PAGE_BACK, "abc", NULL, x0, r3, wide);
        add_key(K_EMOJI_PREV, LG_SYMBOL_LEFT, NULL, (int16_t)(x0 + wide), r3, kw);
        add_key(K_EMOJI_NEXT, LG_SYMBOL_RIGHT, NULL, (int16_t)(x0 + wide + kw), r3, kw);
        add_key(K_TEXT, "space", " ", (int16_t)(x0 + wide + 2 * kw), r3, (int16_t)(4 * kw));
        add_key(K_BACKSPACE, LG_SYMBOL_BACKSPACE, NULL, (int16_t)(x0 + wide + 6 * kw), r3, kw);
        add_key(K_HIDE, LG_SYMBOL_DOWN, NULL, (int16_t)(x0 + wide + 7 * kw), r3, wide);
        return;
    }
    }
    add_key(K_PAGE_EMOJI, LG_EMOJI[0], NULL, (int16_t)(x0 + wide), r3, kw);
    add_key(K_PAGE_KEYPAD, "123", NULL, (int16_t)(x0 + wide + kw), r3, kw);   /* back to the keypad */
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
    const lg_font_t *f = k->action == K_TEXT ? F_BODY : k->action == K_T9 ? F_SMALL : F_ICON;
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
    s.t9_key = T9_NONE;
    s.page = lg_bsp_setting_get_bool(KB_SETTING, false) ? PAGE_LETTERS : PAGE_KEYPAD;
    build();
}

void ui_kb_draw(void)
{
    lg_rect_t a = area();
    lg_draw_region(&a, paint_all, NULL);
}

/* Removes the last character of the buffer, whatever its length in bytes. */
static size_t drop_last(size_t len)
{
    while (len > 0) {
        char ch = s.buf[--len];
        if ((ch & 0xC0) != 0x80) {
            break;
        }
    }
    if (s.buf != NULL) {
        s.buf[len] = '\0';
    }
    return len;
}

static kb_event_t press(const key_t *k)
{
    size_t len = s.buf != NULL ? strlen(s.buf) : 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool same_run = k->action == K_T9 && s.t9_key != T9_NONE && s.t9_key < s.n_keys &&
                    strcmp(s.keys[s.t9_key].text, k->text) == 0 && now - s.t9_ms < T9_PAUSE_MS;
    if (k->action != K_T9) {
        s.t9_key = T9_NONE;   /* any other key ends the run */
    }
    switch (k->action) {
    case K_T9: {
        /* The same key again steps the letter on; a different key, or a pause, starts a new one. */
        const char *cycle = k->text;
        size_t n = strlen(cycle);
        if (same_run) {
            len = drop_last(len);
            s.t9_tap = (uint8_t)((s.t9_tap + 1u) % n);
        } else {
            s.t9_tap = 0;
            s.t9_upper = s.shift > 0;
        }
        for (uint8_t i = 0; i < s.n_keys; i++) {
            if (s.keys[i].action == K_T9 && strcmp(s.keys[i].text, cycle) == 0) {
                s.t9_key = i;
                break;
            }
        }
        s.t9_ms = now;
        char one = cycle[s.t9_tap];
        if (s.t9_upper && one >= 'a' && one <= 'z') {
            one = (char)(one - 'a' + 'A');
        }
        if (s.buf != NULL && len + 2 <= s.cap) {
            s.buf[len] = one;
            s.buf[len + 1] = '\0';
        }
        if (s.shift == 1) {
            s.shift = 0;   /* one capital, then back to lower case */
            build();
            ui_kb_draw();
        }
        return KB_TEXT_CHANGED;
    }
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
        (void)drop_last(len);
        return KB_TEXT_CHANGED;
    case K_SHIFT:
        s.shift = (uint8_t)((s.shift + 1u) % 3u);   /* off, one capital, caps lock */
        break;
    case K_PAGE_BACK:
        s.page = lg_bsp_setting_get_bool(KB_SETTING, false) ? PAGE_LETTERS : PAGE_KEYPAD;
        break;
    case K_PAGE_KEYPAD:
        s.page = PAGE_KEYPAD;
        lg_bsp_setting_set_bool(KB_SETTING, false);   /* whichever was used last comes back (D59) */
        break;
    case K_PAGE_LETTERS:
        s.page = PAGE_LETTERS;
        lg_bsp_setting_set_bool(KB_SETTING, true);
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
    /* The key that was pressed, not the one under the lift: a fingertip rolls a few pixels as it
     * leaves the glass, and on a 24 px key that landed on the neighbour (owner: "ankush" came out
     * "abjudh"). x and y are the lift point and are deliberately unused. */
    (void)x;
    (void)y;
    *event = press(&k);
    return true;
}

kb_event_t ui_kb_type(const char *text)
{
    kb_event_t last = KB_NOTHING;
    for (const char *p = text; *p != '\0'; p++) {
        /* On the keypad a letter is a run of taps on its key, which is what a finger does. */
        if (s.page == PAGE_KEYPAD) {
            for (uint8_t i = 0; i < s.n_keys && s.keys[i].action == K_T9; i++) {
                const char *at = strchr(s.keys[i].text, *p);
                if (at == NULL) {
                    continue;
                }
                key_t k = s.keys[i];
                for (size_t tap = 0; tap <= (size_t)(at - s.keys[i].text); tap++) {
                    if (press(&k) == KB_TEXT_CHANGED) {
                        last = KB_TEXT_CHANGED;
                    }
                }
                s.t9_key = T9_NONE;   /* the next letter starts its own run, however quick this was */
                break;
            }
            continue;
        }
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
    key_t k = { .action = page == 1   ? K_PAGE_NUMBERS
                        : page == 2   ? K_PAGE_EMOJI
                        : page == 3   ? K_SHIFT
                        : page == 4   ? K_PAGE_KEYPAD
                                      : K_PAGE_LETTERS };
    (void)press(&k);
}

void ui_kb_stats(uint32_t *paints, uint64_t *us)
{
    *paints = s.paints;
    *us = s.us;
}
