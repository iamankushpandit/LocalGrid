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
 * - The keyboard is spike_kb: letters with shift, numbers and symbols, and emoji.
 * - Any conversation: 1:1 (end-to-end encrypted by the service), a group, or everyone. A group
 *   message counts delivered and read handhelds instead of naming one state (D42).
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
#include "spike_kb.h"
#include "spike_nav.h"
#include "spike_overlay.h"
#include "spike_theme.h"

static const char *TAG = "UI";

#define PAD          6
#define GAP          4
#define HEAD_H       30
#define INPUT_H      30
#define LINES_MAX    12
#define DRAG_START   6       /* pixels a finger moves before a press becomes a scroll */
#define FLASH_MS     700     /* the new-message arrow: on this long, off this long */
#define ARROW_SIZE   36

#define MARK_WAIT      "\xF0\x9F\x95\x93"   /* U+1F553 clock: this handheld still holds it */

typedef struct {
    uint32_t id;
    int16_t  y;           /* top, in list coordinates (0 = above the first message) */
    int16_t  h;
    bool     mine;
} bubble_t;

static struct {
    uint16_t   w;
    uint16_t   h;
    uint8_t    scope;
    uint32_t   target;
    char       title[40];
    lg_rect_t  list;
    lg_rect_t  field;
    lg_rect_t  send;
    lg_rect_t  back;
    lg_rect_t  arrow;
    bool       keyboard;
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
    if (m->scope != s.scope) {
        return false;
    }
    switch (s.scope) {
    case LG_SCOPE_BROADCAST: return true;
    case LG_SCOPE_GROUP:     return m->target == s.target;
    default:                 return m->mine ? m->target == s.target : m->author == s.target;
    }
}

/* A group message's delivered and read counts (D42): everyone in the group but the sender. */
static void count_text(const hh_message_t *m, char *out, size_t cap)
{
    out[0] = '\0';
    if (!m->mine || m->scope == LG_SCOPE_DIRECT || m->state == HH_MSG_REJECTED || m->state == HH_MSG_REFUSED) {
        return;
    }
    const hh_status_t *st = spike_status();
    uint8_t total = 0;
    for (uint8_t i = 0; i < st->n_groups && m->scope == LG_SCOPE_GROUP; i++) {
        if (st->groups[i].id == (uint16_t)m->target) {
            total = st->groups[i].members > 0 ? (uint8_t)(st->groups[i].members - 1u) : 0u;
        }
    }
    if (total > 0 && m->read_count >= total) {
        snprintf(out, cap, "all read");
        return;
    }
    if (total > 0) {
        snprintf(out, cap, "%u/%u", (unsigned)m->delivered_count, (unsigned)total);
    } else if (m->delivered_count > 0) {
        snprintf(out, cap, "%u", (unsigned)m->delivered_count);   /* everyone: no denominator */
    }
    if (m->read_count > 0 && out[0] != '\0') {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, " %s%u", LV_SYMBOL_EYE_OPEN, (unsigned)m->read_count);
    }
}

/* Who wrote a received message, for group and everyone chats. */
static const char *author_name(uint32_t device)
{
    const hh_status_t *st = spike_status();
    for (uint8_t i = 0; i < st->n_people; i++) {
        if (st->people[i].device == device) {
            return st->people[i].name;
        }
    }
    return "unknown handheld";
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
        /* It is on this screen, so its reader has it in front of them: tell the author (D34, D42).
         * Everyone messages do not report reads. */
        if (!m.mine && !m.read_sent && (m.scope == LG_SCOPE_DIRECT || m.scope == LG_SCOPE_GROUP)) {
            hh_service_mark_read(m.id);
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
        char clock[48];
        uint32_t day = m.grid_time % 86400u;
        char counts[24];
        count_text(&m, counts, sizeof(counts));
        if (!m.mine && s.scope != LG_SCOPE_DIRECT) {
            snprintf(clock, sizeof(clock), "%02u:%02u %s", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u),
                     author_name(m.author));
        } else {
            snprintf(clock, sizeof(clock), "%02u:%02u%s%s", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u),
                     counts[0] ? "  " : "", counts);
        }
        lg_color_t mark_colour = C_MUTED;
        /* A group or everyone message shows counts, not a state; a refusal keeps the warning icon. */
        const char *mark = !m.mine ? NULL
                         : (s.scope == LG_SCOPE_DIRECT || m.state == HH_MSG_REJECTED || m.state == HH_MSG_REFUSED ||
                            m.state == HH_MSG_PENDING)
                               ? marker(&m, &mark_colour)
                               : NULL;
        int16_t note_w = (int16_t)(lg_draw_text_width(&lv_font_montserrat_10, F_EMOJI, clock) +
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
        lg_paint_text(c, &s.list, nx, (int16_t)(note_top + 1), &lv_font_montserrat_10, F_EMOJI, C_MUTED, clock,
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

static void place(bool keyboard)
{
    s.keyboard = keyboard;
    int16_t kb_h = keyboard ? (int16_t)(spike_kb_height() + GAP) : 0;
    int16_t input_y = (int16_t)(s.h - kb_h - INPUT_H - GAP);
    s.back = (lg_rect_t){ (int16_t)(s.w - 40), 0, 40, HEAD_H };
    s.list = (lg_rect_t){ 0, HEAD_H, (int16_t)s.w, (int16_t)(input_y - HEAD_H - GAP) };
    s.arrow = (lg_rect_t){ (int16_t)((s.w - ARROW_SIZE) / 2), (int16_t)(s.list.y + (s.list.h - ARROW_SIZE) / 2),
                           ARROW_SIZE, ARROW_SIZE };
    s.field = (lg_rect_t){ PAD, input_y, (int16_t)(s.w - 2 * PAD - 36), INPUT_H };
    s.send = (lg_rect_t){ (int16_t)(s.w - PAD - 32), input_y, 32, INPUT_H };
    if (keyboard) {
        spike_kb_open(s.w, s.h, s.input, sizeof(s.input));
    }
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
        spike_kb_draw();
    }
}

void spike_chat_open(uint16_t w, uint16_t h, uint8_t scope, uint32_t target, const char *title)
{
    memset(&s, 0, sizeof(s));
    s.w = w;
    s.h = h;
    s.scope = scope;
    s.target = target;
    snprintf(s.title, sizeof(s.title), "%s", title != NULL ? title : "Chat");
    const hh_status_t *st = spike_status();
    place(false);
    layout();
    s.scroll = max_scroll();   /* newest at the bottom, in view */
    s.shown_messages = st->messages_version;
    s.newest_id = s.n_bubbles ? s.bubbles[s.n_bubbles - 1u].id : 0;
    spike_notify_mark_seen(scope, target);
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
        spike_notify_mark_seen(s.scope, s.target);
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

static void keyboard_event(kb_event_t ev)
{
    s.input_len = strlen(s.input);
    if (ev == KB_TEXT_CHANGED) {
        draw_field();
    } else if (ev == KB_HIDE) {
        place(false);
        draw_all();
    }
}

bool spike_chat_touch(int16_t x, int16_t y, bool down)
{
    if (s.keyboard) {
        kb_event_t kev;
        if (spike_kb_touch(x, y, down, &kev)) {
            keyboard_event(kev);
            return false;
        }
    }
    if (down && !s.was_down) {
        s.was_down = true;
        s.down_x = x;
        s.down_y = y;
        s.down_scroll = s.scroll;
        s.dragging = false;
        return false;
    }
    if (down) {
        int16_t dy = (int16_t)(y - s.down_y);
        if (!s.dragging && lg_rect_hit(&s.list, s.down_x, s.down_y) &&
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
    } else if (lg_rect_hit(&s.send, x, y) && s.input_len > 0 && (s.scope != LG_SCOPE_DIRECT || s.target != 0)) {
        esp_err_t err = hh_service_send(s.scope, s.target, false, s.input);
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
    if (s.keyboard) {
        keyboard_event(spike_kb_type(text));
    }
}

void spike_chat_page(int page)
{
    if (s.keyboard) {
        spike_kb_page(page);
    }
}

void spike_chat_log(void)
{
    uint32_t key_paints = 0;
    uint64_t key_us = 0;
    spike_kb_stats(&key_paints, &key_us);
    ESP_LOGI(TAG, "[UI] Spike chat: %u bubbles, content %d px, full list paints %" PRIu32 " (avg %" PRIu64
             " ms), scroll steps %" PRIu32 " (avg %" PRIu64 " ms), key paints %" PRIu32 " (avg %" PRIu64 " ms)",
             s.n_bubbles, s.content_h, s.list_paints, s.list_paints ? s.list_us / s.list_paints / 1000u : 0u,
             s.scroll_steps, s.scroll_steps ? s.scroll_us / s.scroll_steps / 1000u : 0u, key_paints,
             key_paints ? key_us / key_paints / 1000u : 0u);
}

void spike_chat_redraw(void)
{
    place(s.keyboard);   /* restores the scroll area an overlay turned off; the reader keeps their place */
    draw_all();
}

bool spike_chat_is(uint8_t scope, uint32_t target)
{
    return s.scope == scope && (scope == LG_SCOPE_BROADCAST || s.target == target);
}
