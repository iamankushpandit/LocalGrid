/*
 * The list screens: the conversation list, Groups and the group editor, Status, and Settings
 * with Which AP, the rename screen, and touch calibration. Each rebuilds its rows from the status
 * snapshot and repaints only when what it shows changed, so a refresh never costs a repaint and a
 * tap is never lost to one.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hh_mem.h"
#include "hh_service.h"
#include "hh_voice.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_settings.h"
#include "lg_bsp_touch.h"
#include "lg_envelope.h"
#include "lg_selftest.h"
#include "ui_geo.h"
#include "ui_kb.h"
#include "ui_list.h"
#include "ui_nav.h"
#include "ui_overlay.h"
#include "ui_theme.h"

static const char *TAG = "UI";

static uint16_t s_w;
static uint16_t s_h;

/* A cheap signature over a string, so screens compare what they show without keeping a copy. */
static uint32_t mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

static uint32_t mix_str(uint32_t h, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        h = mix(h, (uint8_t)*p);
    }
    return h;
}

/* ---- conversations ---- */

typedef struct {
    uint8_t  scope;
    uint32_t target;
} conv_ref_t;

static struct {
    uint32_t   shown;
    conv_ref_t refs[SLIST_ROWS];
} s_convs;

static uint32_t convs_build(bool rebuild)
{
    const hh_status_t *st = ui_status();
    uint32_t sig = 2166136261u;
    sig = mix(mix(sig, st->time_restricted), ui_notify_unread_total());
    for (uint8_t i = 0; i < st->n_groups; i++) {
        sig = mix_str(mix(mix(sig, st->groups[i].id), st->groups[i].member), st->groups[i].name);
    }
    for (uint8_t i = 0; i < st->n_people; i++) {
        sig = mix_str(mix(mix(sig, st->people[i].device), st->people[i].online), st->people[i].name);
    }
    if (!rebuild) {
        return sig;
    }
    slist_begin("Messages", false, false);
    uint8_t n = 0;
    char value[SLIST_VALUE_MAX];
    uint32_t unread = ui_notify_unread(LG_SCOPE_BROADCAST, LG_TARGET_ALL);
    if (unread) {
        snprintf(value, sizeof(value), "%" PRIu32 " new", unread);
    } else {
        snprintf(value, sizeof(value), "%s", st->time_restricted ? "urgent only" : "broadcast");
    }
    s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_BROADCAST, LG_TARGET_ALL };
    slist_add(ROW_ACTION, "Everyone", value, 0, n++)->warn = unread > 0;
    for (uint8_t i = 0; i < st->n_groups && n < SLIST_ROWS - 2; i++) {
        if (st->groups[i].member) {
            s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_GROUP, st->groups[i].id };
            unread = ui_notify_unread(LG_SCOPE_GROUP, st->groups[i].id);
            if (unread) {
                snprintf(value, sizeof(value), "%" PRIu32 " new", unread);
            } else {
                snprintf(value, sizeof(value), "group");
            }
            slist_add(ROW_ACTION, st->groups[i].name, value, 0, n++)->warn = unread > 0;
        }
    }
    for (uint8_t i = 0; i < st->n_people && n < SLIST_ROWS - 2; i++) {
        s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_DIRECT, st->people[i].device };
        unread = ui_notify_unread(LG_SCOPE_DIRECT, st->people[i].device);
        if (unread) {
            snprintf(value, sizeof(value), "%" PRIu32 " new", unread);
        } else {
            snprintf(value, sizeof(value), "%s", st->people[i].online ? "online, encrypted" : "offline");
        }
        slist_add(ROW_ACTION, st->people[i].name, value, 0, n++)->warn = unread > 0;
    }
    if (st->n_people == 0) {
        slist_add(ROW_NOTE, NULL, "No other handheld seen yet, so there is no one to message directly.", -1, 0);
    }
    return sig;
}

/* Holding a person or group talks to them (D61), on a board with a microphone. */
static bool can_talk(void)
{
    hh_voice_state_t vs;
    hh_voice_state(&vs);
    return vs.can_talk;
}

static void mark_talk_rows(void)
{
    if (!can_talk()) {
        return;
    }
    for (uint8_t i = 0; i < slist_count(); i++) {
        slist_row_t *r = slist_row(i);
        if (r->kind == ROW_ACTION && r->id == 0 && r->arg >= 0 && r->arg < SLIST_ROWS) {
            r->talk = s_convs.refs[r->arg].scope == LG_SCOPE_DIRECT || s_convs.refs[r->arg].scope == LG_SCOPE_GROUP;
        }
    }
}

static void hold_talk(const slist_event_t *ev, uint8_t scope, uint32_t target)
{
    if (ev->type == SLIST_HOLD) {
        esp_err_t err = hh_voice_ptt_start(scope, target);
        ESP_LOGI("UI", "[UI] Hold to talk on a list row: %s", err == ESP_OK ? "talking" : esp_err_to_name(err));
        if (err != ESP_OK) {
            slist_hold_refused();
        }
    } else if (ev->type == SLIST_HOLD_END) {
        hh_voice_ptt_stop();
        ESP_LOGI("UI", "[UI] List row released: talk ended");
    }
}

void ui_convs_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_convs.shown = convs_build(true);
    mark_talk_rows();
    slist_show(w, h, 0, false);
}

void ui_convs_refresh(void)
{
    uint32_t sig = convs_build(false);
    if (sig != s_convs.shown) {
        s_convs.shown = convs_build(true);
        mark_talk_rows();
        slist_update();
    }
}

void ui_convs_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        ui_go(NAV_HOME, 0, 0, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 0 && ev.arg < SLIST_ROWS) {
        const conv_ref_t *c = &s_convs.refs[ev.arg];
        ui_go(NAV_CHAT, c->scope, c->target, slist_row(ev.index)->label);
    } else if ((ev.type == SLIST_HOLD || ev.type == SLIST_HOLD_END) && ev.arg >= 0 && ev.arg < SLIST_ROWS) {
        const conv_ref_t *c = &s_convs.refs[ev.arg];
        hold_talk(&ev, c->scope, c->target);
    }
}

/* ---- groups ---- */

static void member_names(const hh_status_t *st, const hh_group_t *g, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint32_t dev = 1; dev <= 32u && used + 1 < cap; dev++) {
        if ((g->member_devices & (1u << (dev - 1u))) == 0) {
            continue;
        }
        char number[16];
        const char *name = NULL;
        for (uint8_t u = 0; u < st->n_users; u++) {
            if (st->users[u].device == dev) {
                name = st->users[u].name;
            }
        }
        if (name == NULL) {
            snprintf(number, sizeof(number), "Handheld %" PRIu32, dev);
            name = number;
        }
        int n = snprintf(out + used, cap - used, "%s%s", used ? ", " : "", dev == st->device ? "you" : name);
        used += n > 0 ? (size_t)n : 0u;
    }
}

static struct {
    uint32_t shown;
} s_groups;

static uint32_t groups_build(bool rebuild)
{
    const hh_status_t *st = ui_status();
    uint32_t sig = mix(mix(2166136261u, st->groups_version), st->n_users);
    if (!rebuild) {
        return sig;
    }
    slist_begin("Groups", false, true);
    if (st->n_groups == 0) {
        slist_add(ROW_NOTE, NULL, "No groups yet. Tap + to make one.", -1, 0);
    }
    for (uint8_t i = 0; i < st->n_groups; i++) {
        const hh_group_t *g = &st->groups[i];
        slist_row_t *r = slist_add(ROW_ACTION, g->name, g->member ? "" : "not in it", 1, g->id);
        r->muted = !g->member;   /* only members may change a group */
        r->talk = g->member && can_talk();   /* and only members may talk to it (D61) */
        char members[SLIST_VALUE_MAX];
        member_names(st, g, members, sizeof(members));
        slist_add(ROW_NOTE, NULL, members, -1, 0);
    }
    return sig;
}

void ui_groups_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_groups.shown = groups_build(true);
    slist_show(w, h, 0, false);
}

void ui_groups_refresh(void)
{
    uint32_t sig = groups_build(false);
    if (sig != s_groups.shown) {
        s_groups.shown = groups_build(true);
        slist_update();
    }
}

void ui_groups_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        ui_go(NAV_HOME, 0, 0, NULL);
    } else if (ev.type == SLIST_PLUS) {
        ui_go(NAV_GROUP_EDIT, 0, 0, NULL);
    } else if (ev.type == SLIST_HOLD || ev.type == SLIST_HOLD_END) {
        hold_talk(&ev, LG_SCOPE_GROUP, (uint32_t)ev.arg);
    } else if (ev.type == SLIST_ROW && ev.id == 1) {
        ui_go(NAV_GROUP_EDIT, 0, (uint32_t)ev.arg, NULL);
    }
}

/* ---- group editor ---- */

enum { ED_NAME = 10, ED_MEMBER, ED_SAVE, ED_REMOVE, ED_PROBLEM };

#define ANSWER_WAIT_MS 8000

static struct {
    uint16_t id;
    char     name[HH_GROUP_NAME_MAX + 1];
    uint32_t members;      /* bit (device - 1) */
    bool     typing;
    bool     remove_armed;
    bool     waiting;
    uint32_t waiting_since;
    uint32_t waiting_groups;
    uint32_t waiting_status;
    char     problem[SLIST_VALUE_MAX];
} s_ed;

static void editor_build(void)
{
    const hh_status_t *st = ui_status();
    slist_begin(s_ed.id == 0 ? "New group" : "Edit group", true, false);
    slist_row_t *field = slist_add(ROW_FIELD, "Group name", s_ed.name, ED_NAME, 0);
    field->focused = s_ed.typing;
    if (!s_ed.typing) {
        slist_add(ROW_SECTION, "Members", NULL, -1, 0);
        for (uint8_t u = 0; u < st->n_users; u++) {
            uint32_t dev = st->users[u].device;
            char label[SLIST_LABEL_MAX];
            snprintf(label, sizeof(label), "%s%s", st->users[u].name, dev == st->device ? " (you)" : "");
            slist_row_t *r = slist_add(ROW_CHECK, label, NULL, ED_MEMBER, (int32_t)dev);
            r->checked = dev >= 1u && dev <= 32u && (s_ed.members & (1u << (dev - 1u))) != 0;
            r->muted = s_ed.id == 0 && dev == st->device;   /* whoever makes a group is in it */
        }
        slist_add(ROW_BUTTON, s_ed.id == 0 ? "Make group" : "Save", NULL, ED_SAVE, 0)->style = BTN_MAIN;
        if (s_ed.id != 0) {
            slist_add(ROW_BUTTON, s_ed.remove_armed ? "Tap again to remove" : "Remove", NULL, ED_REMOVE, 0)->style =
                BTN_DANGER;
        }
    }
    if (s_ed.problem[0] != '\0') {
        slist_add(ROW_NOTE, NULL, s_ed.problem, ED_PROBLEM, 0)->warn = true;
    }
}

static void editor_show(bool keep_scroll)
{
    editor_build();
    if (s_ed.typing) {
        ui_kb_open(s_w, s_h, s_ed.name, sizeof(s_ed.name));
        slist_show(s_w, s_h, ui_kb_height(), false);
        ui_kb_draw();
    } else {
        slist_show(s_w, s_h, 0, keep_scroll);
    }
}

void ui_group_edit_open(uint16_t w, uint16_t h, uint16_t id)
{
    s_w = w;
    s_h = h;
    memset(&s_ed, 0, sizeof(s_ed));
    s_ed.id = id;
    const hh_status_t *st = ui_status();
    if (id == 0) {
        if (st->device >= 1u && st->device <= 32u) {
            s_ed.members = 1u << (st->device - 1u);
        }
    } else {
        for (uint8_t i = 0; i < st->n_groups; i++) {
            if (st->groups[i].id == id) {
                snprintf(s_ed.name, sizeof(s_ed.name), "%s", st->groups[i].name);
                s_ed.members = st->groups[i].member_devices;
            }
        }
    }
    editor_show(false);
}

static void set_problem(const char *text)
{
    snprintf(s_ed.problem, sizeof(s_ed.problem), "%s", text);
    editor_build();
    slist_update();
}

static void send_edit(bool remove)
{
    const hh_status_t *st = ui_status();
    if (!remove) {
        size_t len = strlen(s_ed.name);
        if (len == 0) {
            set_problem("Give the group a name.");
            return;
        }
        if (len > HH_GROUP_NAME_MAX) {
            set_problem("Names are 15 letters at most; emoji count as 4.");
            return;
        }
        if (s_ed.id != 0 && s_ed.members == 0) {
            set_problem("Pick at least one handheld, or remove the group.");
            return;
        }
        if (s_ed.id == 0 && st->n_groups >= LG_MAX_GROUPS) {
            set_problem("There are already 8 groups.");
            return;
        }
    }
    esp_err_t err = hh_service_edit_group(s_ed.id, s_ed.name, s_ed.members, remove);
    if (err != ESP_OK) {
        set_problem(err == ESP_ERR_INVALID_ARG ? "Check the name." : "Busy; try again.");
        return;
    }
    s_ed.waiting = true;
    s_ed.waiting_since = (uint32_t)(esp_timer_get_time() / 1000);
    s_ed.waiting_groups = st->groups_version;
    s_ed.waiting_status = st->version;
    set_problem("Sending to the AP...");
    ESP_LOGI(TAG, "[UI] Group %s sent: id %u", remove ? "removal" : s_ed.id ? "change" : "creation", s_ed.id);
}

void ui_group_edit_refresh(uint32_t now_ms)
{
    if (!s_ed.waiting) {
        return;
    }
    const hh_status_t *st = ui_status();
    if (st->groups_version != s_ed.waiting_groups) {
        s_ed.waiting = false;
        ui_go(NAV_GROUPS, 0, 0, NULL);   /* the new table is here: Groups shows the result */
    } else if (st->version != s_ed.waiting_status && st->group_problem[0] != '\0') {
        s_ed.waiting = false;
        set_problem(st->group_problem);
    } else if (now_ms - s_ed.waiting_since > ANSWER_WAIT_MS) {
        s_ed.waiting = false;
        set_problem("No answer from the AP; try again.");
    }
}

void ui_group_edit_touch(int16_t x, int16_t y, bool down)
{
    if (s_ed.typing) {
        kb_event_t kev;
        if (ui_kb_touch(x, y, down, &kev)) {
            if (kev == KB_TEXT_CHANGED) {
                snprintf(slist_row(0)->value, SLIST_VALUE_MAX, "%s", s_ed.name);
                slist_repaint_row(0);
            } else if (kev == KB_HIDE) {
                s_ed.typing = false;
                editor_show(false);
            }
            return;
        }
    }
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        if (s_ed.typing) {
            s_ed.typing = false;
            editor_show(false);
        } else {
            ui_go(NAV_GROUPS, 0, 0, NULL);
        }
        return;
    }
    if (ev.type != SLIST_ROW || s_ed.waiting) {
        return;
    }
    switch (ev.id) {
    case ED_NAME:
        if (!s_ed.typing) {
            s_ed.typing = true;
            editor_show(false);
        }
        break;
    case ED_MEMBER: {
        uint32_t dev = (uint32_t)ev.arg;
        if (dev >= 1u && dev <= 32u) {
            s_ed.members ^= 1u << (dev - 1u);
            slist_row(ev.index)->checked = (s_ed.members & (1u << (dev - 1u))) != 0;
            slist_repaint_row(ev.index);
        }
        break;
    }
    case ED_SAVE:
        send_edit(false);
        break;
    case ED_REMOVE:
        if (!s_ed.remove_armed) {
            s_ed.remove_armed = true;   /* removal deletes messages everywhere: one tap is not enough */
            set_problem("Removing deletes this group's messages on every handheld.");
        } else {
            send_edit(true);
        }
        break;
    default:
        break;
    }
}

/* ---- settings ---- */

enum { SET_TAB_GRID, SET_TAB_SOUND, SET_TAB_SCREEN, SET_TAB_DEVICE };
enum { SET_WHICH_AP = 20, SET_RECONNECT, SET_SCAN, SET_VOLUME, SET_TEST, SET_SAVER, SET_CALIBRATE, SET_NAME,
       SET_RESTART, SET_TALK, SET_THEME, SET_CLEAR_MSGS };

static const char *const SET_TABS[] = { "Grid", "Sound", "Screen", "Device" };
static const char *const VOLUME_NAMES[LG_VOLUME_STEPS] = { "Off", "Low", "Med", "High" };
static const char *const TALK_NAMES[LG_TALK_STEPS] = { "Normal", "Loud", "Louder" };

static struct {
    uint8_t  tab;
    uint32_t shown;
    bool     restart_armed;
    bool     clear_armed;
} s_set;

static uint32_t settings_build(bool rebuild)
{
    const hh_status_t *st = ui_status();
    uint32_t sig = mix(mix(mix(2166136261u, s_set.tab), st->link), (uint32_t)(st->preferred_node + 2));
    sig = mix_str(mix(sig, st->grid_time != 0), st->node_ssid);
    sig = mix(mix(sig, lg_bsp_audio_volume()), lg_bsp_setting_get_bool("saver", true));
    sig = mix(sig, lg_bsp_audio_talk_boost());
    sig = mix(mix_str(sig, st->name), lg_bsp_touch_needs_calibration());
    sig = mix(mix(mix(sig, st->free_heap / 4096u), s_set.restart_armed), s_set.clear_armed);
    if (!rebuild) {
        return sig;
    }
    char value[SLIST_VALUE_MAX];
    slist_begin("Settings", false, false);
    slist_tabs(SET_TABS, 4, s_set.tab);
    switch (s_set.tab) {
    case SET_TAB_GRID:
        slist_add(ROW_ACTION, "Which AP", st->preferred_node < 0 ? "automatic" : "fixed", SET_WHICH_AP, 0);
        slist_add(ROW_BUTTON, "Reconnect", NULL, SET_RECONNECT, 0)->style = BTN_MAIN;
        slist_add(ROW_BUTTON, "Look for APs", NULL, SET_SCAN, 0)->style = BTN_PLAIN;
        snprintf(value, sizeof(value), "%s", st->link == HH_LINK_ONLINE ? st->node_ssid : "not connected");
        slist_add(ROW_FACT, "Connected to", value, -1, 0);
        slist_add(ROW_FACT, "Grid time", st->grid_time != 0 ? "set" : "not set", -1, 0)->warn = st->grid_time == 0;
        slist_add(ROW_NOTE, NULL, "Grid time is set on any AP's admin page; a handheld never sets it.", -1, 0);
        break;
    case SET_TAB_SOUND:
        if (lg_bsp_audio_available()) {
            slist_row_t *r = slist_add(ROW_CHOICE, "Volume", NULL, SET_VOLUME, 0);
            r->options = VOLUME_NAMES;
            r->n_options = LG_VOLUME_STEPS;
            r->selected = lg_bsp_audio_volume();
            r = slist_add(ROW_CHOICE, "Talk loudness", NULL, SET_TALK, 0);
            r->options = TALK_NAMES;
            r->n_options = LG_TALK_STEPS;
            r->selected = lg_bsp_audio_talk_boost();
            slist_add(ROW_BUTTON, "Test sound", NULL, SET_TEST, LG_CUE_RECEIVED)->style = BTN_MAIN;
            slist_add(ROW_BUTTON, "Test urgent", NULL, SET_TEST, LG_CUE_URGENT)->style = BTN_PLAIN;
            slist_add(ROW_NOTE, NULL, "A bell when a message arrives, and three notes for an urgent one.", -1, 0);
            slist_add(ROW_NOTE, NULL, "Off silences the handheld except for urgent broadcasts, which always sound.",
                      -1, 0);
        } else {
            slist_add(ROW_FACT, "Volume", "no speaker on this board", -1, 0);
        }
        break;
    case SET_TAB_SCREEN:
        /* Daylight first: it is the row someone reaches for while squinting at the screen outside. */
        slist_add(ROW_ACTION, "Theme", ui_theme_name(ui_theme_kind()), SET_THEME, 0);
        slist_add(ROW_ACTION, "Screen saver", lg_bsp_setting_get_bool("saver", true) ? "on" : "off", SET_SAVER, 0);
        if (lg_bsp_touch_can_calibrate()) {
            slist_row_t *r = slist_add(ROW_ACTION, "Calibrate touch",
                                       lg_bsp_touch_needs_calibration() ? "needed" : "", SET_CALIBRATE, 0);
            r->warn = lg_bsp_touch_needs_calibration();
        } else {
            slist_add(ROW_FACT, "Touch", "no calibration needed", -1, 0);
        }
        slist_add(ROW_NOTE, NULL, "The screen saver setting is kept; the rain itself is not drawn by this UI yet.", -1,
                  0);
        break;
    case SET_TAB_DEVICE:
        slist_add(ROW_ACTION, "Name", st->name, SET_NAME, 0);
        snprintf(value, sizeof(value), "%" PRIu32, st->device);
        slist_add(ROW_FACT, "Device number", value, -1, 0);
        snprintf(value, sizeof(value), "%" PRIu32 " KB free, %" PRIu32 " KB lowest", st->free_heap / 1024u,
                 st->min_free_heap / 1024u);
        slist_add(ROW_FACT, "Memory", value, -1, 0);
        if (st->psram_total > 0) {
            snprintf(value, sizeof(value), "%" PRIu32 " of %" PRIu32 " KB free", st->psram_free / 1024u,
                     st->psram_total / 1024u);
            slist_add(ROW_FACT, "PSRAM", value, -1, 0);
        }
        /* Above Restart, and armed the same way: clearing cannot be undone, and a stray tap on a
           handheld in a pocket should not wipe somebody's messages. */
        snprintf(value, sizeof(value), "%u held", (unsigned)hh_service_message_count());
        slist_add(ROW_BUTTON, s_set.clear_armed ? "Tap again to clear messages" : "Clear all messages",
                  s_set.clear_armed ? NULL : value, SET_CLEAR_MSGS, 0)->style = BTN_DANGER;
        slist_add(ROW_BUTTON, s_set.restart_armed ? "Tap again to restart" : "Restart", NULL, SET_RESTART, 0)->style =
            BTN_DANGER;
        break;
    default:
        break;
    }
    return sig;
}

void ui_settings_open(uint16_t w, uint16_t h, uint8_t tab)
{
    s_w = w;
    s_h = h;
    s_set.tab = tab < 4 ? tab : 0;
    s_set.restart_armed = false;
    s_set.clear_armed = false;
    s_set.shown = settings_build(true);
    slist_show(w, h, 0, false);
}

void ui_settings_refresh(void)
{
    uint32_t sig = settings_build(false);
    if (sig != s_set.shown) {
        s_set.shown = settings_build(true);
        slist_update();
    }
}

void ui_settings_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    switch (ev.type) {
    case SLIST_BACK:
        ui_go(NAV_HOME, 0, 0, NULL);
        return;
    case SLIST_TAB:
        if (ev.index != s_set.tab) {
            ui_settings_open(s_w, s_h, ev.index);
        }
        return;
    case SLIST_ROW:
        break;
    default:
        return;
    }
    switch (ev.id) {
    case SET_WHICH_AP:
        ui_go(NAV_WHICH_AP, 0, 0, NULL);
        break;
    case SET_RECONNECT:
        hh_service_reconnect();
        break;
    case SET_SCAN:
        hh_service_scan_now();
        break;
    case SET_VOLUME:
        lg_bsp_audio_set_volume((uint8_t)ev.arg);
        ui_settings_refresh();
        break;
    case SET_TALK:
        lg_bsp_audio_set_talk_boost((uint8_t)ev.arg);
        ui_settings_refresh();
        break;
    case SET_TEST:
        (void)lg_bsp_audio_cue((lg_cue_t)ev.arg);
        break;
    case SET_SAVER:
        (void)lg_bsp_setting_set_bool("saver", !lg_bsp_setting_get_bool("saver", true));
        ui_settings_refresh();
        break;
    case SET_THEME:
        /* ui_theme_set repaints the whole screen, so the change is seen at once rather than
           leaving this list in the colours it was drawn in. */
        ui_theme_set(ui_theme_kind() == UI_THEME_DAYLIGHT ? UI_THEME_NIGHT : UI_THEME_DAYLIGHT);
        break;
    case SET_CLEAR_MSGS:
        if (!s_set.clear_armed) {
            s_set.clear_armed = true;   /* the first tap only asks */
            ui_settings_refresh();
            break;
        }
        s_set.clear_armed = false;
        ESP_LOGI("UI", "[UI] Cleared %u message(s)", (unsigned)hh_service_clear_messages());
        ui_settings_refresh();
        break;
    case SET_CALIBRATE:
        ui_go(NAV_CALIBRATE, 0, 0, NULL);
        break;
    case SET_NAME:
        ui_go(NAV_RENAME, 0, 0, NULL);
        break;
    case SET_RESTART:
        if (!s_set.restart_armed) {
            s_set.restart_armed = true;
            ui_settings_refresh();
        } else {
            ESP_LOGI(TAG, "[UI] Restart asked from Settings");
            esp_restart();
        }
        break;
    default:
        break;
    }
}

/* ---- status ----
 *
 * The facts, then where things are (D65): this handheld's GPS and position, MAIN from here, and
 * the other handhelds whose positions are known, nearest in time first. It scrolls like any list.
 *
 * Status changes every second (the clock), so it is not rebuilt and repainted whole: the rows are
 * made again and compared with the ones shown. When every row keeps its kind and label, only the
 * rows whose value changed are repainted; a row coming, going, or moving lays the list out again.
 */

#define STATUS_NEARBY_MAX  8
#define STATUS_STALE_S     120   /* an older position says how old it is */

static struct {
    bool     building;   /* making the rows afresh, rather than comparing with those shown */
    bool     same;       /* comparing: every row so far has the kind and label shown */
    uint8_t  index;
    uint32_t dirty;      /* comparing: rows whose value changed, one bit each */
} s_stat;

_Static_assert(SLIST_ROWS <= 32, "the Status screen keeps a bit per row");

static void stat_put(slist_kind_t kind, const char *label, const char *value, bool warn)
{
    label = label != NULL ? label : "";
    value = value != NULL ? value : "";
    if (s_stat.building) {
        slist_add(kind, label, value, -1, 0)->warn = warn;
        return;
    }
    slist_row_t *r = slist_row(s_stat.index);
    uint8_t i = s_stat.index++;
    if (!s_stat.same || r == NULL || r->kind != (uint8_t)kind || strcmp(r->label, label) != 0) {
        s_stat.same = false;
        return;
    }
    if (strncmp(r->value, value, SLIST_VALUE_MAX - 1u) == 0 && r->warn == warn) {
        return;
    }
    if (kind == ROW_NOTE) {
        s_stat.same = false;   /* a note's height follows its text */
        return;
    }
    snprintf(r->value, sizeof(r->value), "%s", value);
    r->warn = warn;
    s_stat.dirty |= 1u << i;
}

static const char *link_word(hh_link_t link)
{
    switch (link) {
    case HH_LINK_SEARCHING:   return "searching";
    case HH_LINK_CONNECTING:  return "connecting";
    case HH_LINK_REGISTERING: return "registering";
    case HH_LINK_ONLINE:      return "online";
    default:                  return "stopped";
    }
}

typedef struct {
    uint8_t       person;   /* index in the status snapshot's people */
    hh_position_t pos;
} nearby_t;

/* How good a dilution of precision is, in the words GPS people use. */
static const char *dop_word(uint16_t dop_c)
{
    return dop_c <= 100u ? "excellent" : dop_c <= 200u ? "good" : dop_c <= 500u ? "moderate" : "poor";
}

/*
 * Everything this handheld's GPS says (D65), only on a board with one fitted: first whether it has
 * a lock, in words, then the numbers behind it. Eight rows, so Status stays inside SLIST_ROWS with
 * the Nearby list full.
 */
static void stat_gps(void)
{
    hh_gps_info_t g;
    if (!hh_service_gps_info(&g)) {
        return;   /* no GPS on this handheld: nothing to say */
    }
    char value[SLIST_VALUE_MAX];
    stat_put(ROW_SECTION, "GPS", NULL, false);
    /* D73: the GPS is read on the interval the grid sets, and the serial port is handed back in
     * between so the radios keep the memory. Say which of the three the reader is doing before
     * anything else, or "no fix" between readings reads as a fault. */
    if (!g.always) {
        if (g.phase == 0) {
            snprintf(value, sizeof(value), "reading now, every %u min", (unsigned)((g.interval_s + 30u) / 60u));
        } else if (g.next_in_s >= 60u) {
            snprintf(value, sizeof(value), "next in %lu min %lu s", (unsigned long)(g.next_in_s / 60u),
                     (unsigned long)(g.next_in_s % 60u));
        } else {
            snprintf(value, sizeof(value), "next in %lu s", (unsigned long)g.next_in_s);
        }
        stat_put(ROW_FACT, "Reading", value, false);
        if (g.phase != 0) {
            /* Nothing is being read, so the live numbers below would be stale. Show what the last
             * reading found, which is still where this handheld is, and what it bought. */
            if (g.has_pos) {
                ui_geo_coord_text(g.lat_u, g.lon_u, value, sizeof(value));
                stat_put(ROW_FACT, "Position", value, false);
            }
            if (g.utc != 0) {
                char age[16];
                ui_geo_age_text(g.fix_age_ms == UINT32_MAX ? 0u : g.fix_age_ms / 1000u, age, sizeof(age));
                snprintf(value, sizeof(value), "%s ago, %u satellites", age, g.used);
                stat_put(ROW_FACT, "Last fix", value, false);
            }
            if (g.freed_bytes != 0) {
                snprintf(value, sizeof(value), "port released, %lu bytes free", (unsigned long)g.freed_bytes);
                stat_put(ROW_FACT, "Between", value, false);
            }
            return;
        }
    }
    if (!g.talking) {
        stat_put(ROW_FACT, "Lock", "no data from the GPS: check its wiring", true);
        return;
    }
    if (g.fix) {
        snprintf(value, sizeof(value), "yes, %s%s", g.fix_type == 2u ? "2D" : "3D", g.corrected ? " + WAAS" : "");
    } else {
        snprintf(value, sizeof(value), "no, looking for satellites");
    }
    stat_put(ROW_FACT, "Lock", value, !g.fix);
    snprintf(value, sizeof(value), "%u used, %u in view, best %u dB", g.used, g.in_view, g.best_snr);
    stat_put(ROW_FACT, "Satellites", value, !g.fix);
    if (g.fix && g.hdop_c != HH_GPS_UNKNOWN) {
        snprintf(value, sizeof(value), "HDOP %u.%02u, %s", g.hdop_c / 100u, g.hdop_c % 100u, dop_word(g.hdop_c));
        stat_put(ROW_FACT, "Accuracy", value, g.hdop_c > 500u);
    }
    if (g.has_pos) {
        ui_geo_coord_text(g.lat_u, g.lon_u, value, sizeof(value));
        stat_put(ROW_FACT, "Position", value, false);
    }
    if (g.fix) {
        char alt[16] = "?";
        if (g.has_alt) {
            snprintf(alt, sizeof(alt), "%ld m", (long)((g.alt_dm + (g.alt_dm < 0 ? -5 : 5)) / 10));
        }
        uint32_t kmh10 = g.speed_cms == HH_GPS_UNKNOWN ? 0u : (uint32_t)g.speed_cms * 36u / 100u;   /* 0.1 km/h */
        if (g.speed_cms != HH_GPS_UNKNOWN && kmh10 >= 20u && g.course_cd != HH_GPS_UNKNOWN) {
            snprintf(value, sizeof(value), "%s, %lu km/h %s", alt, (unsigned long)(kmh10 / 10u),
                     ui_geo_compass((uint16_t)(g.course_cd / 100u)));
        } else {
            snprintf(value, sizeof(value), "%s, not moving", alt);   /* under 2 km/h is GPS drift */
        }
        stat_put(ROW_FACT, "Altitude", value, false);
    }
    if (g.utc != 0) {
        uint32_t day = g.utc % 86400u;
        char age[16];
        ui_geo_age_text(g.fix_age_ms == UINT32_MAX ? 0u : g.fix_age_ms / 1000u, age, sizeof(age));
        snprintf(value, sizeof(value), "%02lu:%02lu:%02lu UTC, %s ago", (unsigned long)(day / 3600u),
                 (unsigned long)(day / 60u % 60u), (unsigned long)(day % 60u), age);
        stat_put(ROW_FACT, "Last fix", value, !g.fix);
    }
    snprintf(value, sizeof(value), "%lu sentences, %lu bad", (unsigned long)g.sentences, (unsigned long)g.bad);
    stat_put(ROW_FACT, "Data", value, g.bad > g.sentences / 20u);
}

/*
 * The optional LoRa module (D71, D76), only on a board wired for one: whether it is fitted, the
 * link to an AP and the last signal. Three rows at most, and none at all on the boards that carry
 * no module, which is nearly all of them.
 */
static void stat_lora(const hh_status_t *st)
{
    hh_lora_state_t lo;
    if (!hh_service_lora(&lo)) {
        return;   /* this board has no LoRa connector: nothing to say */
    }
    char value[SLIST_VALUE_MAX];
    stat_put(ROW_SECTION, "LoRa", NULL, false);
    if (!lo.fitted) {
        stat_put(ROW_FACT, "Module", lo.ever_fitted ? "stopped answering" : "none fitted", lo.ever_fitted);
        return;
    }
    stat_put(ROW_FACT, "Module", lo.off ? "fitted, held off" : "fitted", lo.off);
    /*
     * A handheld's radio is meant to be quiet. D74 sends everything but an alert over Wi-Fi while
     * Wi-Fi is working, so on a healthy grid this link is idle by design, and an earlier version of
     * this screen called that "down" and marked it as a fault - which read as a broken module when
     * nothing was wrong. Whether it matters depends entirely on whether Wi-Fi is carrying: silence
     * with Wi-Fi up is the radio in reserve, silence with Wi-Fi down is the thing to worry about.
     */
    bool wifi_ok = st != NULL && st->link == HH_LINK_ONLINE;
    if (lo.heard_age_ms == UINT32_MAX) {
        stat_put(ROW_FACT, "Link", wifi_ok ? "in reserve; no AP heard on it yet" : "no AP heard yet", !wifi_ok);
        return;
    }
    char age[16];
    ui_geo_age_text(lo.heard_age_ms / 1000u, age, sizeof(age));
    if (lo.link) {
        snprintf(value, sizeof(value), "up, heard %s ago", age);
        stat_put(ROW_FACT, "Link", value, false);
    } else if (wifi_ok) {
        snprintf(value, sizeof(value), "in reserve, last heard %s ago", age);
        stat_put(ROW_FACT, "Link", value, false);   /* not a fault: Wi-Fi is carrying */
    } else {
        snprintf(value, sizeof(value), "no AP for %s, and Wi-Fi is down", age);
        stat_put(ROW_FACT, "Link", value, true);
    }
    snprintf(value, sizeof(value), "%d dBm, SNR %d", lo.rssi, lo.snr);
    stat_put(ROW_FACT, "Signal", value, lo.rssi < -110);
}

static void stat_location(const hh_status_t *st)
{
    char value[SLIST_VALUE_MAX];
    char where[32];
    stat_gps();
    stat_lora(st);
    stat_put(ROW_SECTION, "Location", NULL, false);
    hh_position_t own;
    bool have_own = hh_service_own_position(&own) && own.valid;
    hh_position_t main_pos;
    if (hh_service_position(HH_SUBJECT_MAIN, &main_pos) && main_pos.valid) {
        if (have_own) {
            ui_geo_where_text(own.lat_u, own.lon_u, main_pos.lat_u, main_pos.lon_u, value, sizeof(value));
        } else {
            ui_geo_coord_text(main_pos.lat_u, main_pos.lon_u, value, sizeof(value));
        }
        stat_put(ROW_FACT, "MAIN", value, false);
    } else {
        stat_put(ROW_FACT, "MAIN", "not known", true);
    }
    if (!have_own) {
        return;   /* distances need this handheld's own fix */
    }
    /* Other handhelds with known positions, freshest first, at most STATUS_NEARBY_MAX. */
    nearby_t near[STATUS_NEARBY_MAX];
    uint8_t n = 0;
    for (uint8_t i = 0; i < st->n_people; i++) {
        hh_position_t p;
        if (st->people[i].device == st->device || !hh_service_position(st->people[i].device, &p) || !p.valid) {
            continue;
        }
        uint8_t at = n;   /* insertion sort by age, dropping the oldest when full */
        while (at > 0 && near[at - 1u].pos.age_s > p.age_s) {
            at--;
        }
        if (at >= STATUS_NEARBY_MAX) {
            continue;
        }
        uint8_t last = n < STATUS_NEARBY_MAX ? n : (uint8_t)(STATUS_NEARBY_MAX - 1u);
        for (uint8_t k = last; k > at; k--) {
            near[k] = near[k - 1u];
        }
        near[at] = (nearby_t){ i, p };
        if (n < STATUS_NEARBY_MAX) {
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    stat_put(ROW_SECTION, "Nearby", NULL, false);
    for (uint8_t k = 0; k < n; k++) {
        const hh_position_t *p = &near[k].pos;
        ui_geo_where_text(own.lat_u, own.lon_u, p->lat_u, p->lon_u, where, sizeof(where));
        if (p->age_s > STATUS_STALE_S) {
            char age[16];
            ui_geo_age_text(p->age_s, age, sizeof(age));
            snprintf(value, sizeof(value), "%s, %s ago", where, age);
        } else {
            snprintf(value, sizeof(value), "%s", where);
        }
        stat_put(ROW_FACT, st->people[near[k].person].name, value, p->age_s > STATUS_STALE_S);
    }
}

static void stat_rows(void)
{
    const hh_status_t *st = ui_status();
    char value[SLIST_VALUE_MAX];
    stat_put(ROW_FACT, "Name", st->name, false);
    stat_put(ROW_FACT, "Link", link_word(st->link), st->link != HH_LINK_ONLINE);
    stat_put(ROW_FACT, "AP", st->node >= 0 ? st->node_ssid : "--", false);
    snprintf(value, sizeof(value), "%d dBm", st->rssi);
    stat_put(ROW_FACT, "Signal", value, false);
    snprintf(value, sizeof(value), "%u.%u.%u.%u", st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
    stat_put(ROW_FACT, "Address", value, false);
    if (st->grid_time == 0) {
        snprintf(value, sizeof(value), "not set");
    } else {
        struct tm lt;
        hh_local_time(st->grid_time, &lt);   /* D67: the grid's zone, UTC until one is known */
        snprintf(value, sizeof(value), "%02d:%02d:%02d%s%s", lt.tm_hour, lt.tm_min, lt.tm_sec,
                 st->time_zone[0] != '\0' ? "" : " UTC", st->time_from_gps ? " GPS" : "");
    }
    stat_put(ROW_FACT, "Grid time", value, st->grid_time == 0);
    snprintf(value, sizeof(value), "%u", st->n_people);
    stat_put(ROW_FACT, "People", value, false);
    snprintf(value, sizeof(value), "%u", st->n_groups);
    stat_put(ROW_FACT, "Groups", value, false);
    stat_location(st);
    snprintf(value, sizeof(value), "%" PRIu32 " KB", esp_get_free_heap_size() / 1024u);
    stat_put(ROW_FACT, "Free memory", value, false);
    snprintf(value, sizeof(value), "%" PRIu32 " KB", esp_get_minimum_free_heap_size() / 1024u);
    stat_put(ROW_FACT, "Lowest", value, false);
    const lg_selftest_result_t *test = lg_selftest_last();   /* run at boot (D24) */
    if (test->failures == 0) {
        snprintf(value, sizeof(value), "%u checks passed", test->checks);
    } else {
        snprintf(value, sizeof(value), "%u of %u failed", test->failures, test->checks);
    }
    stat_put(ROW_FACT, "Self test", value, test->failures != 0);
}

static void stat_build(void)
{
    slist_begin("Status", false, false);
    s_stat.building = true;
    stat_rows();
    s_stat.building = false;
}

void ui_status_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    stat_build();
    slist_show(w, h, 0, false);
    static bool marked;
    if (!marked) {
        marked = true;
        hh_mem_mark("Status screen drawn");
    }
}

void ui_status_refresh(void)
{
    s_stat.same = true;
    s_stat.index = 0;
    s_stat.dirty = 0;
    stat_rows();
    if (!s_stat.same || s_stat.index != slist_count()) {
        stat_build();
        slist_update();
        return;
    }
    for (uint8_t i = 0; s_stat.dirty != 0u && i < slist_count(); i++) {
        if (s_stat.dirty & (1u << i)) {
            slist_repaint_row(i);
        }
    }
}

void ui_status_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);   /* nothing to tap: the rows only scroll, and the house goes home */
}

/* ---- which AP ---- */

static uint32_t s_ap_shown;

static uint32_t which_ap_build(bool rebuild)
{
    const hh_status_t *st = ui_status();
    uint32_t sig = mix(2166136261u, (uint32_t)(st->preferred_node + 2));
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        sig = mix(mix(sig, st->nodes[i].node), (uint32_t)(st->nodes[i].rssi / 6));
    }
    if (!rebuild) {
        return sig;
    }
    slist_begin("Which AP", true, false);
    slist_add(ROW_NOTE, NULL, "Automatic follows the strongest AP with a working backbone.", -1, 0);
    slist_add(ROW_ACTION, "Automatic", st->preferred_node < 0 ? "in use" : "", 30, -1);
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        const hh_node_seen_t *n = &st->nodes[i];
        char value[SLIST_VALUE_MAX];
        snprintf(value, sizeof(value), "%d dBm%s", n->rssi, st->preferred_node == n->node ? ", in use" : "");
        slist_add(ROW_ACTION, n->ssid, value, 30, n->node);
    }
    if (st->n_nodes == 0) {
        slist_add(ROW_NOTE, NULL, "No APs heard yet. Try Look for APs.", -1, 0);
    }
    return sig;
}

void ui_which_ap_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_ap_shown = which_ap_build(true);
    slist_show(w, h, 0, false);
}

void ui_which_ap_refresh(void)
{
    uint32_t sig = which_ap_build(false);
    if (sig != s_ap_shown) {
        s_ap_shown = which_ap_build(true);
        slist_update();
    }
}

void ui_which_ap_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        ui_go(NAV_SETTINGS, 0, SET_TAB_GRID, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 30) {
        hh_service_prefer_node((int)ev.arg);
        ui_which_ap_refresh();
    }
}

/* ---- rename ---- */

static struct {
    char name[HH_NAME_MAX];
} s_rn;

static void rename_show(void)
{
    const hh_status_t *st = ui_status();
    slist_begin("Name", true, false);
    slist_add(ROW_FIELD, "Name", s_rn.name, 40, 0)->focused = true;
    slist_add(ROW_BUTTON, "Save", NULL, 41, 0)->style = BTN_MAIN;
    char note[SLIST_VALUE_MAX];
    snprintf(note, sizeof(note), "Now: %s. Every AP and handheld in the grid gets the new name.", st->name);
    slist_add(ROW_NOTE, NULL, note, -1, 0);
    ui_kb_open(s_w, s_h, s_rn.name, sizeof(s_rn.name));
    slist_show(s_w, s_h, ui_kb_height(), false);
    ui_kb_draw();
}

void ui_rename_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    snprintf(s_rn.name, sizeof(s_rn.name), "%s", ui_status()->name);
    rename_show();
}

void ui_rename_touch(int16_t x, int16_t y, bool down)
{
    kb_event_t kev;
    if (ui_kb_touch(x, y, down, &kev)) {
        if (kev == KB_TEXT_CHANGED) {
            snprintf(slist_row(0)->value, SLIST_VALUE_MAX, "%s", s_rn.name);
            slist_repaint_row(0);
        } else if (kev == KB_HIDE) {
            ui_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
        }
        return;
    }
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        ui_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 41) {
        esp_err_t err = hh_service_set_name(s_rn.name);
        if (err == ESP_OK) {
            ui_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
        } else {
            snprintf(slist_row(2)->value, SLIST_VALUE_MAX, "%s",
                     err == ESP_ERR_INVALID_ARG ? "Not saved: a name needs 1 to 23 bytes; emoji count as 4."
                                                : "Not saved: the handheld is busy. Try again.");
            slist_row(2)->warn = true;
            slist_update();
        }
    }
}

/* ---- touch calibration ---- */

void ui_calibrate_run(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    const lg_draw_palette_t p = { .bg = C_BG, .accent = C_ACCENT, .muted = C_MUTED, .error = C_ERROR,
                                  .title = F_TITLE, .small = F_SMALL };
    (void)lg_draw_calibrate(&p);
    ui_go(NAV_SETTINGS, 0, SET_TAB_SCREEN, NULL);
}

void ui_group_edit_redraw(void)
{
    editor_show(true);
}

void ui_rename_redraw(void)
{
    rename_show();
}
