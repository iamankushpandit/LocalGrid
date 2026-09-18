#include "ui_list.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "ui_lock.h"
#include "ui_theme.h"

#define TAB_H        30
#define ROW_H        30
#define CHOICE_H     50
#define BUTTON_H     32
#define CHECK_H      28
#define FIELD_H      32
#define SECTION_H    20
#define NOTE_LINES   6
#define DRAG_START   6

static struct {
    uint16_t          w;
    uint16_t          h;
    char              title[SLIST_LABEL_MAX];
    bool              back;
    bool              plus;
    const char *const *tabs;
    uint8_t           n_tabs;
    uint8_t           tab;
    slist_row_t       rows[SLIST_ROWS];
    uint8_t           n;
    lg_rect_t         head_icon;
    lg_rect_t         plus_icon;
    int16_t           title_left;     /* the title starts here, after the house (D62) */
    int16_t           title_right;    /* the title ends here, before the battery and padlock */
    lg_rect_t         list;
    int16_t           bottom;
    int16_t           content_h;
    int16_t           scroll;
    int               pressed;        /* row index, -1 none */
    int               pressed_seg;
    bool              was_down;
    int16_t           down_y;
    int16_t           down_scroll;
    bool              dragging;
    uint32_t          down_ms;
    bool              holding;        /* a talk row held past SLIST_HOLD_MS: talking */
    bool              hold_refused;   /* ...but the talk did not start; the lift is ignored */
} s = { .pressed = -1 };

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void slist_begin(const char *title, bool back, bool plus)
{
    snprintf(s.title, sizeof(s.title), "%s", title);
    s.back = back;
    s.plus = plus;
    s.tabs = NULL;
    s.n_tabs = 0;
    s.n = 0;
    s.pressed = -1;
}

void slist_tabs(const char *const *names, uint8_t n, uint8_t selected)
{
    s.tabs = names;
    s.n_tabs = n;
    s.tab = selected;
}

slist_row_t *slist_add(slist_kind_t kind, const char *label, const char *value, int16_t id, int32_t arg)
{
    if (s.n >= SLIST_ROWS) {
        return &s.rows[SLIST_ROWS - 1u];   /* a full list overwrites its last row rather than failing */
    }
    slist_row_t *r = &s.rows[s.n++];
    memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)kind;
    r->id = id;
    r->arg = arg;
    snprintf(r->label, sizeof(r->label), "%s", label != NULL ? label : "");
    snprintf(r->value, sizeof(r->value), "%s", value != NULL ? value : "");
    return r;
}

slist_row_t *slist_row(uint8_t index)
{
    return index < s.n ? &s.rows[index] : NULL;
}

uint8_t slist_count(void)
{
    return s.n;
}

static int16_t content_w(void)
{
    return (int16_t)(s.w - 2 * UI_PAD);
}

static void layout(void)
{
    int16_t y = UI_GAP;
    for (uint8_t i = 0; i < s.n; i++) {
        slist_row_t *r = &s.rows[i];
        switch (r->kind) {
        case ROW_SECTION: r->h = SECTION_H; break;
        case ROW_CHOICE:  r->h = CHOICE_H; break;
        case ROW_BUTTON:  r->h = BUTTON_H; break;
        case ROW_CHECK:   r->h = CHECK_H; break;
        case ROW_FIELD:   r->h = FIELD_H; break;
        case ROW_NOTE: {
            uint16_t starts[NOTE_LINES + 1];
            uint8_t lines = lg_text_wrap(F_SMALL, F_EMOJI, r->value, content_w(), starts, NOTE_LINES);
            r->h = (int16_t)((lines ? lines : 1) * F_SMALL->line_height + 2);
            break;
        }
        default:
            r->h = ROW_H;
            break;
        }
        r->y = y;
        y = (int16_t)(y + r->h + UI_GAP);
    }
    s.content_h = y;
}

static int16_t max_scroll(void)
{
    return s.content_h > s.list.h ? (int16_t)(s.content_h - s.list.h) : 0;
}

static lg_rect_t row_rect(const slist_row_t *r)
{
    return (lg_rect_t){ UI_PAD, (int16_t)(s.list.y + r->y - s.scroll), content_w(), r->h };
}

static lg_rect_t segment_rect(const slist_row_t *r, uint8_t seg)
{
    lg_rect_t row = row_rect(r);
    int16_t top = (int16_t)(row.y + F_SMALL->line_height + 2);
    int16_t sw = (int16_t)(row.w / (r->n_options ? r->n_options : 1));
    return (lg_rect_t){ (int16_t)(row.x + seg * sw), top, (int16_t)(sw - 3), (int16_t)(row.y + row.h - top) };
}

static void text_in(const lg_canvas_t *c, const lg_rect_t *box, const lg_font_t *f, lg_color_t fg, const char *text,
                    lg_align_t align, int16_t pad)
{
    int16_t tw = lg_draw_text_width(f, F_EMOJI, text);
    int16_t x = align == LG_ALIGN_CENTER ? (int16_t)(box->x + (box->w - tw) / 2)
              : align == LG_ALIGN_RIGHT  ? (int16_t)(box->x + box->w - pad - tw)
                                         : (int16_t)(box->x + pad);
    lg_paint_text(c, box, x, (int16_t)(box->y + (box->h - f->line_height) / 2), f, F_EMOJI, fg, text, strlen(text));
}

static void paint_row(const lg_canvas_t *c, uint8_t i)
{
    const slist_row_t *r = &s.rows[i];
    lg_rect_t box = row_rect(r);
    bool pressed = s.pressed == (int)i;
    lg_paint_panel(c, &s.list, &(lg_rect_t){ 0, box.y, (int16_t)s.w, box.h }, C_BG, C_BG, C_BG, 0, 0);
    switch (r->kind) {
    case ROW_SECTION:
        text_in(c, &box, F_SMALL, C_ACCENT, r->label, LG_ALIGN_LEFT, 2);
        break;
    case ROW_FACT:
        text_in(c, &box, F_SMALL, C_MUTED, r->label, LG_ALIGN_LEFT, 2);
        text_in(c, &box, F_SMALL, r->warn ? C_WARNING : C_TEXT, r->value, LG_ALIGN_RIGHT, 2);
        break;
    case ROW_ACTION: {
        if (pressed && s.holding) {
            /* Talking to this row (D61): red, and says how to stop. */
            lg_paint_panel(c, &s.list, &box, C_ERROR, C_BG, C_ERROR, 1, 6);
            text_in(c, &box, F_BODY, C_ACCENT_INK, r->label, LG_ALIGN_LEFT, 8);
            text_in(c, &box, F_SMALL, C_ACCENT_INK, "Talking... let go", LG_ALIGN_RIGHT, 8);
            break;
        }
        lg_paint_panel(c, &s.list, &box, pressed ? C_OUTLINE : C_SURFACE, C_BG, C_OUTLINE, 1, 6);
        lg_rect_t inner = { box.x, box.y, (int16_t)(box.w - 18), box.h };
        text_in(c, &inner, F_BODY, r->muted ? C_MUTED : C_TEXT, r->label, LG_ALIGN_LEFT, 8);
        text_in(c, &inner, F_SMALL, r->warn ? C_WARNING : C_MUTED, r->value, LG_ALIGN_RIGHT, 2);
        if (!r->muted) {
            text_in(c, &box, F_SMALL, C_MUTED, LG_SYMBOL_RIGHT, LG_ALIGN_RIGHT, 6);
        }
        break;
    }
    case ROW_CHOICE: {
        lg_rect_t label = { box.x, box.y, box.w, F_SMALL->line_height };
        text_in(c, &label, F_SMALL, C_MUTED, r->label, LG_ALIGN_LEFT, 2);
        for (uint8_t k = 0; k < r->n_options; k++) {
            lg_rect_t seg = segment_rect(r, k);
            bool on = k == r->selected;
            bool held = pressed && s.pressed_seg == k;
            lg_paint_panel(c, &s.list, &seg, on ? C_ACCENT : (held ? C_OUTLINE : C_SURFACE), C_BG,
                           on ? C_ACCENT : C_OUTLINE, 1, 5);
            text_in(c, &seg, F_SMALL, on ? C_ACCENT_INK : C_TEXT, r->options[k], LG_ALIGN_CENTER, 0);
        }
        break;
    }
    case ROW_BUTTON: {
        lg_color_t bg = r->style == BTN_MAIN ? C_ACCENT : C_SURFACE;
        lg_color_t edge = r->style == BTN_DANGER ? C_ERROR : C_ACCENT;
        lg_color_t fg = r->style == BTN_MAIN ? C_ACCENT_INK : edge;
        if (pressed) {
            bg = r->style == BTN_MAIN ? C_MUTED : C_OUTLINE;
        }
        lg_paint_panel(c, &s.list, &box, bg, C_BG, edge, 1, 6);
        text_in(c, &box, F_BODY, fg, r->label, LG_ALIGN_CENTER, 0);
        break;
    }
    case ROW_NOTE: {
        uint16_t starts[NOTE_LINES + 1];
        uint8_t lines = lg_text_wrap(F_SMALL, F_EMOJI, r->value, content_w(), starts, NOTE_LINES);
        for (uint8_t l = 0; l < lines; l++) {
            lg_paint_text(c, &box, box.x, (int16_t)(box.y + l * F_SMALL->line_height), F_SMALL, F_EMOJI,
                          r->warn ? C_WARNING : C_MUTED, r->value + starts[l], (size_t)(starts[l + 1] - starts[l]));
        }
        break;
    }
    case ROW_CHECK: {
        lg_rect_t tick = { box.x, (int16_t)(box.y + (box.h - 18) / 2), 18, 18 };
        lg_paint_panel(c, &s.list, &tick, r->checked ? C_ACCENT : (pressed ? C_OUTLINE : C_SURFACE), C_BG,
                       r->muted ? C_OUTLINE : C_ACCENT, 1, 3);
        if (r->checked) {
            text_in(c, &tick, F_SMALL, C_ACCENT_INK, LG_SYMBOL_OK, LG_ALIGN_CENTER, 0);
        }
        lg_rect_t label = { (int16_t)(box.x + 26), box.y, (int16_t)(box.w - 26), box.h };
        text_in(c, &label, F_BODY, r->muted ? C_MUTED : C_TEXT, r->label, LG_ALIGN_LEFT, 0);
        break;
    }
    case ROW_FIELD: {
        lg_paint_panel(c, &s.list, &box, C_SURFACE, C_BG, r->focused ? C_ACCENT : C_OUTLINE, 1, 4);
        const char *shown = r->value;
        int16_t room = (int16_t)(box.w - 20);
        while (*shown != '\0' && lg_draw_text_width(F_BODY, F_EMOJI, shown) > room) {
            shown++;
            while ((*shown & 0xC0) == 0x80) {
                shown++;   /* never start inside a UTF-8 sequence */
            }
        }
        if (r->value[0] == '\0') {
            text_in(c, &box, F_BODY, C_MUTED, r->label, LG_ALIGN_LEFT, 8);
        } else {
            text_in(c, &box, F_BODY, C_TEXT, shown, LG_ALIGN_LEFT, 8);
        }
        if (r->focused) {
            int16_t cx = (int16_t)(box.x + 8 + (r->value[0] ? lg_draw_text_width(F_BODY, F_EMOJI, shown) : 0));
            lg_rect_t bar = { cx, (int16_t)(box.y + 7), 1, (int16_t)(box.h - 14) };
            lg_paint_panel(c, &s.list, &bar, C_ACCENT, C_ACCENT, C_ACCENT, 0, 0);
        }
        break;
    }
    default:
        break;
    }
}

static void paint_list(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_paint_panel(c, &s.list, &s.list, C_BG, C_BG, C_BG, 0, 0);
    for (uint8_t i = 0; i < s.n; i++) {
        lg_rect_t box = row_rect(&s.rows[i]);
        if (box.y < c->band.y + c->band.h && box.y + box.h > c->band.y) {
            paint_row(c, i);
        }
    }
}

typedef struct {
    uint8_t index;
} one_row_t;

static void paint_one(const lg_canvas_t *c, void *ctx)
{
    paint_row(c, ((one_row_t *)ctx)->index);
}

static void paint_header(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    lg_rect_t head = { 0, 0, (int16_t)s.w, (int16_t)(UI_HEAD_H + (s.n_tabs ? TAB_H : 0)) };
    lg_paint_panel(c, &head, &head, C_BG, C_BG, C_BG, 0, 0);
    lg_rect_t title = { s.title_left, 0, (int16_t)(s.title_right - s.title_left), UI_HEAD_H };
    text_in(c, &title, F_BODY, C_ACCENT, s.title, LG_ALIGN_LEFT, 2);
    ui_bar_paint(c);   /* the house at the left, the battery and padlock at the right */
    if (s.back) {
        text_in(c, &s.head_icon, F_ICON, C_ACCENT, LG_SYMBOL_LEFT, LG_ALIGN_CENTER, 0);
    }
    if (s.plus) {
        text_in(c, &s.plus_icon, F_ICON, C_ACCENT, LG_SYMBOL_PLUS, LG_ALIGN_CENTER, 0);
    }
    if (s.n_tabs) {
        int16_t tw = (int16_t)(s.w / s.n_tabs);
        for (uint8_t k = 0; k < s.n_tabs; k++) {
            lg_rect_t tab = { (int16_t)(k * tw), UI_HEAD_H, tw, TAB_H };
            bool on = k == s.tab;
            text_in(c, &tab, F_SMALL, on ? C_ACCENT : C_MUTED, s.tabs[k], LG_ALIGN_CENTER, 0);
            lg_rect_t line = { (int16_t)(tab.x + 4), (int16_t)(tab.y + tab.h - (on ? 3 : 1)), (int16_t)(tw - 8),
                               on ? 3 : 1 };
            lg_paint_panel(c, &head, &line, on ? C_ACCENT : C_OUTLINE, C_BG, C_BG, 0, 0);
        }
    }
}

void slist_show(uint16_t w, uint16_t h, int16_t bottom, bool keep_scroll)
{
    s.w = w;
    s.h = h;
    s.bottom = bottom;
    int16_t top = (int16_t)(UI_HEAD_H + (s.n_tabs ? TAB_H : 0));
    /* Home is the house at the left on every list (ui_bar_home); the right-hand icon is only a
     * back arrow, on screens that have a parent other than home. */
    int16_t edge = (int16_t)(s.back ? w - 40 : w - UI_PAD);
    s.head_icon = s.back ? (lg_rect_t){ edge, 0, 40, UI_HEAD_H } : (lg_rect_t){ 0, 0, 0, 0 };
    s.plus_icon = (lg_rect_t){ (int16_t)(edge - 40), 0, 40, UI_HEAD_H };
    s.title_right = ui_bar_place(s.plus ? s.plus_icon.x : edge, 0, UI_HEAD_H);
    s.title_left = ui_bar_home(0, UI_HEAD_H);
    s.list = (lg_rect_t){ 0, top, (int16_t)w, (int16_t)(h - top - bottom) };
    layout();
    if (!keep_scroll) {
        s.scroll = 0;
    } else if (s.scroll > max_scroll()) {
        s.scroll = max_scroll();
    }
    lg_draw_scroll_area(s.list.y, s.list.h);   /* rows scroll; header, tabs, and keyboard stay fixed */
    lg_rect_t head = { 0, 0, (int16_t)w, top };
    lg_draw_region(&head, paint_header, NULL);
    lg_draw_region(&s.list, paint_list, NULL);
}

void slist_update(void)
{
    layout();
    if (s.scroll > max_scroll()) {
        s.scroll = max_scroll();
    }
    lg_draw_scroll_area(s.list.y, s.list.h);   /* back to unscrolled memory, so the repaint lines up */
    lg_draw_region(&s.list, paint_list, NULL);
}

void slist_repaint_row(uint8_t index)
{
    if (index >= s.n) {
        return;
    }
    lg_rect_t box = row_rect(&s.rows[index]);
    int16_t y1 = box.y < s.list.y ? s.list.y : box.y;
    int16_t y2 = (int16_t)((box.y + box.h) > (s.list.y + s.list.h) ? (s.list.y + s.list.h) : (box.y + box.h));
    if (y2 <= y1) {
        return;
    }
    one_row_t ctx = { index };
    lg_rect_t visible = { 0, y1, (int16_t)s.w, (int16_t)(y2 - y1) };
    lg_draw_region(&visible, paint_one, &ctx);
}

void slist_scroll_by(int16_t dy)
{
    int16_t want = (int16_t)(s.scroll + dy);
    want = want < 0 ? 0 : (want > max_scroll() ? max_scroll() : want);
    int16_t d = (int16_t)(want - s.scroll);
    if (d == 0) {
        return;
    }
    if (d >= s.list.h || -d >= s.list.h) {
        s.scroll = want;
        lg_draw_region(&s.list, paint_list, NULL);
        return;
    }
    lg_draw_scroll(d);
    s.scroll = want;
    lg_rect_t strip = d > 0 ? (lg_rect_t){ 0, (int16_t)(s.list.y + s.list.h - d), s.list.w, d }
                            : (lg_rect_t){ 0, s.list.y, s.list.w, (int16_t)-d };
    lg_draw_region(&strip, paint_list, NULL);
}

static bool tappable(const slist_row_t *r)
{
    return !r->muted && (r->kind == ROW_ACTION || r->kind == ROW_CHOICE || r->kind == ROW_BUTTON ||
                         r->kind == ROW_CHECK || r->kind == ROW_FIELD);
}

static int row_at(int16_t x, int16_t y)
{
    if (!lg_rect_hit(&s.list, x, y)) {
        return -1;
    }
    for (uint8_t i = 0; i < s.n; i++) {
        lg_rect_t box = row_rect(&s.rows[i]);
        box.x = 0;
        box.w = (int16_t)s.w;
        if (lg_rect_hit(&box, x, y)) {
            return i;
        }
    }
    return -1;
}

static int segment_at(const slist_row_t *r, int16_t x, int16_t y)
{
    for (uint8_t k = 0; k < r->n_options; k++) {
        lg_rect_t seg = segment_rect(r, k);
        if (lg_rect_hit(&seg, x, y)) {
            return k;
        }
    }
    return -1;
}

bool slist_touch(int16_t x, int16_t y, bool down, slist_event_t *event)
{
    memset(event, 0, sizeof(*event));
    if (down && !s.was_down) {
        s.was_down = true;
        s.down_y = y;
        s.down_scroll = s.scroll;
        s.dragging = false;
        s.down_ms = now_ms();
        s.holding = false;
        s.hold_refused = false;
        int i = row_at(x, y);
        if (i >= 0 && tappable(&s.rows[i])) {
            s.pressed_seg = s.rows[i].kind == ROW_CHOICE ? segment_at(&s.rows[i], x, y) : -1;
            if (s.rows[i].kind != ROW_CHOICE || s.pressed_seg >= 0) {
                s.pressed = i;
                slist_repaint_row((uint8_t)i);   /* the pressed look */
            }
        }
        return lg_rect_hit(&s.list, x, y);
    }
    if (down) {
        if (s.holding || s.hold_refused) {
            return true;   /* a talking finger may wander; it only matters when it lets go */
        }
        if (s.pressed >= 0 && !s.dragging && s.rows[s.pressed].talk && now_ms() - s.down_ms >= SLIST_HOLD_MS) {
            s.holding = true;
            slist_repaint_row((uint8_t)s.pressed);
            event->type = SLIST_HOLD;
            event->id = s.rows[s.pressed].id;
            event->arg = s.rows[s.pressed].arg;
            event->index = (uint8_t)s.pressed;
            return true;
        }
        int16_t dy = (int16_t)(y - s.down_y);
        if (!s.dragging && (dy > DRAG_START || dy < -DRAG_START) && max_scroll() > 0) {
            s.dragging = true;
            if (s.pressed >= 0) {
                int was = s.pressed;
                s.pressed = -1;   /* a drag is not a tap */
                slist_repaint_row((uint8_t)was);
            }
        }
        if (s.dragging) {
            slist_scroll_by((int16_t)(s.down_scroll - dy - s.scroll));
        }
        return true;
    }
    if (!s.was_down) {
        return false;
    }
    s.was_down = false;
    if (s.holding || s.hold_refused) {
        int i = s.pressed;
        if (s.holding) {
            event->type = SLIST_HOLD_END;
            event->id = s.rows[i].id;
            event->arg = s.rows[i].arg;
            event->index = (uint8_t)i;
        }
        s.holding = false;
        s.hold_refused = false;
        s.pressed = -1;
        if (i >= 0) {
            slist_repaint_row((uint8_t)i);
        }
        return true;
    }
    if (s.dragging) {
        s.dragging = false;
        return true;
    }
    if (s.pressed >= 0) {
        int i = s.pressed;
        int seg = s.pressed_seg;
        s.pressed = -1;
        slist_repaint_row((uint8_t)i);
        if (row_at(x, y) == i) {
            event->type = SLIST_ROW;
            event->id = s.rows[i].id;
            event->arg = s.rows[i].kind == ROW_CHOICE ? seg : s.rows[i].arg;
            event->index = (uint8_t)i;
        }
        return true;
    }
    if (lg_rect_hit(&s.head_icon, x, y)) {
        event->type = SLIST_BACK;
        return true;
    }
    if (s.plus && lg_rect_hit(&s.plus_icon, x, y)) {
        event->type = SLIST_PLUS;
        return true;
    }
    if (s.n_tabs && y >= UI_HEAD_H && y < UI_HEAD_H + TAB_H) {
        event->type = SLIST_TAB;
        event->index = (uint8_t)(x / (s.w / s.n_tabs));
        return true;
    }
    return false;
}

void slist_hold_refused(void)
{
    s.holding = false;
    s.hold_refused = true;
    if (s.pressed >= 0) {
        slist_repaint_row((uint8_t)s.pressed);
    }
}

void slist_redraw(void)
{
    slist_show(s.w, s.h, s.bottom, true);
}
