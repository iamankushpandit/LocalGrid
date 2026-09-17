/*
 * The Groups screen and the group editor (D52).
 *
 * Groups are their own place, a launcher tile beside Messages, not something tucked into the
 * conversation list: the owner asked for that. The Groups screen lists every group with its
 * members, marks the ones this handheld is not in (only members may change a group), and has +
 * for a new one. Tapping a group this handheld belongs to opens the editor.
 *
 * A name, a tick for each handheld in the roster, Save, and for an existing group Remove,
 * which asks for a second tap because it deletes the group's messages on every handheld. The
 * edit goes to the AP through hh_service_edit_group(); this screen waits for the answer, a new
 * groups_version or a refusal, and only then returns to the conversation list, so a refused
 * edit is never shown as done. Sizes, fonts, and colours come from the theme (D9, D10).
 */
#include "ui_group.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_display.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "ui_launcher.h"
#include "ui_screen.h"
#include "ui_snapshot.h"

static const char *TAG = "UI";

#define REFRESH_MS      300
#define ANSWER_WAIT_MS  8000   /* an AP answers in well under a second; this is a lost session */
#define KEYBOARD_PCT    45

static struct {
    lv_obj_t *screen;
    lv_obj_t *name;
    lv_obj_t *keyboard;
    lv_obj_t *problem;
    lv_obj_t *save_label;
    lv_obj_t *remove_label;
    lv_obj_t *boxes[LG_MAX_DEVICES];
    uint32_t  box_device[LG_MAX_DEVICES];
    uint8_t   n_boxes;
    uint16_t  id;                 /* 0: a new group */
    uint32_t  self;
    bool      remove_armed;
    bool      waiting;
    uint32_t  waiting_since_ms;
    uint32_t  waiting_groups;     /* groups_version when the edit was sent */
    uint32_t  waiting_status;     /* status version when the edit was sent */
} s_ui;

/* The Groups screen: built when opened, freed when another screen replaces it (ui_screen.h). */
static struct {
    lv_obj_t *screen;
    lv_obj_t *rows;
    uint32_t  shown_groups;   /* groups_version drawn, so a new table redraws the list */
    uint32_t  shown_users;    /* handhelds offered, which also changes what a row says */
    bool      drawn;
} s_list;

static void set_problem(const char *text)
{
    if (s_ui.problem != NULL) {
        lg_ui_set_text(s_ui.problem, text);
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_group_open_list();
}

static void forget(void)
{
    memset(&s_ui, 0, sizeof(s_ui));   /* waiting ends too: an answer after leaving changes nothing here */
}

static void keyboard_show(bool show)
{
    if (s_ui.keyboard == NULL) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(s_ui.name, LV_STATE_FOCUSED);
    }
}

static void on_name_tapped(lv_event_t *e)
{
    (void)e;
    keyboard_show(true);
}

static void on_keyboard_done(lv_event_t *e)
{
    (void)e;
    keyboard_show(false);   /* the tick and the hide key both put it away */
}

static void send_edit(bool remove)
{
    const hh_status_t *st = ui_status();
    const char *name = lv_textarea_get_text(s_ui.name);
    uint32_t members = 0;
    for (uint8_t i = 0; i < s_ui.n_boxes; i++) {
        uint32_t dev = s_ui.box_device[i];
        if (lv_obj_has_state(s_ui.boxes[i], LV_STATE_CHECKED) && dev >= 1u && dev <= 32u) {
            members |= 1u << (dev - 1u);
        }
    }
    if (!remove) {
        size_t len = strlen(name);
        if (len == 0) {
            set_problem("Give the group a name");
            return;
        }
        if (len > HH_GROUP_NAME_MAX) {
            set_problem("Names are 15 letters at most");
            return;
        }
        if (s_ui.id != 0 && members == 0) {
            set_problem("Pick at least one handheld, or remove the group");
            return;
        }
        if (s_ui.id == 0 && st->n_groups >= LG_MAX_GROUPS) {
            set_problem("There are already 8 groups");
            return;
        }
    }
    esp_err_t err = hh_service_edit_group(s_ui.id, name, members, remove);
    if (err != ESP_OK) {
        set_problem(err == ESP_ERR_INVALID_ARG ? "Check the name" : "Busy; try again");
        return;
    }
    keyboard_show(false);
    s_ui.waiting = true;
    s_ui.waiting_since_ms = lv_tick_get();
    s_ui.waiting_groups = st->groups_version;
    s_ui.waiting_status = st->version;
    set_problem("Sending to the AP...");
    ESP_LOGI(TAG, "[UI] Group %s sent: id %u \"%s\"", remove ? "removal" : s_ui.id ? "change" : "creation", s_ui.id,
             name);
}

static void on_save(lv_event_t *e)
{
    (void)e;
    if (!s_ui.waiting) {
        send_edit(false);
    }
}

static void on_remove(lv_event_t *e)
{
    (void)e;
    if (s_ui.waiting) {
        return;
    }
    if (!s_ui.remove_armed) {
        s_ui.remove_armed = true;   /* removal deletes messages everywhere: one tap is not enough */
        lg_ui_set_text(s_ui.remove_label, "Tap again");
        set_problem("Removing deletes this group's messages on every handheld");
        return;
    }
    send_edit(true);
}

static void draw_list(const hh_status_t *st);
static void ensure_timer(void);

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_t *active = lv_screen_active();
    if (s_list.screen != NULL && active == s_list.screen) {
        const hh_status_t *st = ui_status();
        if (!s_list.drawn || st->groups_version != s_list.shown_groups || st->n_users != s_list.shown_users) {
            draw_list(st);   /* only when the table or the handhelds changed, so taps are not lost to redraws */
        }
        return;
    }
    if (s_ui.screen == NULL || active != s_ui.screen || !s_ui.waiting) {
        return;
    }
    const hh_status_t *st = ui_status();
    if (st->groups_version != s_ui.waiting_groups) {
        s_ui.waiting = false;
        ui_group_open_list();   /* the new table is here: the list shows the result */
        return;
    }
    if (st->version != s_ui.waiting_status && st->group_problem[0] != '\0') {
        s_ui.waiting = false;
        set_problem(st->group_problem);
        return;
    }
    if (lv_tick_elaps(s_ui.waiting_since_ms) > ANSWER_WAIT_MS) {
        s_ui.waiting = false;
        set_problem("No answer from the AP; try again");
    }
}

void ui_group_open(uint16_t id)
{
    if (!lg_display_lock(3000)) {
        /* Never draw without the lock: two tasks in LVGL at once corrupt its event list. */
        ESP_LOGW(TAG, "[UI] Display busy or not started; group editor not opened");
        return;
    }
    ensure_timer();
    const lg_theme_t *t = lg_theme();
    const hh_status_t *st = ui_status();
    const hh_group_t *group = NULL;
    for (uint8_t i = 0; i < st->n_groups; i++) {
        if (st->groups[i].id == id) {
            group = &st->groups[i];
        }
    }
    if (id != 0 && group == NULL) {
        lg_display_unlock();
        ui_group_open_list();   /* removed while the editor was being opened */
        return;
    }

    if (s_ui.screen != NULL) {
        /* Already open. Replacing it here would let the old screen's forget() wipe the new
         * one's state when it unloads. */
        lg_display_unlock();
        return;
    }
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.id = id;
    s_ui.self = st->device;
    s_ui.screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.screen);
    lv_obj_remove_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.screen, t->gap, 0);
    lv_obj_set_style_pad_row(s_ui.screen, t->gap, 0);

    lv_obj_t *head = lg_ui_row(s_ui.screen);
    lv_obj_t *title = lg_ui_text(head, t->font_body, t->accent, id == 0 ? "New group" : "Edit group");
    lv_obj_set_flex_grow(title, 1);
    lg_ui_icon_button(head, LV_SYMBOL_LEFT, on_back, NULL);

    s_ui.name = lv_textarea_create(s_ui.screen);
    lv_textarea_set_one_line(s_ui.name, true);
    lv_textarea_set_max_length(s_ui.name, HH_GROUP_NAME_MAX);
    lv_textarea_set_placeholder_text(s_ui.name, "Group name");
    lv_obj_set_width(s_ui.name, LV_PCT(100));
    lv_obj_set_style_min_height(s_ui.name, t->touch_min * 3 / 4, 0);
    lv_obj_set_style_pad_ver(s_ui.name, t->gap, 0);
    lv_obj_set_style_bg_color(s_ui.name, t->surface, 0);
    lv_obj_set_style_border_color(s_ui.name, t->outline, 0);
    lv_obj_set_style_border_width(s_ui.name, t->hairline, 0);
    lv_obj_set_style_text_color(s_ui.name, t->text, 0);
    lv_obj_set_style_text_font(s_ui.name, t->font_small, 0);
    lv_obj_add_event_cb(s_ui.name, on_name_tapped, LV_EVENT_CLICKED, NULL);
    if (group != NULL) {
        lv_textarea_set_text(s_ui.name, group->name);
    }

    lg_ui_text(s_ui.screen, t->font_small, t->muted, "Members");
    /* The member list takes what the header, buttons, and keyboard leave, and scrolls. */
    lv_obj_t *list = lg_ui_column(s_ui.screen, t->gap / 2);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_height(list, LV_PCT(100));
    lv_obj_set_style_min_height(list, t->touch_min, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (uint8_t i = 0; i < st->n_users && s_ui.n_boxes < LG_MAX_DEVICES; i++) {
        const hh_user_t *u = &st->users[i];
        lv_obj_t *box = lv_checkbox_create(list);
        char label[HH_NAME_MAX + 8];
        snprintf(label, sizeof(label), "%s%s", u->name, u->device == s_ui.self ? " (you)" : "");
        lv_checkbox_set_text(box, label);
        lv_obj_set_style_text_font(box, t->font_small, 0);
        lv_obj_set_style_text_color(box, t->text, 0);
        lv_obj_set_style_min_height(box, t->touch_min * 3 / 4, 0);
        lv_obj_set_style_border_color(box, t->accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(box, t->accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
        bool member = group != NULL && u->device >= 1u && u->device <= 32u &&
                      (group->member_devices & (1u << (u->device - 1u))) != 0;
        if (member || (id == 0 && u->device == s_ui.self)) {
            lv_obj_add_state(box, LV_STATE_CHECKED);
        }
        if (id == 0 && u->device == s_ui.self) {
            lv_obj_add_state(box, LV_STATE_DISABLED);   /* whoever makes a group is in it */
        }
        s_ui.boxes[s_ui.n_boxes] = box;
        s_ui.box_device[s_ui.n_boxes] = u->device;
        s_ui.n_boxes++;
    }

    s_ui.problem = lg_ui_label(s_ui.screen, t->font_small, t->warning, "");

    lv_obj_t *actions = lg_ui_row(s_ui.screen);
    lv_obj_t *save = lg_ui_button_small(actions, id == 0 ? "Make group" : "Save", on_save, NULL);
    s_ui.save_label = lv_obj_get_child(save, 0);
    if (id != 0) {
        lv_obj_t *remove = lg_ui_button_small(actions, "Remove", on_remove, NULL);
        lv_obj_set_style_border_color(remove, t->error, 0);
        s_ui.remove_label = lv_obj_get_child(remove, 0);
        lv_obj_set_style_text_color(s_ui.remove_label, t->error, 0);
    }

    s_ui.keyboard = lv_keyboard_create(s_ui.screen);
    lv_keyboard_set_textarea(s_ui.keyboard, s_ui.name);
    lv_obj_set_height(s_ui.keyboard, LV_PCT(KEYBOARD_PCT));
    lv_obj_set_style_bg_color(s_ui.keyboard, t->bg, 0);
    lv_obj_set_style_text_font(s_ui.keyboard, t->font_body, 0);
    lv_obj_add_event_cb(s_ui.keyboard, on_keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ui.keyboard, on_keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);   /* raised by tapping the name */

    ui_screen_free_on_leave(s_ui.screen, forget);
    lv_screen_load(s_ui.screen);
    lg_display_unlock();
    ESP_LOGI(TAG, "[UI] Group editor opened: %s", id == 0 ? "new group" : group->name);
}

/* ---- the Groups screen ---- */

static void ensure_timer(void)
{
    static bool started;
    if (!started) {
        started = true;
        lv_timer_create(refresh, REFRESH_MS, NULL);   /* kept for good; it checks for a live screen */
    }
}

static void on_group_tapped(lv_event_t *e)
{
    ui_group_open((uint16_t)(intptr_t)lv_event_get_user_data(e));
}

static void on_new_group(lv_event_t *e)
{
    (void)e;
    ui_group_open(0);
}

static void on_home(lv_event_t *e)
{
    (void)e;
    ui_launcher_open();
}

static void forget_list(void)
{
    memset(&s_list, 0, sizeof(s_list));
}

/* Member names for a row, from the handhelds this one knows; others by number, never invented. */
static void member_text(const hh_status_t *st, const hh_group_t *g, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint32_t dev = 1; dev <= 32u && used + 1 < cap; dev++) {
        if ((g->member_devices & (1u << (dev - 1u))) == 0) {
            continue;
        }
        const char *name = NULL;
        for (uint8_t u = 0; u < st->n_users; u++) {
            if (st->users[u].device == dev) {
                name = st->users[u].name;
            }
        }
        char number[16];
        if (name == NULL) {
            snprintf(number, sizeof(number), "Handheld %" PRIu32, dev);
            name = number;
        }
        int n = snprintf(out + used, cap - used, "%s%s", used ? ", " : "", dev == st->device ? "you" : name);
        used += n > 0 ? (size_t)n : 0u;
    }
}

static void draw_list(const hh_status_t *st)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_clean(s_list.rows);
    s_list.shown_groups = st->groups_version;
    s_list.shown_users = st->n_users;
    s_list.drawn = true;
    if (st->n_groups == 0) {
        lg_ui_label(s_list.rows, t->font_small, t->muted, "No groups yet. Tap + to make one.");
        return;
    }
    for (uint8_t i = 0; i < st->n_groups; i++) {
        const hh_group_t *g = &st->groups[i];
        lv_obj_t *b = lv_button_create(s_list.rows);
        lg_theme_style_button(b);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(b, t->touch_min, 0);
        lv_obj_set_style_pad_all(b, t->gap, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(b, t->gap / 2, 0);
        lg_ui_label(b, t->font_body, g->member ? t->text : t->muted, g->name);
        char members[160];
        member_text(st, g, members, sizeof(members));
        char line[200];
        snprintf(line, sizeof(line), "%s%s", members, g->member ? "" : "  (you are not in it)");
        lg_ui_label(b, t->font_small, t->muted, line);
        if (g->member) {
            lv_obj_add_event_cb(b, on_group_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)g->id);
        } else {
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);   /* only members may change a group */
            lv_obj_set_style_border_color(b, t->outline, 0);
        }
    }
}

void ui_group_open_list(void)
{
    if (!lg_display_lock(3000)) {
        /* Never draw without the lock: two tasks in LVGL at once corrupt its event list. */
        ESP_LOGW(TAG, "[UI] Display busy or not started; Groups not opened");
        return;
    }
    ensure_timer();
    const lg_theme_t *t = lg_theme();
    if (s_list.screen == NULL) {
        s_list.screen = lv_obj_create(NULL);
        lg_theme_apply_screen(s_list.screen);
        lv_obj_set_flex_flow(s_list.screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(s_list.screen, t->pad, 0);
        lv_obj_set_style_pad_row(s_list.screen, t->gap * 2, 0);

        lv_obj_t *head = lg_ui_row(s_list.screen);
        lv_obj_t *heading = lg_ui_text(head, t->font_title, t->accent, "Groups");
        lv_obj_set_flex_grow(heading, 1);
        lg_ui_icon_button(head, LV_SYMBOL_PLUS, on_new_group, NULL);
        lg_ui_icon_button(head, LV_SYMBOL_HOME, on_home, NULL);

        s_list.rows = lg_ui_column(s_list.screen, t->gap);
        ui_screen_free_on_leave(s_list.screen, forget_list);
    }
    draw_list(ui_status());
    lv_screen_load(s_list.screen);
    lg_display_unlock();
    ESP_LOGI(TAG, "[UI] Groups opened");
}
