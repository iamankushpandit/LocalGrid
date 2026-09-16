/*
 * Conversation list and chat screens (P6).
 *
 * Conversations are Everyone (broadcast), each group this handheld belongs to, and each
 * handheld it has heard about. The chat view shows what was sent and received with the
 * delivery state of our own messages, and the keyboard takes the lower half of the screen
 * (decision D8). Messages and sending go through hh_service.h alone (D27); sizes, fonts,
 * and colours come from the theme, and positions are fractions of the screen (D9, D10).
 */
#include "ui_chat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_display.h"
#include "lg_emoji.h"
#include "lg_envelope.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "ui_home.h"
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

/* ---- chat screen ---- */

static void add_message_row(lv_obj_t *parent, const hh_message_t *m, const hh_status_t *st)
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

    char clock[8];
    clock_text(m->grid_time, clock, sizeof(clock));
    char note[72];
    if (m->mine) {
        snprintf(note, sizeof(note), "%s, %s", clock, hh_message_state_text(m));
    } else if (m->scope == LG_SCOPE_DIRECT) {
        snprintf(note, sizeof(note), "%s, %s", clock, person_name(st, m->author));
    } else {
        snprintf(note, sizeof(note), "%s, %s%s", clock, person_name(st, m->author), m->urgent ? ", urgent" : "");
    }
    lg_ui_label(card, t->font_small, m->urgent ? t->warning : t->muted, note);
}

static void rebuild_messages(const hh_status_t *st)
{
    static hh_message_t msgs[HH_MESSAGES];
    size_t n = hh_service_messages(msgs, HH_MESSAGES);
    lv_obj_clean(s_ui.chat_rows);
    size_t shown = 0;
    for (size_t i = n; i > 0; i--) {   /* the list arrives newest first; show oldest at the top */
        const hh_message_t *m = &msgs[i - 1];
        if (in_conversation(m, &s_ui.open)) {
            add_message_row(s_ui.chat_rows, m, st);
            shown++;
        }
    }
    if (shown == 0) {
        lg_ui_label(s_ui.chat_rows, lg_theme()->font_small, lg_theme()->muted, "No messages here yet.");
    }
    lv_obj_scroll_to_view(lv_obj_get_child(s_ui.chat_rows, -1), LV_ANIM_OFF);
    lv_obj_update_layout(s_ui.chat_screen);
    ESP_LOGI(TAG, "[UI] Chat list: %u of %u message(s) shown, area %dx%d px", (unsigned)shown, (unsigned)n,
             (int)lv_obj_get_width(s_ui.chat_rows), (int)lv_obj_get_height(s_ui.chat_rows));
}

static void update_hint(const hh_status_t *st)
{
    const lg_theme_t *t = lg_theme();
    bool urgent_only = st->time_restricted;
    if (st->link != HH_LINK_ONLINE) {
        lv_label_set_text(s_ui.chat_hint, "Not on the grid: messages wait until this handheld joins a node.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->error, 0);
    } else if (urgent_only && s_ui.open.scope == LG_SCOPE_BROADCAST) {
        lv_label_set_text(s_ui.chat_hint, "Grid time is not set: only urgent broadcasts can be sent.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->warning, 0);
    } else if (urgent_only) {
        lv_label_set_text(s_ui.chat_hint, "Grid time is not set: use Everyone with Urgent on, or ask the admin to set time.");
        lv_obj_set_style_text_color(s_ui.chat_hint, t->warning, 0);
    } else {
        lv_label_set_text(s_ui.chat_hint, "");
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
    lv_label_set_text(s_ui.urgent_label, s_ui.urgent ? "Urgent ON" : "Urgent off");
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
        s_emoji_map[page][k] = "";
        lv_keyboard_set_map(keyboard, modes[page], s_emoji_map[page], s_emoji_ctrl[page]);
    }
}

/*
 * A 320 px panel cannot show messages, four controls, the field, and a keyboard at once.
 * While the keyboard is up the controls row goes away and the keyboard keeps to 40% of the
 * panel (52% for the taller emoji pages), which leaves the message list about two bubbles
 * and keeps the field and Send visible. With the keyboard down the list has the panel.
 */
#define KB_LETTERS_PCT 40
#define KB_EMOJI_PCT   52

static bool keyboard_is_emoji(void)
{
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(s_ui.keyboard);
    return mode == LV_KEYBOARD_MODE_USER_1 || mode == LV_KEYBOARD_MODE_USER_2;
}

static void keyboard_show(bool show)
{
    if (s_ui.keyboard == NULL) {
        return;
    }
    if (show) {
        lv_obj_set_height(s_ui.keyboard, LV_PCT(keyboard_is_emoji() ? KB_EMOJI_PCT : KB_LETTERS_PCT));
        lv_obj_remove_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.controls, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.controls, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_ui.keyboard_label, show ? "Hide keys" : "Keys");
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
    if (lv_strcmp(txt, EMOJI_KEY_MORE) == 0) {
        lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
        lv_keyboard_set_mode(kb, mode == LV_KEYBOARD_MODE_USER_1 ? LV_KEYBOARD_MODE_USER_2
                                                                 : LV_KEYBOARD_MODE_USER_1);
    } else if (lv_strcmp(txt, "abc") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        keyboard_show(true);
    } else if (lv_strcmp(txt, EMOJI_KEY_ABC) == 0) {
        /* Shift on the letters page; on an emoji page it returns to letters. */
        lv_keyboard_set_mode(kb, keyboard_is_emoji() ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
        keyboard_show(true);
    } else if (lv_strcmp(txt, "1#") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
    } else if (lv_strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(ta);
    } else if (lv_strcmp(txt, LV_SYMBOL_OK) == 0 || lv_strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
        send_now();
        keyboard_show(false);
    } else if (lv_strcmp(txt, LV_SYMBOL_CLOSE) == 0 || lv_strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        keyboard_show(false);
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

static void on_back_to_list(lv_event_t *e)
{
    (void)e;
    s_ui.in_chat = false;
    lv_screen_load(s_ui.list_screen);
}

static void on_back_home(lv_event_t *e)
{
    (void)e;
    s_ui.in_chat = false;
    lv_screen_load(ui_home_screen());
}

static void build_chat_screen(void)
{
    const lg_theme_t *t = lg_theme();
    s_ui.chat_screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.chat_screen);
    lv_obj_remove_flag(s_ui.chat_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.chat_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.chat_screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.chat_screen, t->gap, 0);

    lv_obj_t *head = lg_ui_row(s_ui.chat_screen);
    s_ui.chat_title = lg_ui_text(head, t->font_title, t->accent, "Chat");
    lv_obj_set_flex_grow(s_ui.chat_title, 1);
    lv_label_set_long_mode(s_ui.chat_title, LV_LABEL_LONG_DOT);
    lg_ui_button(head, "Back", on_back_to_list, NULL);

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
    lg_ui_button_small(controls, "Emoji", on_emoji_page, NULL);
    lv_obj_t *keys = lg_ui_button_small(controls, "Keys", on_keyboard_toggle, NULL);
    s_ui.keyboard_label = lv_obj_get_child(keys, 0);
    s_ui.urgent_button = lg_ui_button_small(controls, "Urgent off", on_urgent, NULL);
    s_ui.urgent_label = lv_obj_get_child(s_ui.urgent_button, 0);
    lv_obj_set_style_text_color(s_ui.urgent_label, t->muted, 0);
    lg_ui_button_small(controls, "Home", on_back_home, NULL);

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
    lg_ui_button(entry, "Send", on_send, NULL);

    /* Decision D8: the keyboard may take the lower half. LVGL's own pages cover letters,
     * capitals, numbers, and symbols; emoji need a font this build does not carry yet. */
    s_ui.keyboard = lv_keyboard_create(s_ui.chat_screen);
    lv_keyboard_set_textarea(s_ui.keyboard, s_ui.input);
    lv_obj_set_height(s_ui.keyboard, LV_PCT(KB_LETTERS_PCT));
    lv_obj_set_style_bg_color(s_ui.keyboard, t->bg, 0);
    lv_obj_set_style_text_font(s_ui.keyboard, t->font_body, 0);   /* falls back to the emoji font */
    lv_obj_remove_event_cb(s_ui.keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_ui.keyboard, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    build_emoji_pages(s_ui.keyboard);
    lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);   /* tap the field, Keys, or Emoji to raise it */
}

static void open_chat(const conv_t *conv)
{
    s_ui.open = *conv;
    s_ui.in_chat = true;
    if (s_ui.chat_screen == NULL) {
        build_chat_screen();
    }
    static hh_status_t st;
    hh_service_status(&st);
    lv_label_set_text(s_ui.chat_title, conv->title);
    lv_textarea_set_text(s_ui.input, "");
    update_hint(&st);
    rebuild_messages(&st);
    s_ui.shown_messages = st.messages_version;
    ui_notify_mark_seen(conv->scope, conv->target);
    lv_screen_load(s_ui.chat_screen);
    ESP_LOGI(TAG, "[UI] Chat opened: %s", conv->title);
}

/* Safe from any task: the display lock is recursive, so a call from an LVGL callback nests. */
void ui_chat_open_conversation(uint8_t scope, uint32_t target, const char *title)
{
    conv_t conv = { .scope = scope, .target = target };
    snprintf(conv.title, sizeof(conv.title), "%s", title != NULL ? title : "Chat");
    if (s_ui.list_screen == NULL) {
        ui_chat_open_list();   /* builds the screens and the refresh timer */
    }
    lg_display_lock(1000);
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
    static hh_status_t st;
    hh_service_status(&st);
    if (s_ui.in_chat) {
        if (st.messages_version != s_ui.shown_messages) {
            rebuild_messages(&st);
            s_ui.shown_messages = st.messages_version;
            ui_notify_mark_seen(s_ui.open.scope, s_ui.open.target);
        }
        update_hint(&st);
    } else if (st.version != s_ui.shown_status) {
        rebuild_list(&st);
        s_ui.shown_status = st.version;
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
        lg_ui_button(head, "Home", on_back_home, NULL);

        s_ui.list_rows = lg_ui_column(s_ui.list_screen, t->gap);
        lv_timer_create(refresh, REFRESH_MS, NULL);
    }
    static hh_status_t st;
    hh_service_status(&st);
    rebuild_list(&st);
    s_ui.shown_status = st.version;
    s_ui.in_chat = false;
    lv_screen_load(s_ui.list_screen);
    lg_display_unlock();
}
