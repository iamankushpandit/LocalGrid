/*
 * Settings, and the node chooser behind it.
 *
 * Rows are rebuilt from live data on every refresh, so what the screen says is what the
 * service currently reports. Two actions cannot run here: touch calibration waits for
 * presses and the self test runs for tens of milliseconds, and this code runs on the
 * drawing task. Both are handed to the application task through ui_settings_take_job.
 */
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
    volatile ui_job_t    job;
    uint32_t             shown_signature;
    uint32_t             shown_nodes_signature;
} s_ui;

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

static void on_choose_node(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_ui.nodes_screen);
    s_ui.shown_nodes_signature = 0;   /* draw it from the current snapshot */
}

static void on_node_picked(lv_event_t *e)
{
    int node = (int)(intptr_t)lv_event_get_user_data(e);
    hh_service_prefer_node(node);
    ESP_LOGI(TAG, "[UI] Node choice: %s", node < 0 ? "automatic" : "fixed");
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

/* Tapping steps through the four levels the hardware actually has and wraps at the top. */
static void on_sound(lv_event_t *e)
{
    (void)e;
    lg_bsp_audio_set_volume((uint8_t)((lg_bsp_audio_volume() + 1u) % LG_VOLUME_STEPS));
}

static void on_saver(lv_event_t *e)
{
    (void)e;
    lg_ui_screensaver_set_enabled(!lg_ui_screensaver_enabled());
}

static void on_calibrate(lv_event_t *e)
{
    (void)e;
    s_ui.job = UI_JOB_CALIBRATE;
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
    ui_list_action(s_ui.list, "Reconnect now", NULL, on_reconnect, NULL);
    ui_list_action(s_ui.list, "Look for APs", NULL, on_scan, NULL);
    ui_list_note(s_ui.list, "Grid time comes from the master's page; a handheld never sets it.");

    ui_list_clear(s_ui.sound_list);
    if (lg_bsp_audio_available()) {
        ui_list_action(s_ui.sound_list, "Volume", lg_bsp_audio_volume_name(lg_bsp_audio_volume()),
                       on_sound, NULL);
        ui_list_note(s_ui.sound_list, "A bell when a message arrives, and three notes for an urgent one.");
        ui_list_note(s_ui.sound_list,
                     "Off silences the handheld except for urgent broadcasts, which always sound.");
    } else {
        ui_list_row(s_ui.sound_list, "Volume", "no speaker on this board");
    }

    ui_list_clear(s_ui.screen_list);
    if (lg_bsp_touch_can_calibrate()) {
        ui_list_action(s_ui.screen_list, "Calibrate touch", lg_bsp_touch_needs_calibration() ? "needed" : NULL,
                       on_calibrate, NULL);
    } else {
        ui_list_row(s_ui.screen_list, "Touch", "no calibration needed");
    }
    ui_list_action(s_ui.screen_list, "Screen saver", lg_ui_screensaver_enabled() ? "on" : "off",
                   on_saver, NULL);
    ui_list_note(s_ui.screen_list, "Green rain covers the panel after a minute untouched; a touch clears it.");

    ui_list_clear(s_ui.device_list);
    ui_list_row(s_ui.device_list, "Name", st->name);
    snprintf(value, sizeof(value), "%" PRIu32, st->device);
    ui_list_row(s_ui.device_list, "Device number", value);
    ui_list_row(s_ui.device_list, "Board",
                s_ui.identity != NULL && s_ui.identity->present ? s_ui.identity->board : "unknown");
    ui_list_row(s_ui.device_list, "Identity",
                s_ui.identity != NULL && s_ui.identity->present ? s_ui.identity->id : "none");
    snprintf(value, sizeof(value), "%" PRIu32 " KB free, %" PRIu32 " KB lowest", st->free_heap / 1024u,
             st->min_free_heap / 1024u);
    ui_list_row(s_ui.device_list, "Memory", value);
    ui_list_action(s_ui.device_list, "Restart", NULL, on_restart, NULL);
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
    static hh_status_t st;
    lv_obj_t *active = lv_screen_active();
    if (active != s_ui.screen && active != s_ui.nodes_screen) {
        return;
    }
    hh_service_status(&st);
    if (active == s_ui.screen) {
        ui_bar_update(s_ui.bar, &st);   /* one short label, written only when it differs */
        uint32_t signature = settings_signature(&st);
        if (signature != s_ui.shown_signature) {
            s_ui.shown_signature = signature;
            rebuild_settings(&st);
        }
    } else {
        ui_bar_update(s_ui.nodes_bar, &st);
        uint32_t signature = nodes_signature(&st);
        if (signature != s_ui.shown_nodes_signature) {
            s_ui.shown_nodes_signature = signature;
            rebuild_nodes(&st);
        }
    }
}

void ui_settings_build(const lg_identity_t *identity)
{
    const lg_theme_t *t = lg_theme();
    s_ui.identity = identity;
    lg_display_lock(1000);

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
    lv_obj_set_style_text_color(s_ui.tabs, t->text, 0);

    s_ui.list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Grid"));
    s_ui.sound_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Sound"));
    s_ui.screen_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Screen"));
    s_ui.device_list = ui_list_create(lv_tabview_add_tab(s_ui.tabs, "Device"));

    s_ui.nodes_screen = lv_obj_create(NULL);
    lg_theme_apply_screen(s_ui.nodes_screen);
    lv_obj_remove_flag(s_ui.nodes_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.nodes_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ui.nodes_screen, t->pad, 0);
    lv_obj_set_style_pad_row(s_ui.nodes_screen, t->gap, 0);
    s_ui.nodes_bar = ui_bar_create(s_ui.nodes_screen, "Which AP", on_back_to_settings);
    s_ui.nodes_list = ui_list_create(s_ui.nodes_screen);

    lv_timer_create(refresh, REFRESH_MS, NULL);
    lg_display_unlock();
}

void ui_settings_open(void)
{
    if (s_ui.screen == NULL) {
        return;
    }
    lg_display_lock(1000);
    s_ui.shown_signature = 0;
    lv_screen_load(s_ui.screen);
    lg_display_unlock();
}

void ui_settings_run_selftest(void)
{
    s_ui.job = UI_JOB_SELFTEST;
}

ui_job_t ui_settings_take_job(void)
{
    ui_job_t job = s_ui.job;
    s_ui.job = UI_JOB_NONE;
    return job;
}
