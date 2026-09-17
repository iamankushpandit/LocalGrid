/*
 * The no-LVGL list screens: the conversation list, Groups and the group editor, and Settings
 * with Which AP, the rename screen, and touch calibration. Each rebuilds its rows from the status
 * snapshot and repaints only when what it shows changed, so a refresh never costs a repaint and a
 * tap is never lost to one (the LVGL conversation list rebuilt itself every second).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_settings.h"
#include "lg_bsp_touch.h"
#include "lg_envelope.h"
#include "spike_kb.h"
#include "spike_list.h"
#include "spike_nav.h"
#include "spike_theme.h"

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
    const hh_status_t *st = spike_status();
    uint32_t sig = 2166136261u;
    sig = mix(sig, st->time_restricted);
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
    s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_BROADCAST, LG_TARGET_ALL };
    slist_add(ROW_ACTION, "Everyone", st->time_restricted ? "urgent only" : "broadcast", 0, n++);
    for (uint8_t i = 0; i < st->n_groups && n < SLIST_ROWS - 2; i++) {
        if (st->groups[i].member) {
            s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_GROUP, st->groups[i].id };
            slist_add(ROW_ACTION, st->groups[i].name, "group", 0, n++);
        }
    }
    for (uint8_t i = 0; i < st->n_people && n < SLIST_ROWS - 2; i++) {
        s_convs.refs[n] = (conv_ref_t){ LG_SCOPE_DIRECT, st->people[i].device };
        slist_add(ROW_ACTION, st->people[i].name, st->people[i].online ? "online, encrypted" : "offline", 0, n++);
    }
    if (st->n_people == 0) {
        slist_add(ROW_NOTE, NULL, "No other handheld seen yet, so there is no one to message directly.", -1, 0);
    }
    return sig;
}

void spike_convs_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_convs.shown = convs_build(true);
    slist_show(w, h, 0, false);
}

void spike_convs_refresh(void)
{
    uint32_t sig = convs_build(false);
    if (sig != s_convs.shown) {
        s_convs.shown = convs_build(true);
        slist_update();
    }
}

void spike_convs_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        spike_go(NAV_HOME, 0, 0, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 0 && ev.arg < SLIST_ROWS) {
        const conv_ref_t *c = &s_convs.refs[ev.arg];
        spike_go(NAV_CHAT, c->scope, c->target, slist_row(ev.index)->label);
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
    const hh_status_t *st = spike_status();
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
        char members[SLIST_VALUE_MAX];
        member_names(st, g, members, sizeof(members));
        slist_add(ROW_NOTE, NULL, members, -1, 0);
    }
    return sig;
}

void spike_groups_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_groups.shown = groups_build(true);
    slist_show(w, h, 0, false);
}

void spike_groups_refresh(void)
{
    uint32_t sig = groups_build(false);
    if (sig != s_groups.shown) {
        s_groups.shown = groups_build(true);
        slist_update();
    }
}

void spike_groups_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        spike_go(NAV_HOME, 0, 0, NULL);
    } else if (ev.type == SLIST_PLUS) {
        spike_go(NAV_GROUP_EDIT, 0, 0, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 1) {
        spike_go(NAV_GROUP_EDIT, 0, (uint32_t)ev.arg, NULL);
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
    const hh_status_t *st = spike_status();
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
        spike_kb_open(s_w, s_h, s_ed.name, sizeof(s_ed.name));
        slist_show(s_w, s_h, spike_kb_height(), false);
        spike_kb_draw();
    } else {
        slist_show(s_w, s_h, 0, keep_scroll);
    }
}

void spike_group_edit_open(uint16_t w, uint16_t h, uint16_t id)
{
    s_w = w;
    s_h = h;
    memset(&s_ed, 0, sizeof(s_ed));
    s_ed.id = id;
    const hh_status_t *st = spike_status();
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
    const hh_status_t *st = spike_status();
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
    ESP_LOGI(TAG, "[UI] Spike group %s sent: id %u", remove ? "removal" : s_ed.id ? "change" : "creation", s_ed.id);
}

void spike_group_edit_refresh(uint32_t now_ms)
{
    if (!s_ed.waiting) {
        return;
    }
    const hh_status_t *st = spike_status();
    if (st->groups_version != s_ed.waiting_groups) {
        s_ed.waiting = false;
        spike_go(NAV_GROUPS, 0, 0, NULL);   /* the new table is here: Groups shows the result */
    } else if (st->version != s_ed.waiting_status && st->group_problem[0] != '\0') {
        s_ed.waiting = false;
        set_problem(st->group_problem);
    } else if (now_ms - s_ed.waiting_since > ANSWER_WAIT_MS) {
        s_ed.waiting = false;
        set_problem("No answer from the AP; try again.");
    }
}

void spike_group_edit_touch(int16_t x, int16_t y, bool down)
{
    if (s_ed.typing) {
        kb_event_t kev;
        if (spike_kb_touch(x, y, down, &kev)) {
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
            spike_go(NAV_GROUPS, 0, 0, NULL);
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
       SET_RESTART };

static const char *const SET_TABS[] = { "Grid", "Sound", "Screen", "Device" };
static const char *const VOLUME_NAMES[LG_VOLUME_STEPS] = { "Off", "Low", "Med", "High" };

static struct {
    uint8_t  tab;
    uint32_t shown;
    bool     restart_armed;
} s_set;

static uint32_t settings_build(bool rebuild)
{
    const hh_status_t *st = spike_status();
    uint32_t sig = mix(mix(mix(2166136261u, s_set.tab), st->link), (uint32_t)(st->preferred_node + 2));
    sig = mix_str(mix(sig, st->grid_time != 0), st->node_ssid);
    sig = mix(mix(sig, lg_bsp_audio_volume()), lg_bsp_setting_get_bool("saver", true));
    sig = mix(mix_str(sig, st->name), lg_bsp_touch_needs_calibration());
    sig = mix(mix(sig, st->free_heap / 4096u), s_set.restart_armed);
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
        slist_add(ROW_BUTTON, s_set.restart_armed ? "Tap again to restart" : "Restart", NULL, SET_RESTART, 0)->style =
            BTN_DANGER;
        break;
    default:
        break;
    }
    return sig;
}

void spike_settings_open(uint16_t w, uint16_t h, uint8_t tab)
{
    s_w = w;
    s_h = h;
    s_set.tab = tab < 4 ? tab : 0;
    s_set.restart_armed = false;
    s_set.shown = settings_build(true);
    slist_show(w, h, 0, false);
}

void spike_settings_refresh(void)
{
    uint32_t sig = settings_build(false);
    if (sig != s_set.shown) {
        s_set.shown = settings_build(true);
        slist_update();
    }
}

void spike_settings_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    switch (ev.type) {
    case SLIST_BACK:
        spike_go(NAV_HOME, 0, 0, NULL);
        return;
    case SLIST_TAB:
        if (ev.index != s_set.tab) {
            spike_settings_open(s_w, s_h, ev.index);
        }
        return;
    case SLIST_ROW:
        break;
    default:
        return;
    }
    switch (ev.id) {
    case SET_WHICH_AP:
        spike_go(NAV_WHICH_AP, 0, 0, NULL);
        break;
    case SET_RECONNECT:
        hh_service_reconnect();
        break;
    case SET_SCAN:
        hh_service_scan_now();
        break;
    case SET_VOLUME:
        lg_bsp_audio_set_volume((uint8_t)ev.arg);
        spike_settings_refresh();
        break;
    case SET_TEST:
        (void)lg_bsp_audio_cue((lg_cue_t)ev.arg);
        break;
    case SET_SAVER:
        (void)lg_bsp_setting_set_bool("saver", !lg_bsp_setting_get_bool("saver", true));
        spike_settings_refresh();
        break;
    case SET_CALIBRATE:
        spike_go(NAV_CALIBRATE, 0, 0, NULL);
        break;
    case SET_NAME:
        spike_go(NAV_RENAME, 0, 0, NULL);
        break;
    case SET_RESTART:
        if (!s_set.restart_armed) {
            s_set.restart_armed = true;
            spike_settings_refresh();
        } else {
            ESP_LOGI(TAG, "[UI] Restart asked from Settings");
            esp_restart();
        }
        break;
    default:
        break;
    }
}

/* ---- which AP ---- */

static uint32_t s_ap_shown;

static uint32_t which_ap_build(bool rebuild)
{
    const hh_status_t *st = spike_status();
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

void spike_which_ap_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    s_ap_shown = which_ap_build(true);
    slist_show(w, h, 0, false);
}

void spike_which_ap_refresh(void)
{
    uint32_t sig = which_ap_build(false);
    if (sig != s_ap_shown) {
        s_ap_shown = which_ap_build(true);
        slist_update();
    }
}

void spike_which_ap_touch(int16_t x, int16_t y, bool down)
{
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        spike_go(NAV_SETTINGS, 0, SET_TAB_GRID, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 30) {
        hh_service_prefer_node((int)ev.arg);
        spike_which_ap_refresh();
    }
}

/* ---- rename ---- */

static struct {
    char name[HH_NAME_MAX];
} s_rn;

static void rename_show(void)
{
    const hh_status_t *st = spike_status();
    slist_begin("Name", true, false);
    slist_add(ROW_FIELD, "Name", s_rn.name, 40, 0)->focused = true;
    slist_add(ROW_BUTTON, "Save", NULL, 41, 0)->style = BTN_MAIN;
    char note[SLIST_VALUE_MAX];
    snprintf(note, sizeof(note), "Now: %s. Every AP and handheld in the grid gets the new name.", st->name);
    slist_add(ROW_NOTE, NULL, note, -1, 0);
    spike_kb_open(s_w, s_h, s_rn.name, sizeof(s_rn.name));
    slist_show(s_w, s_h, spike_kb_height(), false);
    spike_kb_draw();
}

void spike_rename_open(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    snprintf(s_rn.name, sizeof(s_rn.name), "%s", spike_status()->name);
    rename_show();
}

void spike_rename_touch(int16_t x, int16_t y, bool down)
{
    kb_event_t kev;
    if (spike_kb_touch(x, y, down, &kev)) {
        if (kev == KB_TEXT_CHANGED) {
            snprintf(slist_row(0)->value, SLIST_VALUE_MAX, "%s", s_rn.name);
            slist_repaint_row(0);
        } else if (kev == KB_HIDE) {
            spike_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
        }
        return;
    }
    slist_event_t ev;
    slist_touch(x, y, down, &ev);
    if (ev.type == SLIST_BACK) {
        spike_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
    } else if (ev.type == SLIST_ROW && ev.id == 41) {
        esp_err_t err = hh_service_set_name(s_rn.name);
        if (err == ESP_OK) {
            spike_go(NAV_SETTINGS, 0, SET_TAB_DEVICE, NULL);
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

#define CAL_POINTS     3
#define CAL_TIMEOUT_MS 30000

static void cal_box(const char *text, lg_color_t fg, int16_t y, const lv_font_t *font)
{
    lg_box_t b;
    memset(&b, 0, sizeof(b));
    b.rect = (lg_rect_t){ 0, y, (int16_t)s_w, 28 };
    b.bg = b.outside = C_BG;
    b.font = font;
    b.fg = fg;
    b.align = LG_ALIGN_CENTER;
    snprintf(b.text, sizeof(b.text), "%s", text);
    lg_draw_box(&b);
}

static void cal_target(int16_t x, int16_t y, bool on)
{
    lg_rect_t h = { (int16_t)(x - 14), (int16_t)(y - 1), 29, 3 };
    lg_rect_t v = { (int16_t)(x - 1), (int16_t)(y - 14), 3, 29 };
    lg_draw_fill(&h, on ? C_ACCENT : C_BG);
    lg_draw_fill(&v, on ? C_ACCENT : C_BG);
}

void spike_calibrate_run(uint16_t w, uint16_t h)
{
    s_w = w;
    s_h = h;
    lg_rect_t all = { 0, 0, (int16_t)w, (int16_t)h };
    lg_draw_fill(&all, C_BG);
    cal_box("Touch calibration", C_ACCENT, (int16_t)(h * 30 / 100), F_TITLE);
    int16_t screen[CAL_POINTS][2] = {
        { (int16_t)(w * 15 / 100), (int16_t)(h * 12 / 100) },
        { (int16_t)(w * 85 / 100), (int16_t)(h * 50 / 100) },
        { (int16_t)(w * 50 / 100), (int16_t)(h * 88 / 100) },
    };
    int16_t raw[CAL_POINTS][2];
    bool ok = true;
    for (int i = 0; i < CAL_POINTS && ok; i++) {
        char hint[48];
        snprintf(hint, sizeof(hint), "Tap and hold the target, %d of %d", i + 1, CAL_POINTS);
        cal_box(hint, C_MUTED, (int16_t)(h * 30 / 100 + 30), F_SMALL);
        cal_target(screen[i][0], screen[i][1], true);
        ok = lg_bsp_touch_wait_press(&raw[i][0], &raw[i][1], CAL_TIMEOUT_MS);
        cal_target(screen[i][0], screen[i][1], false);
        if (ok) {
            ESP_LOGI(TAG, "[UI] calibration point %d: raw %d,%d for screen %d,%d", i + 1, raw[i][0], raw[i][1],
                     screen[i][0], screen[i][1]);
        }
    }
    esp_err_t err = ok ? lg_bsp_touch_set_calibration(raw, screen) : ESP_ERR_TIMEOUT;
    ok = !lg_bsp_touch_needs_calibration();
    cal_box(ok ? "Calibration saved" : "Calibration failed", ok ? C_ACCENT : C_ERROR, (int16_t)(h * 30 / 100), F_TITLE);
    cal_box(ok ? "" : err == ESP_ERR_TIMEOUT ? "No touch for 30 seconds" : "Touch three different places", C_MUTED,
            (int16_t)(h * 30 / 100 + 30), F_SMALL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    spike_go(NAV_SETTINGS, 0, SET_TAB_SCREEN, NULL);
}
