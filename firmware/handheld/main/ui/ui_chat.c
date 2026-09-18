/*
 * A conversation: 1:1, group, or Everyone (D55).
 *
 * - The message list is the panel's hardware scroll area: dragging moves it with one command and
 *   paints only the rows that scroll into view, so scrolling costs a few pixel rows per step.
 * - Bubbles carry the delivery icons of D42: a clock while this handheld holds a message, an up
 *   arrow once an AP took it, a down arrow once the other handheld has it, an eye once it was
 *   shown there. The colours are the theme's marker roles.
 * - A message that arrives while the reader is scrolled up does not move the list. A down arrow
 *   flashes slowly in the middle of the list instead, and goes when the reader reaches the end.
 * - The keyboard is ui_kb: letters with shift, numbers and symbols, and emoji.
 * - In Everyone, a toggle beside the send key marks the message urgent, and sending one asks
 *   first: an urgent broadcast seizes every screen in the grid (D41). The toggle is locked on
 *   when urgent is all this handheld may send -- the admin page has not let it announce (D56),
 *   or grid time is not set (D6) -- so an emergency is never blocked.
 * - Any conversation: 1:1 (end-to-end encrypted by the service), a group, or everyone. A group
 *   message counts delivered and read handhelds instead of naming one state (D42), and under our
 *   own group or broadcast message a line names who has read it (D58).
 * - Push-to-talk (D61): in a 1:1 or group chat, on a board with a microphone, a full-width bar
 *   under the field while the keyboard is down. Hold it to talk; letting go sends the end. While
 *   someone talks in this conversation the title says who, and the bar says to wait.
 *
 * RAM holds a layout per message (id, position, height, side), never its text: a bubble fetches
 * its message from the service when painted.
 */
#include "ui_chat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hh_service.h"
#include "hh_voice.h"
#include "lg_draw.h"
#include "lg_emoji.h"
#include "lg_envelope.h"
#include "ui_kb.h"
#include "ui_lock.h"
#include "ui_nav.h"
#include "ui_overlay.h"
#include "ui_theme.h"

static const char *TAG = "UI";

#define PAD          6
#define GAP          4
#define HEAD_H       30
#define INPUT_H      30
#define LINES_MAX    12
#define DRAG_START   6       /* pixels a finger moves before a press becomes a scroll */
#define FLASH_MS     700     /* the new-message arrow: on this long, off this long */
#define ARROW_SIZE   36
#define TALK_H       40
#define TALK_GRACE_MS  150   /* a finger that leaves the bar this briefly is still holding it */
#define TALK_NOTE_MS   4000  /* how long the reason a talk stopped stays on the bar */

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
    lg_rect_t  urgent_key;
    lg_rect_t  confirm;
    lg_rect_t  confirm_yes;
    lg_rect_t  confirm_no;
    lg_rect_t  back;
    lg_rect_t  arrow;
    lg_rect_t  talk;             /* the push-to-talk bar; zero height when there is none */
    int16_t    title_left;       /* the title starts here, after the house that goes home (D62) */
    int16_t    title_right;      /* the title ends here: the talking slot, battery and padlock follow */
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
    bool       urgent;           /* this message goes as an urgent broadcast */
    bool       urgent_locked;    /* urgent is all this handheld may send here */
    bool       confirming;       /* the "send to everyone as urgent?" panel is up */
    bool       talk_held;        /* the finger is on the talk bar and we are talking */
    uint32_t   talk_up_ms;       /* when the finger left the bar, 0 while it is on it */
    hh_voice_state_t voice;      /* as last drawn */
    char       talk_note[HH_PROBLEM_MAX];   /* why the last talk stopped, shown for a while */
    uint32_t   talk_note_ms;
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

static const lg_font_t *body_font(void)
{
    return &lg_font_montserrat_12;
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
    const hh_status_t *st = ui_status();
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
        snprintf(out + used, cap - used, " %s%u", LG_SYMBOL_EYE_OPEN, (unsigned)m->read_count);
    }
}

/*
 * "Read by Pinky, Bluey" under our own group or broadcast message (D58): who has opened it, not
 * only how many. Empty for 1:1, where the eye marker already says it, and for received messages.
 * Names are shortened to what the bubble can hold, on a name boundary, with a count instead.
 */
static void readers_text(const hh_message_t *m, char *out, size_t cap)
{
    out[0] = '\0';
    if (!m->mine || m->scope == LG_SCOPE_DIRECT || m->read_mask == 0) {
        return;
    }
    char names[72];   /* only what a line can hold: the count form covers the rest */
    uint8_t n = hh_service_reader_names(m->read_mask, names, sizeof(names));
    if (n == 0) {
        return;
    }
    snprintf(out, cap, "Read by %s", names);
    if (lg_draw_text_width(&lg_font_montserrat_10, NULL, out) <= bubble_text_w()) {
        return;
    }
    snprintf(out, cap, "Read by %u handhelds", (unsigned)n);   /* too many to name in one line */
}

/* Who wrote a received message, for group and everyone chats. */
static const char *author_name(uint32_t device)
{
    const hh_status_t *st = ui_status();
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
        char readers[96];
        readers_text(&m, readers, sizeof(readers));
        bubble_t *b = &s.bubbles[s.n_bubbles++];
        b->id = m.id;
        b->mine = m.mine;
        b->y = y;
        b->h = (int16_t)(PAD + (lines ? lines : 1) * line_h + lg_font_montserrat_12.line_height + PAD / 2 +
                         (readers[0] ? lg_font_montserrat_10.line_height : 0));
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
    case HH_MSG_ACCEPTED:  *colour = C_MARK_NODE;  return LG_SYMBOL_UPLOAD;
    case HH_MSG_DELIVERED: *colour = C_MARK_DELIV; return LG_SYMBOL_DOWNLOAD;
    case HH_MSG_READ:      *colour = C_MARK_READ;  return LG_SYMBOL_EYE_OPEN;
    default:               *colour = C_ERROR;      return LG_SYMBOL_WARNING;
    }
}

static void paint_arrow(const lg_canvas_t *c)
{
    lg_paint_panel(c, &s.list, &s.arrow, C_OUTLINE, C_BG, C_ACCENT, 1, ARROW_SIZE / 2);
    const lg_font_t *f = &lg_font_montserrat_16;
    int16_t tw = lg_draw_text_width(f, NULL, LG_SYMBOL_DOWN);
    lg_paint_text(c, &s.arrow, (int16_t)(s.arrow.x + (s.arrow.w - tw) / 2),
                  (int16_t)(s.arrow.y + (s.arrow.h - f->line_height) / 2), f, NULL, C_ACCENT, LG_SYMBOL_DOWN,
                  strlen(LG_SYMBOL_DOWN));
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
        char readers[96];
        readers_text(&m, readers, sizeof(readers));
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
        int16_t note_w = (int16_t)(lg_draw_text_width(&lg_font_montserrat_10, F_EMOJI, clock) +
                                   (mark ? 4 + lg_draw_text_width(&lg_font_montserrat_12, &lg_font_emoji_14, mark) : 0));
        int16_t readers_w = readers[0] ? lg_draw_text_width(&lg_font_montserrat_10, NULL, readers) : 0;
        note_w = readers_w > note_w ? readers_w : note_w;
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
        lg_paint_text(c, &s.list, nx, (int16_t)(note_top + 1), &lg_font_montserrat_10, F_EMOJI, C_MUTED, clock,
                      strlen(clock));
        if (readers[0] != '\0') {
            int16_t rx = (int16_t)(box.x + box.w - PAD - readers_w);
            lg_paint_text(c, &s.list, rx, (int16_t)(note_top + 1 + lg_font_montserrat_10.line_height),
                          &lg_font_montserrat_10, NULL, C_MARK_READ, readers, strlen(readers));
        }
        if (mark != NULL) {
            lg_paint_text(c, &s.list, (int16_t)(nx + note_w - lg_draw_text_width(&lg_font_montserrat_12,
                                                                                &lg_font_emoji_14, mark)),
                          note_top, &lg_font_montserrat_12, &lg_font_emoji_14, mark_colour, mark, strlen(mark));
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
        ESP_LOGI(TAG, "[UI] Chat: reached the newest message; arrow gone");
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
        const char *hint = s.urgent_locked ? "Urgent message only" : "Message";
        lg_paint_text(c, &s.field, (int16_t)(s.field.x + PAD), top, body_font(), NULL,
                      s.urgent_locked ? C_WARNING : C_MUTED, hint, strlen(hint));
    } else {
        lg_paint_text(c, &s.field, (int16_t)(s.field.x + PAD), top, body_font(), &lg_font_emoji_14, C_TEXT, shown,
                      strlen(shown));
    }
    int16_t cursor = (int16_t)(s.field.x + PAD +
                               (s.input_len ? lg_draw_text_width(body_font(), &lg_font_emoji_14, shown) : 0));
    lg_rect_t bar = { cursor, (int16_t)(top + 1), 1, (int16_t)(body_font()->line_height - 2) };
    lg_paint_panel(c, &s.field, &bar, C_ACCENT, C_ACCENT, C_ACCENT, 0, 0);
    if (s.scope == LG_SCOPE_BROADCAST) {
        /* The urgent key: outlined when off, filled when on, so the state reads at a glance. */
        lg_color_t ink = s.urgent ? C_ACCENT_INK : C_MUTED;
        lg_paint_panel(c, &s.urgent_key, &s.urgent_key, s.urgent ? C_ERROR : C_BG, C_BG,
                       s.urgent ? C_ERROR : C_OUTLINE, 1, 4);
        int16_t uw = lg_draw_text_width(&lg_font_montserrat_16, NULL, LG_SYMBOL_WARNING);
        lg_paint_text(c, &s.urgent_key, (int16_t)(s.urgent_key.x + (s.urgent_key.w - uw) / 2),
                      (int16_t)(s.urgent_key.y + (s.urgent_key.h - lg_font_montserrat_16.line_height) / 2),
                      &lg_font_montserrat_16, NULL, ink, LG_SYMBOL_WARNING, strlen(LG_SYMBOL_WARNING));
    }
    int16_t sx = (int16_t)(s.send.x + (s.send.w - lg_draw_text_width(&lg_font_montserrat_16, NULL, LG_SYMBOL_OK)) / 2);
    lg_paint_text(c, &s.send, sx, (int16_t)(s.send.y + (s.send.h - lg_font_montserrat_16.line_height) / 2),
                  &lg_font_montserrat_16, NULL, s.urgent ? C_ERROR : C_ACCENT, LG_SYMBOL_OK,
                  strlen(LG_SYMBOL_OK));
}

static void draw_field(void)
{
    lg_rect_t row = { 0, s.field.y, (int16_t)s.w, s.field.h };
    lg_draw_region(&row, paint_field, NULL);
}

/* ---- "send this to everyone as urgent?" ---- */

#define CONFIRM_LINES 2
#define CONFIRM_BTN_H 34

static const char *const CONFIRM_TEXT[CONFIRM_LINES] = {
    "This takes over every",
    "screen until it is read.",
};

/*
 * The panel's box and its two buttons, worked out from the text it holds and kept in `s`, so the
 * painter and the touch test cannot disagree. They did once: both guessed the height separately
 * and the buttons landed on top of the second line.
 */
static void place_confirm(void)
{
    int16_t cw = (int16_t)(s.w - 4 * PAD);
    int16_t ch = (int16_t)(2 * PAD + F_TITLE->line_height + 4 + CONFIRM_LINES * F_SMALL->line_height + PAD +
                           CONFIRM_BTN_H + PAD);
    /* Centred in the list, or on the whole panel when the keyboard leaves too little room. */
    int16_t cy = ch <= s.list.h ? (int16_t)(s.list.y + (s.list.h - ch) / 2) : (int16_t)((s.h - ch) / 2);
    s.confirm = (lg_rect_t){ (int16_t)(2 * PAD), cy, cw, ch };
    int16_t bw = (int16_t)((cw - 3 * PAD) / 2);
    int16_t by = (int16_t)(s.confirm.y + ch - PAD - CONFIRM_BTN_H);
    s.confirm_no = (lg_rect_t){ (int16_t)(s.confirm.x + PAD), by, bw, CONFIRM_BTN_H };
    s.confirm_yes = (lg_rect_t){ (int16_t)(s.confirm.x + cw - PAD - bw), by, bw, CONFIRM_BTN_H };
}

static void centre_text(const lg_canvas_t *c, const lg_rect_t *clip, const lg_rect_t *in, int16_t y,
                        const lg_font_t *font, lg_color_t fg, const char *text)
{
    lg_paint_text(c, clip, (int16_t)(in->x + (in->w - lg_draw_text_width(font, NULL, text)) / 2), y, font, NULL, fg,
                  text, strlen(text));
}

static void paint_confirm(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    const lg_rect_t *r = &s.confirm;
    lg_paint_panel(c, r, r, C_SURFACE, C_BG, C_ERROR, 2, 6);
    int16_t y = (int16_t)(r->y + PAD);
    centre_text(c, r, r, y, F_TITLE, C_ERROR, "Urgent broadcast");
    y = (int16_t)(y + F_TITLE->line_height + 4);
    for (int i = 0; i < CONFIRM_LINES; i++) {
        centre_text(c, r, r, y, F_SMALL, C_TEXT, CONFIRM_TEXT[i]);
        y = (int16_t)(y + F_SMALL->line_height);
    }
    lg_paint_panel(c, r, &s.confirm_no, C_SURFACE, C_SURFACE, C_OUTLINE, 1, 4);
    lg_paint_panel(c, r, &s.confirm_yes, C_ERROR, C_SURFACE, C_ERROR, 1, 4);
    int16_t btn_y = (int16_t)(s.confirm_no.y + (CONFIRM_BTN_H - F_BODY->line_height) / 2);
    centre_text(c, &s.confirm_no, &s.confirm_no, btn_y, F_BODY, C_TEXT, "Cancel");
    centre_text(c, &s.confirm_yes, &s.confirm_yes, btn_y, F_BODY, C_ACCENT_INK, "Send");
}

static void draw_confirm(void)
{
    lg_draw_region(&s.confirm, paint_confirm, NULL);
}

static void place(bool keyboard)
{
    s.keyboard = keyboard;
    int16_t kb_h = keyboard ? (int16_t)(ui_kb_height() + GAP) : 0;
    hh_voice_state(&s.voice);
    bool talk = !keyboard && s.voice.can_talk && (s.scope == LG_SCOPE_DIRECT || s.scope == LG_SCOPE_GROUP);
    int16_t talk_h = talk ? (int16_t)(TALK_H + GAP) : 0;
    int16_t input_y = (int16_t)(s.h - kb_h - talk_h - INPUT_H - GAP);
    s.talk = talk ? (lg_rect_t){ PAD, (int16_t)(input_y + INPUT_H + GAP), (int16_t)(s.w - 2 * PAD), TALK_H }
                  : (lg_rect_t){ 0, 0, 0, 0 };
    s.back = (lg_rect_t){ (int16_t)(s.w - 40), 0, 40, HEAD_H };
    s.title_right = ui_bar_place(s.back.x, 0, HEAD_H);   /* ui_main puts the badge and padlock there */
    s.title_left = ui_bar_home(0, HEAD_H);                /* and the house at the left */
    s.list = (lg_rect_t){ 0, HEAD_H, (int16_t)s.w, (int16_t)(input_y - HEAD_H - GAP) };
    s.arrow = (lg_rect_t){ (int16_t)((s.w - ARROW_SIZE) / 2), (int16_t)(s.list.y + (s.list.h - ARROW_SIZE) / 2),
                           ARROW_SIZE, ARROW_SIZE };
    bool has_urgent = s.scope == LG_SCOPE_BROADCAST;
    int16_t keys_w = (int16_t)(has_urgent ? 72 : 36);
    s.field = (lg_rect_t){ PAD, input_y, (int16_t)(s.w - 2 * PAD - keys_w), INPUT_H };
    s.send = (lg_rect_t){ (int16_t)(s.w - PAD - 32), input_y, 32, INPUT_H };
    s.urgent_key = (lg_rect_t){ (int16_t)(s.w - PAD - 68), input_y, 32, INPUT_H };
    place_confirm();
    if (keyboard) {
        ui_kb_open(s.w, s.h, s.input, sizeof(s.input));
    }
    if (s.scroll > max_scroll()) {
        s.scroll = max_scroll();
    }
    /* The list is the panel's scroll area; the header, field, and keyboard stay fixed. */
    lg_draw_scroll_area(s.list.y, s.list.h);
}

/* ---- push-to-talk ---- */

static bool hearing_here(const hh_voice_state_t *vs)
{
    return vs->heard != 0 && vs->heard_scope == s.scope && vs->heard_target == s.target;
}

static void paint_talk(const lg_canvas_t *c, void *ctx)
{
    (void)ctx;
    const lg_rect_t *r = &s.talk;
    const char *text = "Hold to talk";
    char line[HH_PROBLEM_MAX + HH_NAME_MAX];
    lg_color_t fill = C_SURFACE;
    lg_color_t edge = C_ACCENT;
    lg_color_t ink = C_ACCENT;
    if (s.voice.talking) {
        text = "Talking... let go to finish";
        fill = edge = C_ERROR;
        ink = C_ACCENT_INK;
    } else if (s.voice.heard != 0) {
        snprintf(line, sizeof(line), "%s is talking", author_name(s.voice.heard));
        text = line;
        edge = ink = C_MUTED;
    } else if (s.talk_note[0] != '\0') {
        text = s.talk_note;
        edge = ink = C_WARNING;
    }
    lg_paint_panel(c, r, r, fill, C_BG, edge, 2, 6);
    const lg_font_t *f = F_BODY;
    if (lg_draw_text_width(f, NULL, text) > r->w - 2 * PAD) {
        f = F_SMALL;   /* a long reason still fits the bar */
    }
    int16_t tw = lg_draw_text_width(f, NULL, text);
    lg_paint_text(c, r, (int16_t)(r->x + (r->w - tw) / 2), (int16_t)(r->y + (r->h - f->line_height) / 2), f, NULL, ink,
                  text, strlen(text));
}

static void draw_talk(void)
{
    if (s.talk.h > 0) {
        lg_draw_region(&s.talk, paint_talk, NULL);
    }
}

static void draw_title(void)
{
    lg_box_t title;
    memset(&title, 0, sizeof(title));
    title.rect = (lg_rect_t){ s.title_left, 0, (int16_t)(s.title_right - s.title_left), HEAD_H };
    title.bg = title.outside = C_BG;
    title.font = &lg_font_montserrat_14;
    title.fallback = &lg_font_emoji_14;
    title.fg = C_ACCENT;
    title.pad = 2;   /* the house before it already leaves the margin */
    if (hearing_here(&s.voice)) {
        /* Who is talking, where everyone looks first, on boards that cannot talk back too. */
        title.fg = C_WARNING;
        snprintf(title.text, sizeof(title.text), "%s is talking", author_name(s.voice.heard));
    } else {
        snprintf(title.text, sizeof(title.text), "%s", s.title);
    }
    lg_draw_box(&title);
    lg_rect_t spare = ui_bar_spare_rect();   /* the talking mark's slot; ui_main paints the mark */
    if (spare.w > 0) {
        lg_draw_fill(&spare, C_BG);
    }
    lg_box_t back = title;
    back.rect = s.back;
    back.font = &lg_font_montserrat_16;
    back.align = LG_ALIGN_CENTER;
    snprintf(back.text, sizeof(back.text), "%s", LG_SYMBOL_LEFT);
    lg_draw_box(&back);
}

static void draw_all(void)
{
    draw_title();
    draw_list();
    lg_rect_t gap = { 0, (int16_t)(s.list.y + s.list.h), (int16_t)s.w, GAP };
    lg_draw_fill(&gap, C_BG);
    draw_field();
    draw_talk();
    /* Between the field and the keyboard (or the bottom edge): nothing else paints this strip, so
     * whatever an alert left there stayed on screen when the alert was dismissed. */
    int16_t below = (int16_t)(s.talk.h > 0 ? s.talk.y + s.talk.h : s.field.y + s.field.h);
    lg_rect_t tail = { 0, below, (int16_t)s.w, (int16_t)(s.h - below - (s.keyboard ? ui_kb_height() : 0)) };
    if (tail.h > 0) {
        lg_draw_fill(&tail, C_BG);
    }
    if (s.talk.h > 0) {
        lg_rect_t between = { 0, (int16_t)(s.field.y + s.field.h), (int16_t)s.w, GAP };
        lg_rect_t left = { 0, s.talk.y, PAD, s.talk.h };
        lg_rect_t right = { (int16_t)(s.talk.x + s.talk.w), s.talk.y, PAD, s.talk.h };
        lg_draw_fill(&between, C_BG);
        lg_draw_fill(&left, C_BG);
        lg_draw_fill(&right, C_BG);
    }
    if (s.keyboard) {
        ui_kb_draw();
    }
}

void ui_chat_open(uint16_t w, uint16_t h, uint8_t scope, uint32_t target, const char *title)
{
    memset(&s, 0, sizeof(s));
    s.w = w;
    s.h = h;
    s.scope = scope;
    s.target = target;
    snprintf(s.title, sizeof(s.title), "%s", title != NULL ? title : "Chat");
    const hh_status_t *st = ui_status();
    s.urgent_locked = scope == LG_SCOPE_BROADCAST && (!st->may_announce || st->time_restricted);
    s.urgent = s.urgent_locked;   /* D56 and D6: urgent is all that gets out, so it starts on */
    place(false);
    layout();
    s.scroll = max_scroll();   /* newest at the bottom, in view */
    s.shown_messages = st->messages_version;
    s.newest_id = s.n_bubbles ? s.bubbles[s.n_bubbles - 1u].id : 0;
    ui_notify_mark_seen(scope, target);
    draw_all();
    ESP_LOGI(TAG, "[UI] Chat with %s: %u message(s), content %d px", s.title, s.n_bubbles, s.content_h);
}

void ui_chat_close(void)
{
    lg_draw_scroll_area(0, 0);   /* the next screen is drawn unscrolled */
}

void ui_chat_refresh(const hh_status_t *st)
{
    bool locked = s.scope == LG_SCOPE_BROADCAST && (!st->may_announce || st->time_restricted);
    if (locked != s.urgent_locked) {
        s.urgent_locked = locked;   /* the admin page or grid time changed while this was open */
        s.urgent = s.urgent || locked;
        draw_field();
    }
    if (st->messages_version != s.shown_messages) {
        s.shown_messages = st->messages_version;
        ui_notify_mark_seen(s.scope, s.target);
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
            ESP_LOGI(TAG, "[UI] Chat: new message below; arrow flashing");
        }
    }
}

static void talk_release(void)
{
    s.talk_held = false;
    s.talk_up_ms = 0;
    hh_voice_ptt_stop();
    ESP_LOGI(TAG, "[UI] Talk bar released");
}

/* Follows the voice module: who is talking, whether we are, and why a talk stopped. */
static void talk_tick(uint32_t now_ms)
{
    if (s.talk_held && s.talk_up_ms != 0 && now_ms - s.talk_up_ms >= TALK_GRACE_MS) {
        talk_release();
    }
    hh_voice_state_t vs;
    hh_voice_state(&vs);
    bool note_expired = s.talk_note[0] != '\0' && now_ms - s.talk_note_ms >= TALK_NOTE_MS;
    if (vs.version == s.voice.version && !note_expired) {
        return;
    }
    bool title_changed = hearing_here(&vs) != hearing_here(&s.voice) || vs.heard != s.voice.heard;
    if (vs.problem[0] != '\0' && strcmp(vs.problem, s.voice.problem) != 0) {
        snprintf(s.talk_note, sizeof(s.talk_note), "%s", vs.problem);
        s.talk_note_ms = now_ms;
    } else if (note_expired) {
        s.talk_note[0] = '\0';
    }
    if (!vs.talking && s.talk_held && s.talk_up_ms == 0 && vs.problem[0] != '\0') {
        s.talk_held = false;   /* the talk was stopped for us (refused, or a minute is up) */
    }
    s.voice = vs;
    if (title_changed) {
        draw_title();
    }
    draw_talk();
}

void ui_chat_tick(uint32_t now_ms)
{
    talk_tick(now_ms);
    if (!s.new_below || now_ms - s.flash_ms < FLASH_MS) {
        return;
    }
    s.flash_ms = now_ms;
    s.arrow_on = !s.arrow_on;
    draw_arrow(s.arrow_on);
}



/* Hands the text to the service, urgent or not, and clears the field when it was taken. */
static void send_now(void)
{
    esp_err_t err = hh_service_send(s.scope, s.target, s.urgent, s.input);
    ESP_LOGI(TAG, "[UI] Chat send%s: %s", s.urgent ? " (urgent)" : "",
             err == ESP_OK ? "queued" : esp_err_to_name(err));
    if (err != ESP_OK) {
        return;
    }
    s.input_len = 0;
    s.input[0] = '\0';
    if (!s.urgent_locked) {
        s.urgent = false;   /* urgent is per message, never a mode the next one inherits */
    }
    draw_field();
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

bool ui_chat_touch(int16_t x, int16_t y, bool down)
{
    if (s.confirming) {   /* the panel owns every touch until it is answered */
        if (down || !s.was_down) {
            s.was_down = down;
            return false;
        }
        s.was_down = false;
        bool yes = lg_rect_hit(&s.confirm_yes, x, y);
        if (!yes && !lg_rect_hit(&s.confirm_no, x, y)) {
            return false;
        }
        s.confirming = false;
        if (yes) {
            send_now();
        } else {
            ESP_LOGI(TAG, "[UI] Urgent broadcast cancelled");
        }
        draw_list();   /* the panel covered the list */
        draw_field();
        return false;
    }
    if (s.keyboard) {
        kb_event_t kev;
        if (ui_kb_touch(x, y, down, &kev)) {
            keyboard_event(kev);
            return false;
        }
    }
    if (s.talk_held) {   /* the bar owns the finger until it lets go */
        if (down) {
            s.talk_up_ms = 0;
        } else if (s.talk_up_ms == 0) {
            s.talk_up_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        s.was_down = down;
        return false;
    }
    if (down && !s.was_down && s.talk.h > 0 && lg_rect_hit(&s.talk, x, y)) {
        s.was_down = true;
        s.dragging = true;   /* the lift that follows is not a tap on anything */
        esp_err_t err = hh_voice_ptt_start(s.scope, s.target);
        if (err == ESP_OK) {
            s.talk_held = true;
            s.talk_up_ms = 0;
            s.talk_note[0] = '\0';
            hh_voice_state(&s.voice);
            draw_talk();
        }
        ESP_LOGI(TAG, "[UI] Talk bar pressed: %s", err == ESP_OK ? "talking" : esp_err_to_name(err));
        return false;
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
    } else if (s.scope == LG_SCOPE_BROADCAST && lg_rect_hit(&s.urgent_key, x, y)) {
        if (!s.urgent_locked) {
            s.urgent = !s.urgent;
            draw_field();
            ESP_LOGI(TAG, "[UI] Urgent %s", s.urgent ? "on" : "off");
        }
    } else if (lg_rect_hit(&s.field, x, y) && !s.keyboard) {
        place(true);
        draw_all();
    } else if (lg_rect_hit(&s.send, x, y) && s.input_len > 0 && (s.scope != LG_SCOPE_DIRECT || s.target != 0)) {
        if (s.urgent) {
            s.confirming = true;   /* an urgent broadcast is asked about before it goes (owner) */
            draw_confirm();
        } else {
            send_now();
        }
    }
    return false;
}

void ui_chat_scroll_by(int16_t dy)
{
    scroll_to((int16_t)(s.scroll + dy));
}

void ui_chat_keyboard(bool show)
{
    place(show);
    draw_all();
}

void ui_chat_type(const char *text)
{
    if (s.keyboard) {
        keyboard_event(ui_kb_type(text));
    }
}

void ui_chat_page(int page)
{
    if (s.keyboard) {
        ui_kb_page(page);
    }
}

void ui_chat_log(void)
{
    uint32_t key_paints = 0;
    uint64_t key_us = 0;
    ui_kb_stats(&key_paints, &key_us);
    ESP_LOGI(TAG, "[UI] Chat: %u bubbles, content %d px, full list paints %" PRIu32 " (avg %" PRIu64
             " ms), scroll steps %" PRIu32 " (avg %" PRIu64 " ms), key paints %" PRIu32 " (avg %" PRIu64 " ms)",
             s.n_bubbles, s.content_h, s.list_paints, s.list_paints ? s.list_us / s.list_paints / 1000u : 0u,
             s.scroll_steps, s.scroll_steps ? s.scroll_us / s.scroll_steps / 1000u : 0u, key_paints,
             key_paints ? key_us / key_paints / 1000u : 0u);
}

void ui_chat_redraw(void)
{
    place(s.keyboard);   /* restores the scroll area an overlay turned off; the reader keeps their place */
    draw_all();
}

bool ui_chat_is(uint8_t scope, uint32_t target)
{
    return s.scope == scope && (scope == LG_SCOPE_BROADCAST || s.target == target);
}
