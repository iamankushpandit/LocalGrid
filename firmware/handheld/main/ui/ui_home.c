/*
 * Home screen (P5): this handheld, its link to the grid, grid time, the handhelds it has
 * heard about, and the nodes in range. It reads hh_service_status() twice a second and
 * sends only hh_service_prefer_node(); nothing here touches the network (D27).
 * Layout is flex columns with percentage widths; fonts, spacing, and colours come from
 * the theme (D9, D10).
 */
#include "ui_home.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hh_service.h"
#include "lg_display.h"
#include "lg_selftest.h"
#include "lg_theme.h"
#include "lg_ui_widgets.h"
#include "ui_bar.h"
#include "ui_launcher.h"
#include "ui_settings.h"

static const char *TAG = "UI";

#define REFRESH_MS 500

static struct {
    lv_obj_t   *chip;
    lv_obj_t   *name;
    lv_obj_t   *link_text;
    lv_obj_t   *link_detail;
    lv_obj_t   *problem;
    lv_obj_t   *time_value;
    lv_obj_t   *time_hint;
    lv_obj_t   *people;
    lv_obj_t   *nodes;
    lv_obj_t   *memory;
    lv_obj_t   *test_result;
    lv_obj_t   *bar;
    lv_obj_t   *screen;
    bool        shown;
    hh_status_t last;          /* snapshot currently on screen */
} s_ui;

static const char *node_label(const hh_status_t *st, int node, char *buf, size_t cap)
{
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        if (st->nodes[i].node == node) {
            return st->nodes[i].ssid;
        }
    }
    snprintf(buf, cap, "AP %d", node);
    return buf;
}

static const char *signal_word(int rssi)
{
    return rssi >= -60 ? "strong" : rssi >= -72 ? "good" : rssi >= -80 ? "weak" : "poor";
}

static void on_home(lv_event_t *e)
{
    (void)e;
    ui_launcher_open();
}

/* The test runs for tens of milliseconds, which the drawing task cannot spend, so the
 * application task takes it and this card shows the result it leaves behind. */
static void on_run_selftest(lv_event_t *e)
{
    (void)e;
    ui_settings_run_selftest();
}

static void on_node_clicked(lv_event_t *e)
{
    int node = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "[UI] Node choice tapped: %d", node);
    hh_service_prefer_node(node);
}

static void choice_row(lv_obj_t *parent, const char *left, const char *right, bool selected, int node)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *b = lv_button_create(parent);
    lg_theme_style_button(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, t->touch_min, 0);
    lv_obj_set_style_pad_all(b, t->gap, 0);
    lv_obj_set_style_pad_column(b, t->gap, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_color(b, selected ? t->accent : t->outline, 0);
    lv_obj_set_style_border_width(b, selected ? t->stroke : t->hairline, 0);
    lv_obj_t *l = lg_ui_text(b, t->font_body, t->text, left);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lg_ui_text(b, t->font_small, selected ? t->accent : t->muted, right);
    lv_obj_add_event_cb(b, on_node_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)node);
}

static void person_row(lv_obj_t *parent, const char *left, const char *right, lv_color_t right_color)
{
    const lg_theme_t *t = lg_theme();
    lv_obj_t *row = lg_ui_row(parent);
    lv_obj_t *l = lg_ui_text(row, t->font_body, t->text, left);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lg_ui_text(row, t->font_small, right_color, right);
}

static bool lists_changed(const hh_status_t *a, const hh_status_t *b)
{
    return a->n_people != b->n_people || a->n_nodes != b->n_nodes || a->node != b->node || a->link != b->link ||
           a->preferred_node != b->preferred_node ||
           memcmp(a->people, b->people, sizeof(a->people[0]) * a->n_people) != 0 ||
           memcmp(a->nodes, b->nodes, sizeof(a->nodes[0]) * a->n_nodes) != 0;
}

static void rebuild_lists(const hh_status_t *st)
{
    const lg_theme_t *t = lg_theme();
    char buf[24];
    char right[48];

    lv_obj_clean(s_ui.people);
    if (st->n_people == 0) {
        lg_ui_label(s_ui.people, t->font_small, t->muted, "No other handheld seen yet");
    }
    for (uint8_t i = 0; i < st->n_people; i++) {
        const hh_person_t *p = &st->people[i];
        if (!p->online) {
            person_row(s_ui.people, p->name, "offline", t->muted);
        } else if ((int)p->node == st->node) {
            person_row(s_ui.people, p->name, "on your AP", t->success);
        } else {
            snprintf(right, sizeof(right), "online, %s", node_label(st, p->node, buf, sizeof(buf)));
            person_row(s_ui.people, p->name, right, t->success);
        }
    }

    lv_obj_clean(s_ui.nodes);
    choice_row(s_ui.nodes, "Automatic", "best signal", st->preferred_node < 0, -1);
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        const hh_node_seen_t *n = &st->nodes[i];
        if (st->link == HH_LINK_ONLINE && st->node == (int)n->node) {
            snprintf(right, sizeof(right), "connected");
        } else if (!n->backbone) {
            snprintf(right, sizeof(right), "%d dBm, no links", n->rssi);
        } else {
            snprintf(right, sizeof(right), "%d dBm", n->rssi);
        }
        choice_row(s_ui.nodes, n->ssid, right, st->preferred_node == (int)n->node, n->node);
    }
    if (st->n_nodes == 0) {
        lg_ui_label(s_ui.nodes, t->font_small, t->muted, "No APs heard yet");
    }
}

lv_obj_t *ui_home_screen(void)
{
    return s_ui.screen;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    static hh_status_t st;
    hh_service_status(&st);
    if (lv_screen_active() != s_ui.screen && s_ui.shown) {
        return;   /* another screen is up; nothing to repaint here */
    }
    if (s_ui.shown && st.version == s_ui.last.version) {
        return;
    }
    const lg_theme_t *t = lg_theme();
    char text[96];

    lg_ui_set_text(s_ui.name, st.name[0] != '\0' ? st.name : "Not in the grid roster");

    const char *chip;
    lv_color_t chip_color = t->warning;
    switch (st.link) {
    case HH_LINK_ONLINE:
        chip = "Online";
        chip_color = t->success;
        snprintf(text, sizeof(text), "Online on %s", st.node_ssid);
        break;
    case HH_LINK_REGISTERING:
        chip = "Joining";
        snprintf(text, sizeof(text), "Registering with %s", st.node_ssid);
        break;
    case HH_LINK_CONNECTING:
        chip = "Joining";
        snprintf(text, sizeof(text), "Joining %s", st.node_ssid);
        break;
    case HH_LINK_SEARCHING:
        chip = "Searching";
        snprintf(text, sizeof(text), "Looking for APs");
        break;
    default:
        chip = st.problem[0] != '\0' ? "Stopped" : "Starting";
        chip_color = st.problem[0] != '\0' ? t->error : t->warning;
        snprintf(text, sizeof(text), "%s", st.problem[0] != '\0' ? "Not connected" : "Starting");
        break;
    }
    lg_ui_set_text(s_ui.chip, chip);
    lv_obj_set_style_text_color(s_ui.chip, chip_color, 0);
    lg_ui_set_text(s_ui.link_text, text);

    if (st.link == HH_LINK_ONLINE || st.link == HH_LINK_REGISTERING) {
        snprintf(text, sizeof(text), "Signal %d dBm (%s), address %u.%u.%u.%u", st.rssi, signal_word(st.rssi),
                 st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
    } else {
        text[0] = '\0';
    }
    lg_ui_set_text(s_ui.link_detail, text);
    lg_ui_set_text(s_ui.problem, st.problem);
    if (st.problem[0] != '\0') {
        lv_obj_remove_flag(s_ui.problem, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.problem, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.link != HH_LINK_ONLINE) {
        lg_ui_set_text(s_ui.time_value, "Unknown");
        lg_ui_set_text(s_ui.time_hint, "Grid time arrives when this handheld joins an AP.");
        lv_obj_set_style_text_color(s_ui.time_hint, t->muted, 0);
    } else if (st.time_restricted) {
        lg_ui_set_text(s_ui.time_value, "Not set");
        lg_ui_set_text(s_ui.time_hint, "The admin has not set grid time. Only urgent broadcasts can be sent.");
        lv_obj_set_style_text_color(s_ui.time_hint, t->warning, 0);
    } else {
        uint32_t day = st.grid_time % 86400u;
        snprintf(text, sizeof(text), "%02u:%02u:%02u UTC", (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u),
                 (unsigned)(day % 60u));
        lg_ui_set_text(s_ui.time_value, text);
        lg_ui_set_text(s_ui.time_hint, "");
    }

    if (!s_ui.shown || lists_changed(&st, &s_ui.last)) {
        rebuild_lists(&st);
    }

    snprintf(text, sizeof(text), "Memory %u KB free, %u KB lowest", (unsigned)(st.free_heap / 1024u),
             (unsigned)(st.min_free_heap / 1024u));
    lg_ui_set_text(s_ui.memory, text);

    const lg_selftest_result_t *test = lg_selftest_last();
    lg_ui_set_text(s_ui.test_result, lg_selftest_summary());
    lv_obj_set_style_text_color(s_ui.test_result,
                                test == NULL ? t->muted : (test->failures == 0 ? t->success : t->error), 0);
    ui_bar_update(s_ui.bar, &st);

    s_ui.last = st;
    s_ui.shown = true;
}

void ui_home_build(const lg_identity_t *identity)
{
    const lg_theme_t *t = lg_theme();
    lg_display_lock(1000);
    lv_obj_t *scr = lv_obj_create(NULL);
    lg_theme_apply_screen(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, t->pad, 0);
    lv_obj_set_style_pad_row(scr, t->gap, 0);

    s_ui.bar = ui_bar_create(scr, "Status", on_home);
    s_ui.chip = lg_ui_text(scr, t->font_small, t->warning, "Starting");

    lv_obj_t *me = lg_ui_card(scr, "This handheld");
    s_ui.name = lg_ui_label(me, t->font_body, t->text, "");
    lg_ui_label(me, t->font_small, t->muted, identity->present ? identity->id : "Not provisioned");

    lv_obj_t *link = lg_ui_card(scr, "Connection");
    s_ui.link_text = lg_ui_label(link, t->font_body, t->text, "");
    s_ui.link_detail = lg_ui_label(link, t->font_small, t->muted, "");
    s_ui.problem = lg_ui_label(link, t->font_small, t->error, "");

    lv_obj_t *time = lg_ui_card(scr, "Grid time");
    s_ui.time_value = lg_ui_label(time, t->font_title, t->text, "");
    s_ui.time_hint = lg_ui_label(time, t->font_small, t->muted, "");

    lv_obj_t *people = lg_ui_card(scr, "Handhelds on the grid");
    s_ui.people = lg_ui_column(people, t->gap);

    lv_obj_t *nodes = lg_ui_card(scr, "APs in range");
    lg_ui_label(nodes, t->font_small, t->muted, "Tap an AP to use only that one, or Automatic for the best signal.");
    s_ui.nodes = lg_ui_column(nodes, t->gap);

    lv_obj_t *test = lg_ui_card(scr, "Self test");
    s_ui.test_result = lg_ui_label(test, t->font_body, t->muted, "");
    lg_ui_label(test, t->font_small, t->muted,
                "Checks the message format and the encryption against the published test vectors.");
    lg_ui_button(test, "Run", on_run_selftest, NULL);

    s_ui.memory = lg_ui_label(scr, t->font_small, t->muted, "");

    s_ui.screen = scr;
    lv_timer_create(refresh, REFRESH_MS, NULL);
    lg_display_unlock();
    ESP_LOGI(TAG, "[UI] Status screen ready");
}

void ui_home_open(void)
{
    if (s_ui.screen == NULL) {
        return;
    }
    lg_display_lock(1000);
    s_ui.shown = false;   /* repaint from the current snapshot */
    lv_screen_load(s_ui.screen);
    lg_display_unlock();
}
