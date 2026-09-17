/*
 * Settings, and the node chooser behind it.
 *
 * Rows are rebuilt from live data on every refresh, so what the screen says is what the
 * service currently reports. Two actions cannot run here: touch calibration waits for
 * presses and the self test runs for tens of milliseconds, and this code runs on the
 * drawing task. Both are handed to the application task through ui_settings_take_job.
 */
#include "ui_snapshot.h"
#include "ui_settings.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_ui_screensaver.h"
#include "lg_bsp_touch.h"
#include "lg_display.h"
#include "lg_envelope.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "ui_bar.h"
#include "ui_launcher.h"
#include "ui_list.h"
#include "ui_screen.h"

static const char *TAG = "UI";

#define REFRESH_MS 700

static struct {
    const lg_identity_t *identity;
    lv_obj_t            *screen;
    lv_obj_t            *bar;
    lv_obj_t            *tabs;
    lv_obj_t            *list;        /* Grid */
    lv_obj_t            *sound_list;
    lv_obj_t            *screen_list;
    lv_obj_t            *device_list;
    lv_obj_t            *nodes_screen;
    lv_obj_t            *nodes_bar;
    lv_obj_t            *nodes_list;
    lv_obj_t            *rename_screen;
    lv_obj_t            *rename_input;
    lv_obj_t            *rename_note;
    uint32_t             shown_signature;
    uint32_t             shown_nodes_signature;
} s_ui;

static volatile ui_job_t s_job;   /* outside s_ui: freeing a screen must not lose a queued job */

/*
 * The service republishes its snapshot every second, so refreshing on its version counter
 * rebuilt these lists constantly. A signature over only the values a screen shows means the
 * rows are rebuilt when one of them actually changes, and left alone otherwise.
 */
static uint32_t mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

static uint32_t settings_signature(const hh_status_t *st)
{
    uint32_t h = 2166136261u;
    h = mix(h, (uint32_t)st->preferred_node);
    h = mix(h, (uint32_t)st->link);
    h = mix(h, (uint32_t)st->node);
    h = mix(h, (uint32_t)((uint8_t)(st->rssi + 128) / 6u));   /* ~6 dB steps, not noise */
    h = mix(h, st->grid_time != 0 ? 1u : 0u);
    h = mix(h, st->device);
    /*
     * Coarse buckets, deliberately. Free heap moves every second, and while it was in this
     * signature the whole list was rebuilt on almost every refresh -- which is what threw the
     * reader's scroll position away. 16 KB steps change when something real happens.
     */
    h = mix(h, st->free_heap / (16u * 1024u));
    h = mix(h, st->min_free_heap / (16u * 1024u));
    h = mix(h, lg_bsp_touch_needs_calibration() ? 1u : 0u);
    h = mix(h, lg_bsp_audio_volume());
    h = mix(h, lg_ui_screensaver_enabled() ? 1u : 0u);
    return h;
}

static uint32_t nodes_signature(const hh_status_t *st)
{
    uint32_t h = 2166136261u;
    h = mix(h, (uint32_t)st->preferred_node);
    h = mix(h, (uint32_t)st->node);
    h = mix(h, st->n_nodes);
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        h = mix(h, st->nodes[i].node);
        h = mix(h, (uint32_t)((uint8_t)(st->nodes[i].rssi + 128) / 6u));   /* ~6 dB steps, not noise */
        h = mix(h, st->nodes[i].backbone ? 1u : 0u);
    }
    return h;
}

static void on_home(lv_event_t *e)
{
    (void)e;
    ui_launcher_open();
}

static void on_back_to_settings(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

/* ---- actions ---- */

static void build_nodes_screen(void);

static void on_choose_node(lv_event_t *e)
{
    (void)e;
    if (s_ui.nodes_screen == NULL) {
        build_nodes_screen();
    }
    s_ui.shown_nodes_signature = 0;   /* draw it from the current snapshot */
    lv_screen_load(s_ui.nodes_screen);
}

static void on_node_picked(lv_event_t *e)
{
    int node = (int)(intptr_t)lv_event_get_user_data(e);
    hh_service_prefer_node(node);
    ESP_LOGI(TAG, "[UI] Node choice: %s", node < 0 ? "automatic" : "fixed");
    ui_settings_open();
}

/* ---- rename (D50) ---- */

static void build_rename_screen(void);

static void on_rename(lv_event_t *e)
{
    (void)e;
    if (s_ui.rename_screen == NULL) {
        build_rename_screen();
    }
    lv_textarea_set_text(s_ui.rename_input, ui_status()->name);
    lv_screen_load(s_ui.rename_screen);
}

/* The keyboard's OK key, or Enter in the one-line field. */
static void on_rename_ready(lv_event_t *e)
{
    (void)e;
    if (s_ui.rename_input == NULL) {
        return;   /* already left */
    }
    const lg_theme_t *t = lg_theme();
    const char *name = lv_textarea_get_text(s_ui.rename_input);
    esp_err_t err = hh_service_set_name(name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[UI] Rename requested");
        ui_settings_open();
        return;
    }
    lv_obj_set_style_text_color(s_ui.rename_note, t->error, 0);
    lg_ui_set_text(s_ui.rename_note, err == ESP_ERR_INVALID_ARG
                                         ? "Not saved: a name needs 1 to 23 bytes; emoji count as 4."
                                         : "Not saved: the handheld is busy. Try again.");
}

static void on_rename_cancel(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

static void on_reconnect(lv_event_t *e)
{
    (void)e;
    hh_service_reconnect();
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    hh_service_scan_now();
}

/* One button per level the hardware actually has (D40); the button's user data is the level. */
static void on_volume(lv_event_t *e)
{
    lg_bsp_audio_set_volume((uint8_t)(intptr_t)lv_event_get_user_data(e));
}

static void on_test_sound(lv_event_t *e)
{
    lg_bsp_audio_cue((lg_cue_t)(intptr_t)lv_event_get_user_data(e));
}

static void on_saver(lv_event_t *e)
{
    (void)e;
    lg_ui_screensaver_set_enabled(!lg_ui_screensaver_enabled());
}

static void on_calibrate(lv_event_t *e)
{
    (void)e;
    s_job = UI_JOB_CALIBRATE;
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "[UI] Restart from settings");
    esp_restart();
}

/* ---- settings list ---- */

/*
 * One page per tab, the way a browser does it: four short pages instead of one long scroll,
 * so a setting is two taps away rather than a hunt. Each page is rebuilt from live data, and
 * each keeps the reader's place through ui_list_clear.
 */
static void rebuild_settings(const hh_status_t *st)
{
    char value[48];

    ui_list_clear(s_ui.list);
    if (st->preferred_node < 0) {
        snprintf(value, sizeof(value), "automatic");
    } else {
        snprintf(value, sizeof(value), "AP %d", st->preferred_node);
    }
    ui_list_action(s_ui.list, "Which AP", value, on_choose_node, NULL);
    lv_obj_t *buttons = ui_list_buttons(s_ui.list);
    ui_list_button(buttons, "Reconnect", UI_BUTTON_MAIN, on_reconnect, NULL);
    ui_list_button(buttons, "Look for APs", UI_BUTTON_PLAIN, on_scan, NULL);
    if (st->link == HH_LINK_ONLINE && st->node >= 0) {
        snprintf(value, sizeof(value), "AP %d, %d dBm", st->node, st->rssi);
    } else {
        snprintf(value, sizeof(value), "no AP");
    }
    ui_list_fact(s_ui.list, "Connected to", value);
    ui_list_fact(s_ui.list, "Grid time", st->grid_time != 0 ? "set" : "not set");
    ui_list_note(s_ui.list, "Grid time is set on any AP's admin page; a handheld never sets it.");

    ui_list_clear(s_ui.sound_list);
    if (lg_bsp_audio_available()) {
        static const char *const levels[LG_VOLUME_STEPS] = { "Off", "Low", "Med", "High" };
        ui_list_choice(s_ui.sound_list, "Volume", levels, LG_VOLUME_STEPS, lg_bsp_audio_volume(), on_volume);
        buttons = ui_list_buttons(s_ui.sound_list);
        ui_list_button(buttons, "Test sound", UI_BUTTON_MAIN, on_test_sound, (void *)(intptr_t)LG_CUE_RECEIVED);
        ui_list_button(buttons, "Test urgent", UI_BUTTON_PLAIN, on_test_sound, (void *)(intptr_t)LG_CUE_URGENT);
        ui_list_note(s_ui.sound_list, "A bell when a message arrives, and three notes for an urgent one.");
        ui_list_note(s_ui.sound_list,
                     "Off silences the handheld except for urgent broadcasts, which always sound.");
    } else {
        ui_list_fact(s_ui.sound_list, "Volume", "no speaker on this board");
    }

    ui_list_clear(s_ui.screen_list);
    ui_list_action(s_ui.screen_list, "Screen saver", lg_ui_screensaver_enabled() ? "on" : "off",
                   on_saver, NULL);
    if (lg_bsp_touch_can_calibrate()) {
        lv_obj_t *row = ui_list_action(s_ui.screen_list, "Calibrate touch",
                                       lg_bsp_touch_needs_calibration() ? "needed" : NULL, on_calibrate, NULL);
        ui_list_value_warn(row);
    } else {
        ui_list_fact(s_ui.screen_list, "Touch", "no calibration needed");
    }
    ui_list_note(s_ui.screen_list, "Green rain covers the panel after a minute untouched; a touch clears it.");

    ui_list_clear(s_ui.device_list);
    ui_list_action(s_ui.device_list, "Name", st->name, on_rename, NULL);
    snprintf(value, sizeof(value), "%" PRIu32, st->device);
    ui_list_fact(s_ui.device_list, "Device number", value);
    ui_list_fact(s_ui.device_list, "Board",
                 s_ui.identity != NULL && s_ui.identity->present ? s_ui.identity->board : "unknown");
    ui_list_fact(s_ui.device_list, "Identity",
                 s_ui.identity != NULL && s_ui.identity->present ? s_ui.identity->id : "none");
    snprintf(value, sizeof(value), "%" PRIu32 " KB, lowest %" PRIu32, st->free_heap / 1024u,
             st->min_free_heap / 1024u);
    ui_list_fact(s_ui.device_list, "Memory", value);
    ui_list_button(s_ui.device_list, "Restart", UI_BUTTON_DANGER, on_restart, NULL);
    ui_list_note(s_ui.device_list, "The self test and its last result live on the Status screen (D33).");
}

static void rebuild_nodes(const hh_status_t *st)
{
    char value[48];
    ui_list_clear(s_ui.nodes_list);
    ui_list_note(s_ui.nodes_list, "Automatic follows the strongest AP with a working backbone.");
    ui_list_action(s_ui.nodes_list, "Automatic", st->preferred_node < 0 ? "in use" : NULL, on_node_picked,
                   (void *)(intptr_t)-1);
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        const hh_node_seen_t *n = &st->nodes[i];
        bool connected = st->link == HH_LINK_ONLINE && st->node == (int)n->node;
        snprintf(value, sizeof(value), "%d dBm%s%s", n->rssi, connected ? ", connected" : "",
                 n->backbone ? "" : ", no links");
        ui_list_action(s_ui.nodes_list, n->ssid, value, on_node_picked, (void *)(intptr_t)n->node);
    }
    if (st->n_nodes == 0) {
        ui_list_note(s_ui.nodes_list, "No APs heard yet. Try Look for APs.");
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_t *active = lv_screen_active();
    if (active == NULL || (active != s_ui.screen && active != s_ui.nodes_screen)) {
        return;   /* NULL screens never match a live one, so a freed screen is never touched */
    }
    const hh_status_t *st = ui_status();
    if (active == s_ui.screen) {
        ui_bar_update(s_ui.bar, st);   /* one short label, written only when it differs */
        uint32_t signature = settings_signature(st);
        if (signature != s_ui.shown_signature) {
            s_ui.shown_signature = signature;
            rebuild_settings(st);
        }
    } else {
        ui_bar_update(s_ui.nodes_bar, st);
        uint32_t signature = nodes_signature(st);
        if (signature != s_ui.shown_nodes_signature) {
            s_ui.shown_nodes_signature = signature;
            rebuild_nodes(st);
        }
    }
}

static void forget_settings(void)
{
    s_ui.screen = NULL;
    s_ui.bar = NULL;
    s_ui.tabs = NULL;
    s_ui.list = NULL;
    s_ui.sound_list = NULL;
    s_ui.screen_list = NULL;
    s_ui.device_list = NULL;
    s_ui.shown_signature = 0;
}

static void forget_rename(void)
{
    s_ui.rename_screen = NULL;
    s_ui.rename_input = NULL;
    s_ui.rename_note = NULL;
}

static void forget_nodes(void)
{
    s_ui.nodes_screen = NULL;
    s_ui.nodes_bar = NULL;
    s_ui.nodes_list = NULL;
    s_ui.shown_nodes_signature = 0;
}

static void build_settings_screen(void)
{
    const lg_theme_t *t = lg_theme();
    s_ui.screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.screen);
    lv_obj_remove_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.screen, t->gap, 0);
    s_ui.bar = ui_bar_create(s_ui.screen, "Settings", on_home);

    /* Tabs across the top, each holding one short page. The bar is one touch target tall so a
     * tab can be hit without aiming, and the labels are single words so four fit a 240 px
     * panel. lv_tabview is in this build (CONFIG_LV_USE_TABVIEW). */
    s_ui.tabs = lv_tabview_create(s_ui.screen);
    lv_tabview_set_tab_bar_position(s_ui.tabs, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(s_ui.tabs, t->touch_min);
    lv_obj_set_flex_grow(s_ui.tabs, 1);
    lv_obj_set_width(s_ui.tabs, LV_PCT(100));
    lv_obj_set_style_bg_opa(s_ui.tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(s_ui.tabs, t->font_small, 0);

    s_ui.list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Grid"));
    s_ui.sound_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Sound"));
    s_ui.screen_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Screen"));
    s_ui.device_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Device"));
    lg_theme_style_tabview(s_ui.tabs);   /* after the tabs exist: it styles each one */
    ui_screen_free_on_leave(s_ui.screen, forget_settings);
}

static void build_nodes_screen(void)
{
    const lg_theme_t *t = lg_theme();
    s_ui.nodes_screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.nodes_screen);
    lv_obj_remove_flag(s_ui.nodes_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.nodes_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.nodes_screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.nodes_screen, t->gap, 0);
    s_ui.nodes_bar = ui_bar_create(s_ui.nodes_screen, "Which AP", on_back_to_settings);
    s_ui.nodes_list = ui_list_create(s_ui.nodes_screen);
    ui_screen_free_on_leave(s_ui.nodes_screen, forget_nodes);
}

/*
 * The name field over LVGL's own keyboard. OK saves and goes back to Settings; the keyboard's
 * hide key goes back without saving. The field counts characters and the grid counts bytes, so
 * the byte limit is checked when saving and explained on the screen when it is exceeded.
 */
static void build_rename_screen(void)
{
    const lg_theme_t *t = lg_theme();
    s_ui.rename_screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.rename_screen);
    lv_obj_remove_flag(s_ui.rename_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.rename_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.rename_screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.rename_screen, t->gap, 0);
    (void)ui_bar_create(s_ui.rename_screen, "Name", on_rename_cancel);

    s_ui.rename_input = lv_textarea_create(s_ui.rename_screen);
    lv_textarea_set_one_line(s_ui.rename_input, true);
    lv_textarea_set_max_length(s_ui.rename_input, HH_NAME_MAX - 1);
    lv_obj_set_width(s_ui.rename_input, LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.rename_input, t->surface, 0);
    lv_obj_set_style_border_color(s_ui.rename_input, t->accent, 0);
    lv_obj_set_style_border_width(s_ui.rename_input, t->stroke, 0);
    lv_obj_set_style_radius(s_ui.rename_input, t->radius, 0);
    lv_obj_set_style_text_color(s_ui.rename_input, t->text, 0);
    lv_obj_set_style_text_font(s_ui.rename_input, t->font_body, 0);
    lv_obj_set_style_bg_color(s_ui.rename_input, t->accent, LV_PART_CURSOR);
    lv_obj_set_style_border_color(s_ui.rename_input, t->accent, LV_PART_CURSOR);
    /* On the field only: the keyboard sends its OK and hide keys to both itself and the field,
     * and the first handler loads Settings, which frees this screen. */
    lv_obj_add_event_cb(s_ui.rename_input, on_rename_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ui.rename_input, on_rename_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_add_state(s_ui.rename_input, LV_STATE_FOCUSED);   /* shows the cursor */

    s_ui.rename_note = lg_ui_label(s_ui.rename_screen, t->font_small, t->muted,
                                   "Every AP and handheld in the grid gets the new name.");

    lv_obj_t *kb = lv_keyboard_create(s_ui.rename_screen);
    lv_keyboard_set_textarea(kb, s_ui.rename_input);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_set_style_bg_color(kb, t->bg, 0);
    lv_obj_set_style_text_font(kb, t->font_body, 0);   /* falls back to the emoji font */
    ui_screen_free_on_leave(s_ui.rename_screen, forget_rename);
}

/* Screens are built when opened and freed when left (ui_screen.h); only the timer lives on. */
void ui_settings_build(const lg_identity_t *identity)
{
    s_ui.identity = identity;
    lv_timer_create(refresh, REFRESH_MS, NULL);
}

void ui_settings_open(void)
{
    lg_display_lock(1000);
    if (s_ui.screen == NULL) {
        build_settings_screen();
    }
    s_ui.shown_signature = 0;
    lv_screen_load(s_ui.screen);
    lg_display_unlock();
}

void ui_settings_run_selftest(void)
{
    s_job = UI_JOB_SELFTEST;
}

ui_job_t ui_settings_take_job(void)
{
    ui_job_t job = s_job;
    s_job = UI_JOB_NONE;
    return job;
}
