/*
 * Conversation list and chat screens (P6).
 *
 * Conversations are Everyone (broadcast), each group this handheld belongs to, and each
 * handheld it has heard about. The chat view shows what was sent and received with the
 * delivery state of our own messages, and the keyboard takes the lower half of the screen
 * (decision D8). Messages and sending go through hh_service.h alone (D27); sizes, fonts,
 * and colours come from the theme, and positions are fractions of the screen (D9, D10).
 */
#include "ui_screen.h"
#include "ui_snapshot.h"
#include "ui_chat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_display.h"
#include "lg_emoji.h"
#include "lg_envelope.h"
#include "lg_theme.h"
#include "lg_ui_screensaver.h"
#include "lg_ui_widgets.h"
#include "ui_launcher.h"
#include "ui_notify.h"

static const char *TAG = "UI";

#define REFRESH_MS   500
#define CONVS_MAX    (LG_MAX_DEVICES + LG_MAX_GROUPS + 1)
#define TITLE_MAX    40

typedef struct {
    uint8_t  scope;      /* LG_SCOPE_BROADCAST, LG_SCOPE_GROUP, LG_SCOPE_DIRECT */
    uint32_t target;     /* group id or device index */
    char     title[TITLE_MAX];
} conv_t;

/* What each drawn bubble is showing, so a refresh can touch only what moved. */
typedef struct {
    uint32_t  id;
    uint8_t   state;
    uint8_t   reject;
    /* Counts are tracked too, or a group message whose delivery count moved would keep the
     * figure it was drawn with: the state alone does not change when the fourth person
     * receives it (D29, D42). */
    uint8_t   delivered_count;
    uint8_t   read_count;
    lv_obj_t *row;
    lv_obj_t *note;   /* timestamp, author, or the words of a refusal */
    lv_obj_t *mark;   /* the state glyph, drawn one size up so its shape reads */
} shown_msg_t;

static struct {
    lv_obj_t   *list_screen;
    lv_obj_t   *list_rows;
    lv_obj_t   *chat_screen;
    lv_obj_t   *chat_title;
    lv_obj_t   *chat_hint;
    lv_obj_t   *chat_rows;
    lv_obj_t   *input;
    lv_obj_t   *urgent_button;
    lv_obj_t   *urgent_label;
    lv_obj_t   *keyboard;
    lv_obj_t   *keyboard_label;
    lv_obj_t   *controls;
    conv_t      convs[CONVS_MAX];
    uint8_t     n_convs;
    conv_t      open;
    bool        urgent;
    bool        in_chat;
    uint32_t    shown_status;
    uint32_t    shown_messages;
    shown_msg_t shown[HH_MESSAGES];
    uint8_t     shown_count;
    lv_obj_t   *empty_note;      /* the "nothing here yet" line, taken away when one arrives */
    uint8_t     t9_key;          /* keypad key being tapped, T9_NONE once the pause has passed */
    uint8_t     t9_tap;          /* how far through that key's letters we are */
    lv_timer_t *t9_timer;        /* ends the run of taps after a pause, as SMS keypads did */
} s_ui;

/* ---- helpers ---- */

static void clock_text(uint32_t grid_time, char *out, size_t cap)
{
    if (grid_time == 0) {
        snprintf(out, cap, "no time");
        return;
    }
    uint32_t day = grid_time % 86400u;
    snprintf(out, cap, "%02u:%02u", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u));
}

static bool in_conversation(const hh_message_t *m, const conv_t *c)
{
    if (m->scope != c->scope) {
        return false;
    }
    switch (c->scope) {
    case LG_SCOPE_BROADCAST: return true;
    case LG_SCOPE_GROUP:     return m->target == c->target;
    default:                 return m->mine ? m->target == c->target : m->author == c->target;
    }
}

static const char *person_name(const hh_status_t *st, uint32_t device)
{
    for (uint8_t i = 0; i < st->n_people; i++) {
        if (st->people[i].device == device) {
            return st->people[i].name;
        }
    }
    return "unknown handheld";
}

/*
 * One icon per state, not one tick repeated (D42). The clock is the emoji font's U+1F553,
 * already in the committed subset, so nothing new is embedded to draw it. It is drawn at
 * font_icon rather than font_tiny: the emoji font has exactly one size, 20 px, so a clock
 * asked for inside a 10 px line would either overflow the row or not draw at all.
 */
#define MARK_WAIT      "\xF0\x9F\x95\x93"   /* U+1F553, a clock face: this handheld still has it */
#define MARK_NODE      LV_SYMBOL_UPLOAD     /* a node took it, perhaps to store for someone offline */
#define MARK_DELIVERED LV_SYMBOL_DOWNLOAD   /* it reached the recipient's handheld */
#define MARK_READ      LV_SYMBOL_EYE_OPEN   /* that handheld showed it to its reader */
/*
 * The same eye, named separately where it sits inside a count ("3/4  eye 1") rather than
 * alone as the state glyph. One name used at two sizes is how a row ends up with one glyph
 * towering over its own digits: the count is font_tiny, the state glyph is font_icon.
 */
#define COUNT_READ     LV_SYMBOL_EYE_OPEN
/*
 * One state, one glyph, one colour role. Ticks were abandoned for a measured reason: two of
 * them differing only in colour could not work here, because `accent` and `success` are the
 * same value in the theme, so delivered and read were being drawn identically -- and the
 * second tick appearing on delivery read as "they have seen it", which is the opposite of
 * what it meant.
 *
 * Colours come from marker roles rather than borrowed ones (D10, D42). Read is a violet,
 * deliberately outside this theme's greens, so the two states cannot collapse into each
 * other again at marker size.
 *
 * A refusal is not in this table: no icon can say "that handheld is offline", so a refusal
 * keeps its words and the caller clears the glyph.
 */
static const char *state_marker(const hh_message_t *m, lv_color_t *colour)
{
    const lg_theme_t *t = lg_theme();
    switch (m->state) {
    case HH_MSG_PENDING:   *colour = t->mark_wait;      return MARK_WAIT;
    case HH_MSG_ACCEPTED:  *colour = t->mark_node;      return MARK_NODE;
    case HH_MSG_DELIVERED: *colour = t->mark_delivered; return MARK_DELIVERED;
    case HH_MSG_READ:      *colour = t->mark_read;      return MARK_READ;
    default:               *colour = t->error;          return LV_SYMBOL_WARNING;
    }
}

/*
 * A group or broadcast message counts people instead of naming a state (D42). One recipient
 * has a state; "food is ready" to a group only means anything as how many have it and how
 * many have opened it. The counts collapse to "all read" once everybody has, so a number
 * never sits there implying a missing reader when there is none.
 *
 * Broadcasts stop at the delivered count: no node reports having finished handing one out,
 * and the grid cannot know how many handhelds are switched on, so a confirmation count is
 * the honest thing to show rather than a completion.
 */
static uint8_t group_total(const hh_status_t *st, const hh_message_t *m)
{
    if (m->scope != LG_SCOPE_GROUP) {
        return 0;
    }
    for (uint8_t i = 0; i < st->n_groups; i++) {
        if (st->groups[i].id == (uint16_t)m->target) {
            /* Everyone but the sender, who does not report delivery to themselves. */
            return st->groups[i].members > 0 ? (uint8_t)(st->groups[i].members - 1u) : 0u;
        }
    }
    return 0;
}

static void count_text(const hh_status_t *st, const hh_message_t *m, char *out, size_t cap)
{
    out[0] = '\0';
    if (!m->mine || m->scope == LG_SCOPE_DIRECT || m->state == HH_MSG_REJECTED ||
        m->state == HH_MSG_REFUSED) {
        return;
    }
    uint8_t total = group_total(st, m);
    if (total > 0 && m->read_count >= total) {
        snprintf(out, cap, "all read");
        return;
    }
    if (total > 0) {
        snprintf(out, cap, "%u/%u", (unsigned)m->delivered_count, (unsigned)total);
    } else if (m->delivered_count > 0) {
        snprintf(out, cap, "%u", (unsigned)m->delivered_count);   /* broadcast: no denominator */
    }
    if (m->read_count > 0 && out[0] != '\0') {
        size_t used = lv_strlen(out);
        snprintf(out + used, cap - used, "  %s %u", COUNT_READ, (unsigned)m->read_count);
    }
}

/* ---- chat screen ---- */

static lv_obj_t *add_message_row(lv_obj_t *parent, const hh_message_t *m, const hh_status_t *st,
                                 lv_obj_t **note_out, lv_obj_t **mark_out)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *card = lg_ui_column(row, 0);
    lv_obj_set_width(card, LV_PCT(85));
    lv_obj_set_style_bg_color(card, t->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, m->urgent ? t->warning : t->outline, 0);
    lv_obj_set_style_border_width(card, m->urgent ? t->stroke : t->hairline, 0);
    lv_obj_set_style_radius(card, t->radius, 0);
    lv_obj_set_style_pad_all(card, t->gap, 0);

    lg_ui_label(card, t->font_body, t->text, m->text);

    /*
     * The note is a row rather than one label, because the state glyph and the words beside
     * it are drawn at different sizes: the emoji font has exactly one size, 20 px, so a clock
     * asked for inside a 10 px line would not draw. Timestamp and counts stay at font_tiny;
     * only the glyph is font_icon (D42).
     */
    char clock[8];
    clock_text(m->grid_time, clock, sizeof(clock));
    char note[72];
    char counts[32];
    lv_color_t note_colour = m->urgent ? t->warning : t->muted;
    bool refused = m->mine && (m->state == HH_MSG_REJECTED || m->state == HH_MSG_REFUSED);
    if (refused) {
        /* A refusal keeps its words: no icon can say "that handheld is offline". */
        snprintf(note, sizeof(note), "%s %s", LV_SYMBOL_WARNING, hh_message_state_text(m));
        note_colour = t->error;
    } else if (m->mine) {
        snprintf(note, sizeof(note), "%s", clock);
    } else if (m->scope == LG_SCOPE_DIRECT) {
        snprintf(note, sizeof(note), "%s, %s", clock, person_name(st, m->author));
    } else {
        snprintf(note, sizeof(note), "%s, %s%s", clock, person_name(st, m->author), m->urgent ? ", urgent" : "");
    }
    /* Counts join the note's own text rather than becoming a third label: one label per
     * thing that changes, so a moving count rewrites a string instead of rebuilding a row. */
    count_text(st, m, counts, sizeof(counts));
    if (counts[0] != '\0') {
        size_t used = lv_strlen(note);
        snprintf(note + used, sizeof(note) - used, "  %s", counts);
    }

    lv_obj_t *foot = lg_ui_row(card);
    lv_obj_set_style_pad_column(foot, t->gap / 2, 0);
    lv_obj_t *note_label = lg_ui_text(foot, t->font_tiny, note_colour, note);
    lv_obj_t *mark_label = NULL;
    if (m->mine && !refused) {
        lv_color_t mark_colour;
        const char *glyph = state_marker(m, &mark_colour);
        mark_label = lg_ui_text(foot, t->font_icon, mark_colour, glyph);
    }
    if (note_out != NULL) {
        *note_out = note_label;
    }
    if (mark_out != NULL) {
        *mark_out = mark_label;
    }
    return row;
}

/*
 * The note under our own bubbles is the only part that changes after it is drawn: waiting
 * becomes on-the-grid, then delivered, then read. Two labels are rewritten -- the words and
 * the glyph, which live apart because they are drawn at different sizes -- so LVGL repaints
 * one line rather than rebuilding a bubble (D29, D42).
 */
static void update_message_note(shown_msg_t *shown, const hh_message_t *m, const hh_status_t *st)
{
    shown->state = m->state;
    shown->reject = m->reject;
    shown->delivered_count = m->delivered_count;
    shown->read_count = m->read_count;
    if (!m->mine) {
        return;   /* a received bubble's note never changes */
    }
    const lg_theme_t *t = lg_theme();
    char clock[8];
    char note[72];
    char counts[32];
    lv_color_t colour;
    clock_text(m->grid_time, clock, sizeof(clock));
    if (m->state == HH_MSG_REJECTED || m->state == HH_MSG_REFUSED) {
        snprintf(note, sizeof(note), "%s %s", LV_SYMBOL_WARNING, hh_message_state_text(m));
        colour = t->error;
        /* A message can become refused after it was drawn with a glyph, and a refusal has
         * no glyph: leaving the old one would put a down arrow beside "that handheld is
         * offline". The words carry a refusal on their own. */
        if (shown->mark != NULL) {
            lg_ui_set_text(shown->mark, "");
        }
    } else {
        snprintf(note, sizeof(note), "%s", clock);
        count_text(st, m, counts, sizeof(counts));
        if (counts[0] != '\0') {
            size_t used = lv_strlen(note);
            snprintf(note + used, sizeof(note) - used, "  %s", counts);
        }
        colour = m->urgent ? t->warning : t->muted;
        lv_color_t mark_colour;
        const char *glyph = state_marker(m, &mark_colour);
        if (shown->mark != NULL) {
            lg_ui_set_text(shown->mark, glyph);
            lv_obj_set_style_text_color(shown->mark, mark_colour, 0);
        }
    }
    lg_ui_set_text(shown->note, note);
    lv_obj_set_style_text_color(shown->note, colour, 0);
}

/*
 * Draws only what moved. New messages are appended, a delivery state that changed rewrites
 * one note, and nothing else is touched: rebuilding every bubble twice a second repainted
 * the whole list to show the same words, which is what the owner saw as a flickering screen.
 * The list is rebuilt from scratch only when the conversation changes or the oldest message
 * has fallen out of the service's ring.
 */
static void refresh_messages(const hh_status_t *st, bool force)
{
    size_t n = 0;
    const hh_message_t *msgs = ui_messages(&n);

    /* Oldest first, only this conversation's messages. */
    const hh_message_t *mine[HH_MESSAGES];
    size_t count = 0;
    for (size_t i = n; i > 0; i--) {
        const hh_message_t *m = &msgs[i - 1];
        if (in_conversation(m, &s_ui.open)) {
            mine[count++] = m;
        }
    }

    bool rebuild = force || s_ui.shown_count > count;
    for (uint8_t i = 0; !rebuild && i < s_ui.shown_count; i++) {
        if (s_ui.shown[i].id != mine[i]->id) {
            rebuild = true;   /* the ring dropped something, so the rows no longer line up */
        }
    }
    if (rebuild) {
        lv_obj_clean(s_ui.chat_rows);
        s_ui.shown_count = 0;
        s_ui.empty_note = NULL;   /* the clean took it away with the rows */
    }
    if (count == 0 && s_ui.empty_note == NULL) {
        s_ui.empty_note = lg_ui_label(s_ui.chat_rows, lg_theme()->font_small, lg_theme()->muted,
                                      "No messages here yet.");
    }

    for (uint8_t i = 0; i < s_ui.shown_count; i++) {
        const hh_message_t *m = mine[i];
        if (s_ui.shown[i].state != m->state || s_ui.shown[i].reject != m->reject ||
            s_ui.shown[i].delivered_count != m->delivered_count ||
            s_ui.shown[i].read_count != m->read_count) {
            update_message_note(&s_ui.shown[i], m, st);
        }
    }
    /* What is on this screen has been shown to its reader, so the author may mark it read. */
    for (size_t i = 0; i < count; i++) {
        /* Groups report reads too, or a group's read count could never leave zero (D42).
         * Broadcasts do not: a report per handheld on the grid for every announcement is
         * traffic nobody asked for, and the marker stops at a delivered count anyway.
         *
         * And only while the panel is actually showing this conversation. The screen saver
         * draws on LVGL's top layer, so the chat screen underneath stays the active screen
         * and this loop went on reporting messages read while the panel showed nothing but
         * rain -- the sender saw a read marker for a message its reader could not possibly
         * have seen. Nothing is lost by waiting: read_sent is still false, so the report
         * goes out on the first refresh after somebody clears the saver and can see it. */
        if (!mine[i]->mine && !mine[i]->read_sent && !lg_ui_screensaver_showing() &&
            (mine[i]->scope == LG_SCOPE_DIRECT || mine[i]->scope == LG_SCOPE_GROUP)) {
            hh_service_mark_read(mine[i]->id);
        }
    }
    if (count > 0 && s_ui.empty_note != NULL) {
        lv_obj_delete(s_ui.empty_note);   /* messages are here, so the line is no longer true */
        s_ui.empty_note = NULL;
    }
    bool appended = false;
    for (size_t i = s_ui.shown_count; i < count && s_ui.shown_count < HH_MESSAGES; i++) {
        shown_msg_t *shown = &s_ui.shown[s_ui.shown_count];
        shown->id = mine[i]->id;
        shown->state = mine[i]->state;
        shown->reject = mine[i]->reject;
        /* Recorded at creation, or the first refresh would compare a real count against
         * zero and rewrite the note on every tick (D29). */
        shown->delivered_count = mine[i]->delivered_count;
        shown->read_count = mine[i]->read_count;
        shown->row = add_message_row(s_ui.chat_rows, mine[i], st, &shown->note, &shown->mark);
        s_ui.shown_count++;
        appended = true;
    }
    if (appended) {
        lv_obj_scroll_to_view(lv_obj_get_child(s_ui.chat_rows, -1), LV_ANIM_OFF);
        lv_obj_update_layout(s_ui.chat_screen);
        ESP_LOGI(TAG, "[UI] Chat list: %u message(s), area %dx%d px", (unsigned)s_ui.shown_count,
                 (int)lv_obj_get_width(s_ui.chat_rows), (int)lv_obj_get_height(s_ui.chat_rows));
    }
}

static void update_hint(const hh_status_t *st)
{
    const lg_theme_t *t = lg_theme();
    bool urgent_only = st->time_restricted;
    if (st->link != HH_LINK_ONLINE) {
        lg_ui_set_text(s_ui.chat_hint, "Not on the grid: messages wait until this handheld joins an AP.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->error, 0);
    } else if (urgent_only && s_ui.open.scope == LG_SCOPE_BROADCAST) {
        lg_ui_set_text(s_ui.chat_hint, "Grid time is not set: only urgent broadcasts can be sent.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->warning, 0);
    } else if (urgent_only) {
        lg_ui_set_text(s_ui.chat_hint, "Grid time is not set: use Everyone with Urgent on, or ask the admin to set time.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->warning, 0);
    } else {
        lg_ui_set_text(s_ui.chat_hint, "");
    }
    /* An empty hint still takes a line, which the message list needs. */
    if (lv_strlen(lv_label_get_text(s_ui.chat_hint)) == 0) {
        lv_obj_add_flag(s_ui.chat_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui.chat_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.urgent_button != NULL) {
        if (s_ui.open.scope == LG_SCOPE_BROADCAST) {
            lv_obj_remove_flag(s_ui.urgent_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.urgent_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void send_now(void)
{
    const char *text = lv_textarea_get_text(s_ui.input);
    if (text == NULL || text[0] == '\0') {
        return;
    }
    bool urgent = s_ui.urgent && s_ui.open.scope == LG_SCOPE_BROADCAST;
    esp_err_t err = hh_service_send(s_ui.open.scope, s_ui.open.target, urgent, text);
    if (err == ESP_OK) {
        lv_textarea_set_text(s_ui.input, "");
        lg_bsp_audio_cue(LG_CUE_SENT);   /* it left this handheld; the marker says what happens next */
    } else {
        lv_label_set_text(s_ui.chat_hint, err == ESP_ERR_INVALID_ARG ? "That message is empty or too long."
                                                                     : "Too many messages waiting; try again.");
        lv_obj_set_style_text_color(s_ui.chat_hint, lg_theme()->error, 0);
    }
    ESP_LOGI(TAG, "[UI] Send tapped: scope %u target %" PRIu32 " urgent %d -> %s", s_ui.open.scope, s_ui.open.target,
             (int)urgent, esp_err_to_name(err));
}

static void on_send(lv_event_t *e)
{
    (void)e;
    send_now();
}


static void on_urgent(lv_event_t *e)
{
    (void)e;
    s_ui.urgent = !s_ui.urgent;
    /* The icon stays; its colour is the state, amber for on and grey for off. */
    lv_obj_set_style_text_color(s_ui.urgent_label, s_ui.urgent ? lg_theme()->warning : lg_theme()->muted, 0);
}

/*
 * The emoji pages (decision D8). Twenty-four glyphs per page in rows of six, so a key is
 * about a third of an inch tall on this panel; 48 emoji in one page gave 14 px keys, which
 * no fingertip can hit. "More" turns the page and "ABC" goes back to letters.
 *
 * LVGL's own key handler would type "More" into the message, so this file installs its own
 * (on_key) and reproduces the built-in behaviour it needs. The set comes from
 * tools/build_emoji_font.py, and the theme's font falls back to the emoji font to draw it.
 */
#define EMOJI_PER_ROW  6
#define EMOJI_PER_PAGE 24
#define EMOJI_PAGES    ((LG_EMOJI_COUNT + EMOJI_PER_PAGE - 1) / EMOJI_PER_PAGE)
#define EMOJI_MAP_MAX  (EMOJI_PER_PAGE + (EMOJI_PER_PAGE / EMOJI_PER_ROW) + 6)
#define EMOJI_KEY_MORE "More"
#define EMOJI_KEY_ABC  "ABC"
/*
 * Putting the keyboard away needs a key of its own on every page, this one included. Raising
 * the keys hides the controls row, and the button that raised them lives in that row -- so
 * once they are up, the keyboard itself is the only thing that can dismiss them. Letters and
 * the keypad had a cross for this; punctuation and the emoji pages had nothing, so from those
 * there was no way back at all (owner, 2026-09-16).
 *
 * A down chevron rather than a cross: it is already what the controls row's own button turns
 * into while the keys are up, and a cross reads as cancel or delete beside a text field.
 *
 * Defined here rather than with the letter keys below, because the emoji page builder is
 * above them and uses it.
 */
#define KEY_HIDE  LV_SYMBOL_DOWN

static const char            *s_emoji_map[EMOJI_PAGES][EMOJI_MAP_MAX];
static lv_buttonmatrix_ctrl_t s_emoji_ctrl[EMOJI_PAGES][EMOJI_MAP_MAX];

static void build_emoji_pages(lv_obj_t *keyboard)
{
    static const lv_keyboard_mode_t modes[EMOJI_PAGES] = { LV_KEYBOARD_MODE_USER_1, LV_KEYBOARD_MODE_USER_2 };
    for (size_t page = 0; page < EMOJI_PAGES; page++) {
        size_t k = 0;
        size_t buttons = 0;
        size_t first = page * EMOJI_PER_PAGE;
        for (size_t i = first; i < first + EMOJI_PER_PAGE && i < LG_EMOJI_COUNT; i++) {
            s_emoji_map[page][k++] = LG_EMOJI[i];
            s_emoji_ctrl[page][buttons++] = 1;
            if ((i - first + 1) % EMOJI_PER_ROW == 0) {
                s_emoji_map[page][k++] = "\n";
            }
        }
        s_emoji_map[page][k++] = EMOJI_KEY_ABC;
        s_emoji_ctrl[page][buttons++] = 2 | LV_KEYBOARD_CTRL_BUTTON_FLAGS;
        s_emoji_map[page][k++] = EMOJI_KEY_MORE;
        s_emoji_ctrl[page][buttons++] = 2 | LV_KEYBOARD_CTRL_BUTTON_FLAGS;
        s_emoji_map[page][k++] = LV_SYMBOL_BACKSPACE;
        s_emoji_ctrl[page][buttons++] = 1 | LV_KEYBOARD_CTRL_BUTTON_FLAGS;
        s_emoji_map[page][k++] = LV_SYMBOL_OK;
        s_emoji_ctrl[page][buttons++] = 1 | LV_KEYBOARD_CTRL_BUTTON_FLAGS;
        /* Every page can put the keyboard away, this one included: raising the keys hides the
         * controls row that holds the toggle, so without a key here there is no way back. */
        s_emoji_map[page][k++] = KEY_HIDE;
        s_emoji_ctrl[page][buttons++] = 1 | LV_KEYBOARD_CTRL_BUTTON_FLAGS;
        s_emoji_map[page][k] = "";
        lv_keyboard_set_map(keyboard, modes[page], s_emoji_map[page], s_emoji_ctrl[page]);
    }
}

/*
 * Letters, punctuation, and the phone keypad.
 *
 * LVGL's own letters page is ten columns over five rows, which on a 240 px panel is a 22 px
 * key: smaller than a fingertip. These maps take the digits and the punctuation off the
 * letters page and give punctuation its own, so letters are four rows and a key is about
 * 36 px tall. Ten columns is still 22 px wide, and no rearrangement changes that while the
 * layout is qwerty, so there is also a phone keypad: three columns, about 78 px per key,
 * typing the way SMS keypads did, by tapping a key until the letter comes up.
 *
 * Mode keys are icons or two characters wide, so the keys that carry letters get the room.
 */
/*
 * Putting the keyboard away needs a key of its own on every page. Raising it hides the
 * controls row, and the button that raised it lives in that row -- so once the keys are up,
 * the keyboard itself is the only thing that can dismiss them. Letters and the keypad had a
 * cross for this; punctuation and the emoji pages had nothing, so from those there was no way
 * back at all (owner, 2026-09-16).
 *
 * A down chevron rather than a cross: it is already what the controls row's own button turns
 * into while the keys are up, and a cross reads as cancel or delete next to a text field.
 */
#define KEY_SHIFT LV_SYMBOL_UP
#define KEY_PUNCT ".,?"
#define KEY_PHONE "123"
#define KEY_SPACE " "
#define CTRL_KEY(w) ((lv_buttonmatrix_ctrl_t)((w) | LV_KEYBOARD_CTRL_BUTTON_FLAGS))

static const char *const LOWER_MAP[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    KEY_SHIFT, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    KEY_PHONE, KEY_PUNCT, KEY_SPACE, LV_SYMBOL_OK, KEY_HIDE, ""
};
static const char *const UPPER_MAP[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    KEY_SHIFT, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    KEY_PHONE, KEY_PUNCT, KEY_SPACE, LV_SYMBOL_OK, KEY_HIDE, ""
};
static const lv_buttonmatrix_ctrl_t LETTERS_CTRL[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    CTRL_KEY(1), 1, 1, 1, 1, 1, 1, 1, CTRL_KEY(1),
    CTRL_KEY(2), CTRL_KEY(2), 5, CTRL_KEY(2), CTRL_KEY(2)
};

static const char *const PUNCT_MAP[] = {
    ".", ",", "?", "!", ":", ";", "\n",
    "'", "\"", "(", ")", "-", "_", "\n",
    "@", "#", "/", "&", "+", "=", "\n",
    EMOJI_KEY_ABC, KEY_SPACE, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, KEY_HIDE, ""
};
static const lv_buttonmatrix_ctrl_t PUNCT_CTRL[] = {
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    CTRL_KEY(2), 2, CTRL_KEY(1), CTRL_KEY(1), CTRL_KEY(1)
};

/* Each keypad key carries a label and the characters it steps through. */
#define PHONE_KEYS  10
#define T9_NONE     0xFFu
#define T9_PAUSE_MS 900   /* long enough to find the next key, short enough not to wait on it */

static const char *const PHONE_LABEL[PHONE_KEYS] = {
    "1 .,?", "2 abc", "3 def", "4 ghi", "5 jkl", "6 mno", "7 pqrs", "8 tuv", "9 wxyz", "0 space"
};
static const char *const PHONE_CYCLE[PHONE_KEYS] = {
    ".,?1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", " 0"
};
static const char *const PHONE_MAP[] = {
    "1 .,?", "2 abc", "3 def", "\n",
    "4 ghi", "5 jkl", "6 mno", "\n",
    "7 pqrs", "8 tuv", "9 wxyz", "\n",
    KEY_PUNCT, "0 space", LV_SYMBOL_BACKSPACE, "\n",
    EMOJI_KEY_ABC, LV_SYMBOL_OK, KEY_HIDE, ""
};
static const lv_buttonmatrix_ctrl_t PHONE_CTRL[] = {
    1, 1, 1,
    1, 1, 1,
    1, 1, 1,
    CTRL_KEY(1), 1, CTRL_KEY(1),
    CTRL_KEY(1), CTRL_KEY(1), CTRL_KEY(1)
};

static void build_keyboard_pages(lv_obj_t *kb)
{
    build_emoji_pages(kb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, LOWER_MAP, LETTERS_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, UPPER_MAP, LETTERS_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_3, PUNCT_MAP, PUNCT_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_4, PHONE_MAP, PHONE_CTRL);
}

/*
 * A 320 px panel cannot show messages, the controls, the field, and a keyboard at once.
 * While the keyboard is up the controls row goes away, and each page asks for the height its
 * rows need: four rows for letters and punctuation, five for emoji and the keypad. The list
 * keeps its one-bubble floor, so it never collapses to nothing.
 */
#define KB_LETTERS_PCT 46
#define KB_PUNCT_PCT   46
#define KB_EMOJI_PCT   52
#define KB_PHONE_PCT   56

static int keyboard_height_pct(void)
{
    switch (lv_keyboard_get_mode(s_ui.keyboard)) {
    case LV_KEYBOARD_MODE_USER_1:
    case LV_KEYBOARD_MODE_USER_2: return KB_EMOJI_PCT;
    case LV_KEYBOARD_MODE_USER_3: return KB_PUNCT_PCT;
    case LV_KEYBOARD_MODE_USER_4: return KB_PHONE_PCT;
    default:                      return KB_LETTERS_PCT;
    }
}

/* ---- the keypad's multi-tap, the way an SMS phone did it ---- */

static void t9_forget(lv_timer_t *timer)
{
    (void)timer;
    s_ui.t9_key = T9_NONE;   /* the pause ended the run, so the next tap starts a new letter */
}

static void t9_reset(void)
{
    s_ui.t9_key = T9_NONE;
    if (s_ui.t9_timer != NULL) {
        lv_timer_pause(s_ui.t9_timer);
    }
}

/* Types one keypad key, and says whether it was one. */
static bool t9_type(lv_obj_t *ta, const char *txt)
{
    uint8_t key = T9_NONE;
    for (uint8_t i = 0; i < PHONE_KEYS; i++) {
        if (lv_strcmp(txt, PHONE_LABEL[i]) == 0) {
            key = i;
            break;
        }
    }
    if (key == T9_NONE) {
        return false;
    }
    const char *cycle = PHONE_CYCLE[key];
    if (s_ui.t9_key == key) {
        lv_textarea_delete_char(ta);   /* the same key again steps the letter on */
        s_ui.t9_tap = (uint8_t)((s_ui.t9_tap + 1u) % lv_strlen(cycle));
    } else {
        s_ui.t9_key = key;
        s_ui.t9_tap = 0;
    }
    char one[2] = { cycle[s_ui.t9_tap], '\0' };
    lv_textarea_add_text(ta, one);
    if (s_ui.t9_timer == NULL) {
        s_ui.t9_timer = lv_timer_create(t9_forget, T9_PAUSE_MS, NULL);
        /* Not deleted when its one run ends, or the reset below and the delete on leaving the chat
         * would touch freed memory (the banner timer crashed both handhelds that way). */
        lv_timer_set_auto_delete(s_ui.t9_timer, false);
    }
    lv_timer_set_repeat_count(s_ui.t9_timer, 1);
    lv_timer_reset(s_ui.t9_timer);
    lv_timer_resume(s_ui.t9_timer);
    return true;
}

static void keyboard_show(bool show)
{
    if (s_ui.keyboard == NULL) {
        return;
    }
    if (show) {
        lv_obj_set_height(s_ui.keyboard, LV_PCT(keyboard_height_pct()));
        lv_obj_remove_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.controls, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.controls, LV_OBJ_FLAG_HIDDEN);
    }
    lg_ui_set_text(s_ui.keyboard_label, show ? LV_SYMBOL_DOWN : LV_SYMBOL_KEYBOARD);
}

/* Our own key handler: LVGL's would type "More" into the message. It reproduces the
 * built-in keys this keyboard uses and adds the emoji pages. */
static void on_key(lv_event_t *e)
{
    (void)e;
    lv_obj_t *kb = s_ui.keyboard;
    lv_obj_t *ta = s_ui.input;
    const char *txt = lv_keyboard_get_button_text(kb, lv_keyboard_get_selected_button(kb));
    if (txt == NULL || ta == NULL) {
        return;
    }
    if (lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_USER_4 && t9_type(ta, txt)) {
        return;   /* a keypad letter, and the run of taps stays open */
    }
    t9_reset();
    if (lv_strcmp(txt, EMOJI_KEY_MORE) == 0) {
        lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
        lv_keyboard_set_mode(kb, mode == LV_KEYBOARD_MODE_USER_1 ? LV_KEYBOARD_MODE_USER_2
                                                                 : LV_KEYBOARD_MODE_USER_1);
    } else if (lv_strcmp(txt, EMOJI_KEY_ABC) == 0 || lv_strcmp(txt, "abc") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        keyboard_show(true);
    } else if (lv_strcmp(txt, KEY_SHIFT) == 0) {
        lv_keyboard_set_mode(kb, lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER
                                     ? LV_KEYBOARD_MODE_TEXT_LOWER
                                     : LV_KEYBOARD_MODE_TEXT_UPPER);
        keyboard_show(true);
    } else if (lv_strcmp(txt, KEY_PUNCT) == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_3);
        keyboard_show(true);
    } else if (lv_strcmp(txt, KEY_PHONE) == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_4);
        keyboard_show(true);
    } else if (lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(ta);
    } else if (lv_strcmp(txt, LV_SYMBOL_OK) == 0 || lv_strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
        send_now();
        keyboard_show(false);
    } else if (lv_strcmp(txt, KEY_HIDE) == 0 || lv_strcmp(txt, LV_SYMBOL_CLOSE) == 0 ||
               lv_strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        keyboard_show(false);   /* every page carries KEY_HIDE; CLOSE stays for older maps */
    } else if (lv_strcmp(txt, LV_SYMBOL_LEFT) == 0) {
        lv_textarea_cursor_left(ta);
    } else if (lv_strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
        lv_textarea_cursor_right(ta);
    } else {
        lv_textarea_add_text(ta, txt);
    }
}

static void on_keyboard_toggle(lv_event_t *e)
{
    (void)e;
    keyboard_show(lv_obj_has_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN));
}

static void on_input_tapped(lv_event_t *e)
{
    (void)e;
    keyboard_show(true);
}

static void on_emoji_page(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_mode(s_ui.keyboard, LV_KEYBOARD_MODE_USER_1);
    keyboard_show(true);
}

/* Control keys need no magnifier: the glyph is a word, and the bubble would hide the row. */
static bool key_is_control(const char *txt)
{
    return lv_strcmp(txt, EMOJI_KEY_ABC) == 0 || lv_strcmp(txt, EMOJI_KEY_MORE) == 0 ||
           lv_strcmp(txt, "abc") == 0 || lv_strcmp(txt, KEY_SPACE) == 0 ||
           lv_strcmp(txt, KEY_SHIFT) == 0 || lv_strcmp(txt, KEY_PUNCT) == 0 ||
           lv_strcmp(txt, KEY_PHONE) == 0 || lv_strcmp(txt, KEY_HIDE) == 0 ||
           lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0 || lv_strcmp(txt, LV_SYMBOL_NEW_LINE) == 0 ||
           lv_strcmp(txt, LV_SYMBOL_OK) == 0 || lv_strcmp(txt, LV_SYMBOL_CLOSE) == 0 ||
           lv_strcmp(txt, LV_SYMBOL_KEYBOARD) == 0 || lv_strcmp(txt, LV_SYMBOL_LEFT) == 0 ||
           lv_strcmp(txt, LV_SYMBOL_RIGHT) == 0;
}

static void on_key_held(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_keyboard_get_button_text(s_ui.keyboard, lv_keyboard_get_selected_button(s_ui.keyboard));
    lv_indev_t *indev = lv_indev_active();
    if (txt == NULL || indev == NULL || key_is_control(txt)) {
        lg_ui_keycap_hide();
        return;
    }
    /* On a keypad key the label is "2 abc", so magnify the character it typed instead. */
    char one[2];
    for (uint8_t i = 0; i < PHONE_KEYS; i++) {
        if (lv_strcmp(txt, PHONE_LABEL[i]) == 0) {
            one[0] = s_ui.t9_key == i ? PHONE_CYCLE[i][s_ui.t9_tap] : PHONE_LABEL[i][0];
            one[1] = '\0';
            txt = one;
            break;
        }
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lg_ui_keycap_show(txt, p.x, p.y);
}

static void on_key_let_go(lv_event_t *e)
{
    (void)e;
    lg_ui_keycap_hide();
}

static void on_back_to_list(lv_event_t *e)
{
    (void)e;
    ui_chat_open_list();   /* the list was freed when the chat replaced it; this builds it again */
}

static void on_back_home(lv_event_t *e)
{
    (void)e;
    s_ui.in_chat = false;
    ui_launcher_open();
}

/* The chat screen is being freed (ui_screen.h): drop every pointer into it. The conversation
 * list, the open conversation, and the Urgent choice are state, not widgets, and stay. */
static void forget_chat(void)
{
    lg_ui_keycap_hide();
    if (s_ui.t9_timer != NULL) {
        lv_timer_delete(s_ui.t9_timer);
        s_ui.t9_timer = NULL;
    }
    s_ui.t9_key = T9_NONE;
    s_ui.chat_screen = NULL;
    s_ui.chat_title = NULL;
    s_ui.chat_hint = NULL;
    s_ui.chat_rows = NULL;
    s_ui.input = NULL;
    s_ui.urgent_button = NULL;
    s_ui.urgent_label = NULL;
    s_ui.keyboard = NULL;
    s_ui.keyboard_label = NULL;
    s_ui.controls = NULL;
    s_ui.empty_note = NULL;
    s_ui.shown_count = 0;
    s_ui.in_chat = false;
}

static void forget_list(void)
{
    s_ui.list_screen = NULL;
    s_ui.list_rows = NULL;
    s_ui.shown_status = 0;
}

static void refresh(lv_timer_t *timer);

/* One refresh timer for both screens, created on first use and kept: it checks which of
 * them, if either, is on the panel before touching anything. */
static void ensure_timer(void)
{
    static bool started;
    if (!started) {
        started = true;
        lv_timer_create(refresh, REFRESH_MS, NULL);
    }
}

static void build_chat_screen(void)
{
    const lg_theme_t *t = lg_theme();
    s_ui.chat_screen = lv_obj_create(NULL);
    ui_screen_free_on_leave(s_ui.chat_screen, forget_chat);
    lg_theme_apply_screen(s_ui.chat_screen);
    lv_obj_remove_flag(s_ui.chat_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.chat_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.chat_screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.chat_screen, t->gap, 0);

    lv_obj_t *head = lg_ui_row(s_ui.chat_screen);
    /* The name of whoever you are talking to is a label, not a headline: body size, so it
     * gives its room to the messages. Back is an arrow, a third of the width of the word. */
    s_ui.chat_title = lg_ui_text(head, t->font_body, t->accent, "Chat");
    lv_obj_set_flex_grow(s_ui.chat_title, 1);
    lv_label_set_long_mode(s_ui.chat_title, LV_LABEL_LONG_DOT);
    lg_ui_icon_button(head, LV_SYMBOL_LEFT, on_back_to_list, NULL);

    s_ui.chat_hint = lg_ui_label(s_ui.chat_screen, t->font_small, t->warning, "");

    /* The message list takes whatever the header, controls, entry, and keyboard leave.
     * A flex child cannot both grow and be content-sized: content-sizing wins and the list
     * collapses to nothing, which looks like messages never arriving. */
    s_ui.chat_rows = lg_ui_column(s_ui.chat_screen, t->gap);
    lv_obj_set_flex_grow(s_ui.chat_rows, 1);
    lv_obj_set_height(s_ui.chat_rows, LV_PCT(100));
    lv_obj_set_style_min_height(s_ui.chat_rows, t->touch_min, 0);   /* one bubble, so the column never overflows */
    lv_obj_add_flag(s_ui.chat_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.chat_rows, LV_DIR_VER);

    s_ui.controls = lg_ui_row(s_ui.chat_screen);
    lv_obj_t *controls = s_ui.controls;
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(controls, t->gap / 2, 0);
    /* Icons, so this row leaves the messages their height: a face for the emoji pages, a
     * keyboard for the keys, a house for home. Urgent stays a word: no icon says "off". */
    lg_ui_icon_button(controls, LG_EMOJI[0], on_emoji_page, NULL);   /* slight smile */
    lv_obj_t *keys = lg_ui_icon_button(controls, LV_SYMBOL_KEYBOARD, on_keyboard_toggle, NULL);
    s_ui.keyboard_label = lv_obj_get_child(keys, 0);
    s_ui.urgent_button = lg_ui_icon_button(controls, LV_SYMBOL_WARNING, on_urgent, NULL);
    s_ui.urgent_label = lv_obj_get_child(s_ui.urgent_button, 0);
    lv_obj_set_style_text_color(s_ui.urgent_label, t->muted, 0);
    lg_ui_icon_button(controls, LV_SYMBOL_HOME, on_back_home, NULL);

    lv_obj_t *entry = lg_ui_row(s_ui.chat_screen);
    s_ui.input = lv_textarea_create(entry);
    lv_textarea_set_one_line(s_ui.input, true);
    lv_textarea_set_max_length(s_ui.input, HH_TEXT_MAX);
    lv_textarea_set_placeholder_text(s_ui.input, "Message");
    lv_obj_set_flex_grow(s_ui.input, 1);
    lv_obj_set_style_min_height(s_ui.input, t->touch_min, 0);
    lv_obj_set_style_bg_color(s_ui.input, t->surface, 0);
    lv_obj_set_style_border_color(s_ui.input, t->outline, 0);
    lv_obj_set_style_border_width(s_ui.input, t->hairline, 0);
    lv_obj_set_style_text_color(s_ui.input, t->text, 0);
    lv_obj_set_style_text_font(s_ui.input, t->font_body, 0);
    lv_obj_add_event_cb(s_ui.input, on_input_tapped, LV_EVENT_CLICKED, NULL);
    lg_ui_icon_button(entry, LV_SYMBOL_OK, on_send, NULL);   /* an icon, so the field keeps the width */

    /* Decision D8: the keyboard may take the lower half. LVGL's own pages cover letters,
     * capitals, numbers, and symbols; emoji need a font this build does not carry yet. */
    s_ui.keyboard = lv_keyboard_create(s_ui.chat_screen);
    lv_keyboard_set_textarea(s_ui.keyboard, s_ui.input);
    lv_obj_set_height(s_ui.keyboard, LV_PCT(KB_LETTERS_PCT));
    lv_obj_set_style_bg_color(s_ui.keyboard, t->bg, 0);
    lv_obj_set_style_text_font(s_ui.keyboard, t->font_body, 0);   /* falls back to the emoji font */
    lv_obj_remove_event_cb(s_ui.keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_ui.keyboard, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ui.keyboard, on_key_held, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.keyboard, on_key_held, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_ui.keyboard, on_key_let_go, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ui.keyboard, on_key_let_go, LV_EVENT_PRESS_LOST, NULL);
    build_keyboard_pages(s_ui.keyboard);
    s_ui.t9_key = T9_NONE;   /* no run of taps is open yet */
    lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);   /* tap the field, Keys, or Emoji to raise it */
}

static void open_chat(const conv_t *conv)
{
    s_ui.open = *conv;
    s_ui.in_chat = true;
    if (s_ui.chat_screen == NULL) {
        build_chat_screen();
    }
    const hh_status_t *st = ui_status();
    lg_ui_set_text(s_ui.chat_title, conv->title);
    lv_textarea_set_text(s_ui.input, "");
    update_hint(st);
    refresh_messages(st, true);   /* a different conversation: start the list again */
    s_ui.shown_messages = st->messages_version;
    ui_notify_mark_seen(conv->scope, conv->target);
    lv_screen_load(s_ui.chat_screen);
    ESP_LOGI(TAG, "[UI] Chat opened: %s", conv->title);
}

/* Safe from any task: the display lock is recursive, so a call from an LVGL callback nests. */
void ui_chat_open_conversation(uint8_t scope, uint32_t target, const char *title)
{
    conv_t conv = { .scope = scope, .target = target };
    snprintf(conv.title, sizeof(conv.title), "%s", title != NULL ? title : "Chat");
    lg_display_lock(1000);
    ensure_timer();
    open_chat(&conv);
    lg_display_unlock();
}

bool ui_chat_conversation_open(uint8_t scope, uint32_t target)
{
    return s_ui.in_chat && s_ui.open.scope == scope &&
           (scope == LG_SCOPE_BROADCAST || s_ui.open.target == target);
}

/* ---- conversation list ---- */

static void on_conv_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index >= 0 && index < s_ui.n_convs) {
        open_chat(&s_ui.convs[index]);
    }
}

static void conv_row(const char *left, const char *right, int index)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *b = lv_button_create(s_ui.list_rows);
    lg_theme_style_button(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, t->touch_min, 0);
    lv_obj_set_style_pad_all(b, t->gap, 0);
    lv_obj_set_style_pad_column(b, t->gap, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lg_ui_text(b, t->font_body, t->text, left);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lg_ui_text(b, t->font_small, t->muted, right);
    lv_obj_add_event_cb(b, on_conv_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
}

static void rebuild_list(const hh_status_t *st)
{
    lv_obj_clean(s_ui.list_rows);
    s_ui.n_convs = 0;

    char right[40];
    conv_t *all = &s_ui.convs[s_ui.n_convs++];
    all->scope = LG_SCOPE_BROADCAST;
    all->target = LG_TARGET_ALL;
    snprintf(all->title, sizeof(all->title), "Everyone");
    uint32_t unread = ui_notify_unread(all->scope, all->target);
    if (unread > 0) {
        snprintf(right, sizeof(right), "%" PRIu32 " new", unread);
    } else {
        snprintf(right, sizeof(right), "%s", st->time_restricted ? "urgent only" : "broadcast");
    }
    conv_row(all->title, right, 0);

    for (uint8_t i = 0; i < st->n_groups && s_ui.n_convs < CONVS_MAX; i++) {
        if (!st->groups[i].member) {
            continue;   /* a non-member cannot send to or read that group */
        }
        conv_t *c = &s_ui.convs[s_ui.n_convs];
        c->scope = LG_SCOPE_GROUP;
        c->target = st->groups[i].id;
        snprintf(c->title, sizeof(c->title), "%s", st->groups[i].name);
        uint32_t group_unread = ui_notify_unread(c->scope, c->target);
        if (group_unread > 0) {
            snprintf(right, sizeof(right), "%" PRIu32 " new", group_unread);
        } else {
            snprintf(right, sizeof(right), "group");
        }
        conv_row(c->title, right, s_ui.n_convs);
        s_ui.n_convs++;
    }

    uint8_t people = 0;
    for (uint8_t i = 0; i < st->n_people && s_ui.n_convs < CONVS_MAX; i++) {
        conv_t *c = &s_ui.convs[s_ui.n_convs];
        c->scope = LG_SCOPE_DIRECT;
        c->target = st->people[i].device;
        snprintf(c->title, sizeof(c->title), "%s", st->people[i].name);
        uint32_t person_unread = ui_notify_unread(c->scope, c->target);
        if (person_unread > 0) {
            snprintf(right, sizeof(right), "%" PRIu32 " new", person_unread);
        } else {
            snprintf(right, sizeof(right), "%s", st->people[i].online ? "online, encrypted" : "offline");
        }
        conv_row(c->title, right, s_ui.n_convs);
        s_ui.n_convs++;
        people++;
    }
    if (people == 0) {
        lg_ui_label(s_ui.list_rows, lg_theme()->font_small, lg_theme()->muted,
                    "No other handheld seen yet, so there is no one to message directly.");
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_t *active = lv_screen_active();
    if (active == NULL || (active != s_ui.chat_screen && active != s_ui.list_screen)) {
        return;   /* neither is on the panel; a freed screen is NULL and never matches */
    }
    const hh_status_t *st = ui_status();
    if (active == s_ui.chat_screen) {
        if (st->messages_version != s_ui.shown_messages) {
            refresh_messages(st, false);
            s_ui.shown_messages = st->messages_version;
            ui_notify_mark_seen(s_ui.open.scope, s_ui.open.target);
        }
        update_hint(st);
    } else if (st->version != s_ui.shown_status) {
        rebuild_list(st);
        s_ui.shown_status = st->version;
    }
}

void ui_chat_open_list(void)
{
    const lg_theme_t *t = lg_theme();
    lg_display_lock(1000);   /* recursive: safe whether the caller holds it or not */
    if (s_ui.list_screen == NULL) {
        s_ui.list_screen = lv_obj_create(NULL);
        lg_theme_apply_screen(s_ui.list_screen);
        lv_obj_set_flex_flow(s_ui.list_screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(s_ui.list_screen, t->pad, 0);
        lv_obj_set_style_pad_row(s_ui.list_screen, t->gap * 2, 0);

        lv_obj_t *head = lg_ui_row(s_ui.list_screen);
        lg_ui_text(head, t->font_title, t->accent, "Messages");
        lg_ui_icon_button(head, LV_SYMBOL_HOME, on_back_home, NULL);

        s_ui.list_rows = lg_ui_column(s_ui.list_screen, t->gap);
        ui_screen_free_on_leave(s_ui.list_screen, forget_list);
    }
    ensure_timer();
    const hh_status_t *st = ui_status();
    rebuild_list(st);
    s_ui.shown_status = st->version;
    s_ui.in_chat = false;
    lv_screen_load(s_ui.list_screen);
    lg_display_unlock();
}
