/*
 * Serial console for the handheld (decision D28): every device answers status and
 * configuration questions over its serial port, the way `id` does at flash time.
 *
 * Commands read a status snapshot and post changes through hh_service.h, so the
 * network task stays the only owner of Wi-Fi and lg_client state.
 */
#include "hh_console.h"
#include "hh_lora.h"
#include "hh_demo.h"
#include "ui_theme.h"
#include "hh_mem.h"
#include "lg_power.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lg_envelope.h"
#include "ui_main.h"
#include "ui_nav.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_bsp_touch.h"
#include "lg_draw.h"
#include "lg_bsp_settings.h"
#include "lg_identity.h"


/*
 * One status copy and one message list for the whole console. Commands run one at a time on
 * the console task, so they can share them: each command used to keep its own static copy,
 * nine in all, about 21 KB of RAM on a board that has none to spare.
 */
static hh_status_t  s_console_status;
static hh_message_t s_console_msg;   /* one at a time: a copy of every message cost 6.7 KB */

static const hh_status_t *console_status(void)
{
    hh_service_status(&s_console_status);
    return &s_console_status;
}

static const lg_identity_t *s_identity;

static const char *link_word(hh_link_t link)
{
    switch (link) {
    case HH_LINK_SEARCHING:    return "SEARCHING";
    case HH_LINK_CONNECTING:   return "CONNECTING";
    case HH_LINK_REGISTERING:  return "REGISTERING";
    case HH_LINK_ONLINE:       return "ONLINE";
    default:                   return "STOPPED";
    }
}

static const char *signal_word(int rssi)
{
    return rssi >= -60 ? "strong" : rssi >= -72 ? "good" : rssi >= -80 ? "weak" : "poor";
}

static void print_time(const hh_status_t *st)
{
    if (st->time_restricted || st->grid_time == 0) {
        printf("  grid time: not set (receiving and urgent broadcasts only, decision D6)\n");
    } else {
        uint32_t day = st->grid_time % 86400u;
        struct tm lt;
        hh_local_time(st->grid_time, &lt);
        printf("  grid time: %02u:%02u:%02u UTC (%" PRIu32 "), local %04d-%02d-%02d %02d:%02d:%02d%s\n",
               (unsigned)(day / 3600u), (unsigned)(day / 60u % 60u), (unsigned)(day % 60u), st->grid_time,
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
               st->time_from_gps ? ", kept by a GPS" : "");
    }
    printf("  time zone: %s\n", st->time_zone[0] != '\0' ? st->time_zone : "none yet (clocks show UTC)");
}

static int cmd_tone(int argc, char **argv)
{
    uint32_t hz = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1000u;
    uint32_t ms = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 300u;
    if (!lg_bsp_audio_available()) {
        printf("No sound: this board has no speaker, or its codec did not start\n");
        return 0;
    }
    printf("Playing %" PRIu32 " Hz for %" PRIu32 " ms\n", hz, ms);
    esp_err_t err = lg_bsp_audio_tone(hz, ms);
    if (err != ESP_OK) {
        printf("Not played: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_cue(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "received";
    lg_cue_t cue = LG_CUE_RECEIVED;
    if (strcmp(which, "sent") == 0) {
        cue = LG_CUE_SENT;
    } else if (strcmp(which, "urgent") == 0) {
        cue = LG_CUE_URGENT;
    } else if (strcmp(which, "announce") == 0) {
        cue = LG_CUE_ANNOUNCE;
    } else if (strcmp(which, "received") != 0) {
        printf("cue <sent|received|announce|urgent>\n");
        return 1;
    }
    if (!lg_bsp_audio_available()) {
        printf("No sound: this board has no speaker, or its codec did not start\n");
        return 0;
    }
    printf("Playing the %s cue\n", which);
    esp_err_t err = lg_bsp_audio_cue(cue);
    if (err != ESP_OK) {
        printf("Not played: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t found[16];
    int n = lg_bsp_i2c_scan(found, sizeof(found));
    if (n <= 0) {
        printf("Nothing answered on the I2C bus%s\n", n == 0 ? " (or this board has none open)" : "");
        return 0;
    }
    printf("I2C addresses answering: ");
    for (int i = 0; i < n; i++) {
        printf("0x%02X ", found[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_volume(int argc, char **argv)
{
    if (argc > 1) {
        const char *want = argv[1];
        uint8_t level = LG_VOLUME_STEPS;
        for (uint8_t i = 0; i < LG_VOLUME_STEPS; i++) {
            if (strcmp(want, lg_bsp_audio_volume_name(i)) == 0) {
                level = i;
                break;
            }
        }
        if (level == LG_VOLUME_STEPS) {
            printf("volume <off|low|medium|high>, or volume alone to show the setting\n");
            return 1;
        }
        lg_bsp_audio_set_volume(level);   /* kept in NVS, so it survives a reboot */
    }
    printf("Volume: %s%s\n", lg_bsp_audio_volume_name(lg_bsp_audio_volume()),
           lg_bsp_audio_muted() ? " (urgent broadcasts still sound)" : "");
    return 0;
}

static int cmd_saver(int argc, char **argv)
{
    if (argc > 1) {
        bool on = strcmp(argv[1], "on") == 0;
        if (!on && strcmp(argv[1], "off") != 0) {
            printf("saver <on|off>, or saver alone to show the setting\n");
            return 1;
        }
        lg_bsp_setting_set_bool("saver", on);   /* kept in NVS, so it survives a reboot */
    }
    printf("Screen saver: %s\n", lg_bsp_setting_get_bool("saver", true) ? "on" : "off");
    return 0;
}

static int cmd_id(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lg_identity_print(s_identity);   /* identity is read-only after boot */
    return 0;
}

static const char *demo_step_name(hh_demo_step_t s)
{
    switch (s) {
    case HH_DEMO_IDLE:     return "idle";
    case HH_DEMO_LOOKING:  return "looking at the grid";
    case HH_DEMO_DIRECT:   return "1:1 message";
    case HH_DEMO_GROUP:    return "group message";
    case HH_DEMO_VOICE:    return "voice";
    case HH_DEMO_ANNOUNCE: return "announcement";
    case HH_DEMO_URGENT:   return "urgent and all clear";
    case HH_DEMO_POSITION: return "positions";
    case HH_DEMO_ROAM:     return "leaving and rejoining an AP";
    case HH_DEMO_DONE:     return "finished";
    }
    return "?";
}

static int cmd_demo(int argc, char **argv)
{
    hh_demo_state_t st;
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        hh_demo_stop();
        printf("stopping at the next step\n");
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "showcase") == 0 || strcmp(argv[1], "resilience") == 0)) {
        bool hard = strcmp(argv[1], "resilience") == 0;
        esp_err_t err = hh_demo_run(hard ? HH_DEMO_RESILIENCE : HH_DEMO_SHOWCASE);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("not on the grid yet: nothing could be demonstrated\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("a demo is already running; 'demo stop' ends it\n");
            return 1;
        }
        printf("%s demo started; it runs by itself and shows on the screen\n", argv[1]);
        return 0;
    }
    if (argc >= 2) {
        printf("usage: demo [showcase|resilience|stop]\n");
        return 1;
    }
    hh_demo_state(&st);
    printf("demo %s", st.running ? "running" : "idle");
    if (st.steps > 0) {
        printf(", %s, step %u of %u, %u shown, %u skipped",
               demo_step_name(st.step), st.step_n, st.steps, st.done, st.skipped);
    }
    printf("\n");
    if (st.note[0] != '\0') {
        printf("  %s\n", st.note);
    }
    return 0;
}

static int cmd_theme(int argc, char **argv)
{
    if (argc >= 2) {
        bool day = strcmp(argv[1], "day") == 0 || strcmp(argv[1], "daylight") == 0;
        if (!day && strcmp(argv[1], "night") != 0 && strcmp(argv[1], "dark") != 0) {
            printf("usage: theme [night|day]\n");
            return 1;
        }
        ui_theme_set(day ? UI_THEME_DAYLIGHT : UI_THEME_NIGHT);
    }
    printf("theme: %s (kept across restarts)\n", ui_theme_name(ui_theme_kind()));
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t had = hh_service_clear_messages();
    printf("cleared %u message(s)\n", (unsigned)had);
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    hh_mem_mark("on request");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const hh_status_t *st = console_status();
    printf("Handheld status\n");
    printf("  device %" PRIu32 " (%s), id %s\n", st->device, st->name[0] ? st->name : "not in roster",
           s_identity->present ? s_identity->id : "none");
    printf("  link: %s", link_word(st->link));
    if (st->node >= 0) {
        printf(" on %s (node %d)", st->node_ssid[0] ? st->node_ssid : "node", st->node);
    }
    printf("\n");
    if (st->link == HH_LINK_ONLINE || st->link == HH_LINK_REGISTERING) {
        printf("  signal: %d dBm (%s), address %u.%u.%u.%u\n", st->rssi, signal_word(st->rssi), st->ip[0], st->ip[1],
               st->ip[2], st->ip[3]);
    }
    print_time(st);
    if (st->preferred_node < 0) {
        printf("  node choice: automatic\n");
    } else {
        printf("  node choice: fixed to node %d\n", st->preferred_node);
    }
    printf("  registrations since boot: %" PRIu32 "\n", st->joins);
    /* Live figures, not the snapshot's: the service has not published one yet in the first
     * seconds after a boot, and console.py resets the board before it asks, so this line was
     * reporting "0 KB free" on a board with 91 KB. */
    printf("  memory: %" PRIu32 " KB free, %" PRIu32 " KB lowest (internal RAM)\n",
           (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u),
           (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        printf("  PSRAM: %" PRIu32 " KB free of %" PRIu32 " KB, %" PRIu32 " KB lowest\n",
               (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u),
               (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024u),
               (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024u));
    } else {
        printf("  PSRAM: none on this board\n");
    }
    /* Screens are built and drawn on the UI task. The panic says a stack overflowed but never
     * how close the boards that survive are, which is the figure worth watching. */
    printf("  UI task: %" PRIu32 " bytes of stack unused at its worst\n", ui_stack_headroom());
    /* Touch state, because it decides whether a press can happen at all: a resistive panel
     * maps no coordinates until it is calibrated, so an uncalibrated board cannot report a
     * press -- which is the difference between "a stray touch pressed something" and "a
     * stray touch was impossible". There was no way to ask this over serial (D28). */
    printf("  touch: %s\n", !lg_bsp_touch_can_calibrate() ? "no calibration needed"
                            : lg_bsp_touch_needs_calibration() ? "NOT calibrated; presses are ignored"
                                                               : "calibrated");
    printf("  problem: %s\n", st->problem[0] ? st->problem : "none");
    return 0;
}

static int cmd_nodes(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const hh_status_t *st = console_status();
    printf("Nodes in range: %u\n", st->n_nodes);
    printf("  AP    NAME              RSSI  ATTACHED  BACKBONE  TIME  STATE\n");
    for (uint8_t i = 0; i < st->n_nodes; i++) {
        const hh_node_seen_t *n = &st->nodes[i];
        const char *state = st->link == HH_LINK_ONLINE && st->node == (int)n->node ? "connected"
                            : st->preferred_node == (int)n->node                  ? "chosen"
                                                                                 : "";
        printf("  %-4u  %-16s  %-4d  %-8u  %-8s  %-4s  %s\n", n->node, n->ssid, n->rssi, n->clients,
               n->backbone ? "yes" : "no", n->has_time ? "yes" : "no", state);
    }
    return 0;
}

static int cmd_people(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const hh_status_t *st = console_status();
    printf("Handhelds this device has heard about: %u\n", st->n_people);
    printf("  DEVICE  NAME              STATE\n");
    for (uint8_t i = 0; i < st->n_people; i++) {
        const hh_person_t *p = &st->people[i];
        if (!p->online) {
            printf("  %-6" PRIu32 "  %-16s  offline\n", p->device, p->name);
        } else if ((int)p->node == st->node) {
            printf("  %-6" PRIu32 "  %-16s  online, on your node\n", p->device, p->name);
        } else {
            printf("  %-6" PRIu32 "  %-16s  online on node %u\n", p->device, p->name, p->node);
        }
    }
    return 0;
}

static int cmd_node(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "auto") == 0) {
        hh_service_prefer_node(-1);
        printf("node choice: automatic\n");
        return 0;
    }
    if (argc == 2) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end != argv[1] && *end == '\0' && v >= 0 && v < HH_MAX_NODES) {
            hh_service_prefer_node((int)v);
            printf("node choice: node %ld; reconnecting if needed\n", v);
            return 0;
        }
    }
    printf("usage: node <index> | node auto\n");
    return 1;
}

static int cmd_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    hh_service_scan_now();
    printf("scanning; run 'nodes' in a second\n");
    return 0;
}

static int cmd_reconnect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    hh_service_reconnect();
    printf("dropping the session and joining again\n");
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const hh_status_t *st = console_status();
    print_time(st);
    printf("  a handheld never sets grid time; the admin sets it on the master's page (decision D6)\n");
    return 0;
}

static void print_target(const hh_message_t *m, const hh_status_t *st)
{
    if (m->scope == LG_SCOPE_BROADCAST) {
        printf("everyone");
        return;
    }
    if (m->scope == LG_SCOPE_GROUP) {
        for (uint8_t i = 0; i < st->n_groups; i++) {
            if (st->groups[i].id == (uint16_t)m->target) {
                printf("group %s", st->groups[i].name);
                return;
            }
        }
        printf("group %" PRIu32, m->target);
        return;
    }
    printf("device %" PRIu32, m->target);
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

static int cmd_msgs(int argc, char **argv)
{
    size_t want = 10;
    if (argc == 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v > 0 && v <= HH_MESSAGES) {
            want = (size_t)v;
        }
    }
    const hh_status_t *st = console_status();
    size_t n = 0;
    while (n < want && hh_service_message(n, &s_console_msg)) {
        n++;
    }
    printf("Messages, newest first: %u\n", (unsigned)n);
    for (size_t i = 0; i < n && hh_service_message(i, &s_console_msg); i++) {
        const hh_message_t *m = &s_console_msg;
        if (m->mine) {
            printf("  to ");
            print_target(m, st);
        } else {
            printf("  from %s (device %" PRIu32 ")", person_name(st, m->author), m->author);
            if (m->scope == LG_SCOPE_GROUP || m->scope == LG_SCOPE_BROADCAST) {
                printf(" to ");
                print_target(m, st);
            }
        }
        /*
         * Group and broadcast markers are counts rather than a state (D42), and the state
         * text says only "sent" for them. Without the counts here the screens would be the
         * only place they appear, which would leave them untestable over serial (D28).
         */
        char counts[48] = "";
        if (m->mine && m->scope != LG_SCOPE_DIRECT) {
            snprintf(counts, sizeof(counts), ", delivered %u, read %u", (unsigned)m->delivered_count,
                     (unsigned)m->read_count);
        }
        printf("%s [%s%s]: %s\n", m->urgent ? " URGENT" : "", hh_message_state_text(m), counts, m->text);
    }
    return 0;
}

static int cmd_groups(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const hh_status_t *st = console_status();
    printf("Groups, version %" PRIu32 ": %u\n", st->groups_version, st->n_groups);
    printf("  ID  NAME              THIS HANDHELD  MEMBERS\n");
    for (uint8_t i = 0; i < st->n_groups; i++) {
        printf("  %-2u  %-16s  %-13s ", st->groups[i].id, st->groups[i].name,
               st->groups[i].member ? "member" : "not a member");
        for (uint32_t d = 1; d <= 32u; d++) {
            if (st->groups[i].member_devices & (1u << (d - 1u))) {
                printf(" %" PRIu32, d);
            }
        }
        printf("\n");
    }
    if (st->group_problem[0] != '\0') {
        printf("Last group edit refused: %s\n", st->group_problem);
    }
    return 0;
}

/* group new <name> <devices> | group set <id> <name> <devices> | group rm <id>; devices "1,2,3" */
static int cmd_group(int argc, char **argv)
{
    uint32_t members = 0;
    const char *devices = argc >= 4 ? argv[argc - 1] : NULL;
    for (const char *p = devices; p != NULL && *p != '\0';) {
        char *end = NULL;
        unsigned long d = strtoul(p, &end, 10);
        if (end == p || d < 1 || d > 32) {
            printf("devices are handheld numbers separated by commas, e.g. 1,2\n");
            return 1;
        }
        members |= 1u << (d - 1u);
        p = *end == ',' ? end + 1 : end;
    }
    esp_err_t err;
    if (argc == 4 && strcmp(argv[1], "new") == 0) {
        err = hh_service_edit_group(0, argv[2], members, false);
    } else if (argc == 5 && strcmp(argv[1], "set") == 0) {
        err = hh_service_edit_group((uint16_t)strtoul(argv[2], NULL, 10), argv[3], members, false);
    } else if (argc == 3 && strcmp(argv[1], "rm") == 0) {
        err = hh_service_edit_group((uint16_t)strtoul(argv[2], NULL, 10), NULL, 0, true);
    } else {
        printf("usage: group new <name> <devices> | group set <id> <name> <devices> | group rm <id>\n");
        return 1;
    }
    if (err == ESP_OK) {
        printf("sent to the AP; run 'groups' for the result\n");
    } else {
        printf("not sent: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

/* send <device index | group name | all | urgent> <text...> */
static int cmd_send(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: send <device index | group name | all | urgent> <text>\n");
        return 1;
    }
    const hh_status_t *st = console_status();

    uint8_t scope = LG_SCOPE_DIRECT;
    uint32_t target = 0;
    bool urgent = false;
    const char *who = argv[1];
    char *end = NULL;
    long device = strtol(who, &end, 10);
    if (end != who && *end == '\0' && device > 0) {
        target = (uint32_t)device;
    } else if (strcmp(who, "all") == 0 || strcmp(who, "urgent") == 0) {
        scope = LG_SCOPE_BROADCAST;
        urgent = strcmp(who, "urgent") == 0;
    } else {
        int found = -1;
        for (uint8_t i = 0; i < st->n_groups; i++) {
            if (strcmp(st->groups[i].name, who) == 0) {
                found = st->groups[i].id;
            }
        }
        if (found < 0) {
            printf("unknown target '%s'; run 'groups' or 'people'\n", who);
            return 1;
        }
        scope = LG_SCOPE_GROUP;
        target = (uint32_t)found;
    }

    char text[HH_TEXT_MAX + 1] = { 0 };
    size_t len = 0;
    for (int i = 2; i < argc && len < HH_TEXT_MAX; i++) {
        if (i > 2) {
            text[len++] = ' ';
        }
        size_t part = strlen(argv[i]);
        if (len + part > HH_TEXT_MAX) {
            part = HH_TEXT_MAX - len;
        }
        memcpy(text + len, argv[i], part);
        len += part;
    }
    esp_err_t err = hh_service_send(scope, target, urgent, text);
    if (err != ESP_OK) {
        printf("not queued: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("queued; run 'msgs' to see whether the grid accepted it\n");
    return 0;
}

/* chat <device index | group name | all>: opens that conversation on the screen, so the
 * screens can be driven from serial while testing (decisions D23, D25, D28). */
static int cmd_chat(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: chat <device index | group name | all>\n");
        return 1;
    }
    const hh_status_t *st = console_status();
    const char *who = argv[1];
    char *end = NULL;
    long device = strtol(who, &end, 10);
    if (strcmp(who, "all") == 0) {
        ui_open_chat(LG_SCOPE_BROADCAST, LG_TARGET_ALL, "Everyone");
        printf("opened Everyone\n");
        return 0;
    }
    if (end != who && *end == '\0' && device > 0) {
        ui_open_chat(LG_SCOPE_DIRECT, (uint32_t)device, person_name(st, (uint32_t)device));
        printf("opened the chat with device %ld\n", device);
        return 0;
    }
    for (uint8_t i = 0; i < st->n_groups; i++) {
        if (strcmp(st->groups[i].name, who) == 0) {
            ui_open_chat(LG_SCOPE_GROUP, st->groups[i].id, st->groups[i].name);
            printf("opened group %s\n", st->groups[i].name);
            return 0;
        }
    }
    printf("unknown conversation '%s'; run 'groups' or 'people'\n", who);
    return 1;
}

/* ui tap <x> <y> | scroll <px> | kb <on|off> | type <word> | page <...> | log: drive the screens
 * from serial while testing (D23, D25, D28). */
static int cmd_ui(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "scroll") == 0) {
        ui_request_cmd(UI_SCROLL, (int)strtol(argv[2], NULL, 10), NULL);
    } else if (argc == 3 && strcmp(argv[1], "kb") == 0) {
        ui_request_cmd(UI_KEYBOARD, strcmp(argv[2], "on") == 0, NULL);
    } else if (argc >= 3 && strcmp(argv[1], "type") == 0) {
        ui_request_cmd(UI_TYPE, 0, argv[2]);
    } else if (argc >= 2 && strcmp(argv[1], "log") == 0) {
        ui_request_cmd(UI_LOG, 0, NULL);
    } else if (argc == 4 && strcmp(argv[1], "tap") == 0) {
        long x = strtol(argv[2], NULL, 10);
        long y = strtol(argv[3], NULL, 10);
        ui_request_cmd(UI_TAP, (int)((x << 16) | (y & 0xFFFF)), NULL);
    } else if (argc == 3 && strcmp(argv[1], "page") == 0) {
        int page = strcmp(argv[2], "numbers") == 0 ? 1 : strcmp(argv[2], "emoji") == 0 ? 2
                 : strcmp(argv[2], "shift") == 0 ? 3 : 0;
        ui_request_cmd(UI_PAGE, page, NULL);
    } else if (argc == 4 && strcmp(argv[1], "button") == 0) {
        /* D66: a board button pressed or held, as if by hand (the alert unit's SOS, Cancel, Read) */
        long id = strtol(argv[2], NULL, 10);
        ui_request_cmd(UI_BUTTON, (int)((id << 1) | (strcmp(argv[3], "hold") == 0 ? 1 : 0)), NULL);
    } else {
        printf("usage: ui tap <x> <y> | scroll <px> | kb <on|off> | type <word> | page <letters|numbers|emoji|shift> | "
               "button <n> <press|hold> | log\n");
        return 1;
    }
    return 0;
}

/* screen <home|status|messages|groups|settings>: opens that screen, the way `chat` opens a
 * conversation, so every screen can be reached from serial (D23, D25, D28). */
/*
 * touch [seconds]: prints what the panel reports against what the screens are told, so a finger
 * landing on the wrong key can be read as numbers instead of guessed at (D23, D28). One line per
 * press: the controller's own reading, then the screen pixel it maps to.
 */
static int cmd_touch(int argc, char **argv)
{
    if (!lg_bsp_touch_present()) {
        printf("This board has no touch panel\n");
        return 0;
    }
    uint32_t seconds = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 10u;
    if (seconds == 0 || seconds > 60u) {
        seconds = 10u;
    }
    printf("Tap the screen for %" PRIu32 " s (a corner at a time tells the most)\n", seconds);
    uint32_t end = (uint32_t)(esp_timer_get_time() / 1000) + seconds * 1000u;
    int16_t last_x = -1;
    int16_t last_y = -1;
    uint32_t presses = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < end) {
        lg_bsp_touch_raw_t raw;
        if (lg_bsp_touch_read_raw(&raw)) {
            int16_t x = 0;
            int16_t y = 0;
            bool mapped = lg_bsp_touch_map(&raw, &x, &y);
            if (raw.x != last_x || raw.y != last_y) {
                last_x = raw.x;
                last_y = raw.y;
                presses++;
                if (mapped) {
                    printf("  raw %4d,%4d -> screen %3d,%3d\n", raw.x, raw.y, x, y);
                } else {
                    printf("  raw %4d,%4d -> not mapped (uncalibrated)\n", raw.x, raw.y);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("%" PRIu32 " sample(s); the screen is %u by %u\n", presses, (unsigned)lg_draw_width(),
           (unsigned)lg_draw_height());
    return 0;
}

static int cmd_screen(int argc, char **argv)
{
    static const struct {
        const char *name;
        ui_nav_t    nav;
    } SCREENS[] = {
        { "home", NAV_HOME },     { "status", NAV_STATUS },     { "messages", NAV_CONVERSATIONS },
        { "groups", NAV_GROUPS }, { "settings", NAV_SETTINGS },
    };
    for (size_t i = 0; argc == 2 && i < sizeof(SCREENS) / sizeof(SCREENS[0]); i++) {
        if (strcmp(argv[1], SCREENS[i].name) == 0) {
            ui_request_cmd(UI_GO, (int)SCREENS[i].nav, NULL);
            printf("opened %s\n", SCREENS[i].name);
            return 0;
        }
    }
    printf("usage: screen <home|status|messages|groups|settings>\n");
    return 1;
}

/* name [new name...]: shows this handheld's name, or renames it (D50). */
static int cmd_name(int argc, char **argv)
{
    if (argc < 2) {
        printf("%s\n", console_status()->name);
        return 0;
    }
    char name[HH_NAME_MAX] = { 0 };
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        size_t part = strlen(argv[i]) + (i > 1 ? 1u : 0u);
        if (len + part > HH_NAME_MAX - 1u) {
            printf("too long: at most %u bytes\n", (unsigned)(HH_NAME_MAX - 1));
            return 1;
        }
        len += (size_t)snprintf(name + len, sizeof(name) - len, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    esp_err_t err = hh_service_set_name(name);
    if (err != ESP_OK) {
        printf("not renamed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("renamed; every AP and handheld gets it when this handheld is online\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

/* ---------------------------------------------------------------------------------------- *
 * mic: the push-to-talk audio path on one board, for tuning and bring-up.
 *
 *   mic level [s]   peak and RMS of each 100 ms frame, for setting the gain (default 5 s)
 *   mic loop [s]    records s seconds (default 3), IMA ADPCM-encodes it into a bounded buffer,
 *                   then decodes and plays it back: microphone, codec, ADPCM and speaker, end
 *                   to end, with no radio in the way
 *   mic gain [0-7]  the microphone's digital gain in 6 dB steps (not kept across a reboot)
 *
 * Frames are the owner's PTT frames: 8 kHz mono, 100 ms, 800 samples, 400 bytes of ADPCM.
 * ---------------------------------------------------------------------------------------- */
#include <math.h>
#include "hh_adpcm.h"

#define MIC_FRAME       800    /* 100 ms at 8 kHz */
#define MIC_LOOP_MAX_S  10     /* 40 KB of ADPCM, in PSRAM where there is some */
#define MIC_READ_MS     300

/* Full scale as dBFS, with a floor so silence prints as a number. */
static float mic_dbfs(float v)
{
    return v < 1.0f ? -90.3f : 20.0f * log10f(v / 32767.0f);
}

/* One frame's loudest sample and RMS. */
static void mic_measure(const int16_t *pcm, size_t n, int32_t *peak, float *rms)
{
    int32_t p = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        p = v > p ? v : p;
        sum += (int64_t)pcm[i] * pcm[i];
    }
    *peak = p;
    *rms = n > 0 ? sqrtf((float)sum / (float)n) : 0.0f;
}

static uint32_t mic_seconds(int argc, char **argv, uint32_t fallback)
{
    uint32_t s = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : fallback;
    return s == 0 || s > MIC_LOOP_MAX_S ? fallback : s;
}

static int mic_level(uint32_t seconds, int16_t *frame)
{
    esp_err_t err = lg_bsp_audio_mic_start();
    if (err != ESP_OK) {
        printf("microphone would not start: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Speak at about 30 cm for %" PRIu32 " s (digital gain +%u dB). Aim for RMS -30 to -18 dBFS, "
           "peaks below -3.\n", seconds, (unsigned)lg_bsp_audio_mic_gain() * 6u);
    int32_t top = 0;
    for (uint32_t f = 0; f < seconds * 10u; f++) {
        int n = lg_bsp_audio_mic_read(frame, MIC_FRAME, MIC_READ_MS);
        if (n < 0) {
            printf("read failed: %s\n", esp_err_to_name(-n));
            break;
        }
        int32_t peak = 0;
        float rms = 0.0f;
        mic_measure(frame, (size_t)n, &peak, &rms);
        top = peak > top ? peak : top;
        printf("  %2" PRIu32 ".%" PRIu32 " s  peak %5" PRId32 " (%6.1f dBFS)  rms %7.1f (%6.1f dBFS)%s\n",
               f / 10u, f % 10u, peak, (double)mic_dbfs((float)peak), (double)rms, (double)mic_dbfs(rms),
               n < MIC_FRAME ? "  short" : "");
    }
    lg_bsp_audio_mic_stop();
    printf("loudest sample %" PRId32 " (%.1f dBFS)%s\n", top, (double)mic_dbfs((float)top),
           top >= 32000 ? ": clipping, turn the gain down" : top < 200 ? ": nearly silent, is the mic working?" : "");
    return 0;
}

static int mic_loop(uint32_t seconds, int16_t *frame)
{
    const uint32_t frames = seconds * 10u;
    const size_t bytes = (size_t)frames * (MIC_FRAME / 2u);
    uint8_t *adpcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (adpcm == NULL) {
        adpcm = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);   /* no PSRAM: try internal RAM */
    }
    if (adpcm == NULL) {
        printf("no room for %u bytes of ADPCM\n", (unsigned)bytes);
        return 1;
    }
    esp_err_t err = lg_bsp_audio_mic_start();
    if (err != ESP_OK) {
        printf("microphone would not start: %s\n", esp_err_to_name(err));
        heap_caps_free(adpcm);
        return 1;
    }
    printf("Recording %" PRIu32 " s: speak now\n", seconds);
    hh_adpcm_state_t enc = { 0, 0 };
    int32_t top = 0;
    uint32_t got = 0;
    int64_t t0 = esp_timer_get_time();
    for (; got < frames; got++) {
        int n = lg_bsp_audio_mic_read(frame, MIC_FRAME, MIC_READ_MS);
        if (n < 0) {
            printf("read failed: %s\n", esp_err_to_name(-n));
            break;
        }
        if (n < MIC_FRAME) {
            memset(frame + n, 0, (MIC_FRAME - (size_t)n) * sizeof(int16_t));
        }
        int32_t peak = 0;
        float rms = 0.0f;
        mic_measure(frame, MIC_FRAME, &peak, &rms);
        top = peak > top ? peak : top;
        hh_adpcm_encode(&enc, frame, MIC_FRAME, adpcm + (size_t)got * (MIC_FRAME / 2u));
    }
    lg_bsp_audio_mic_stop();
    int64_t t1 = esp_timer_get_time();
    printf("Recorded %" PRIu32 " frames in %" PRId64 " ms, loudest %" PRId32 " (%.1f dBFS), %u bytes of ADPCM\n",
           got, (t1 - t0) / 1000, top, (double)mic_dbfs((float)top), (unsigned)(got * (MIC_FRAME / 2u)));

    err = lg_bsp_audio_voice_start();
    if (err != ESP_OK) {
        printf("speaker would not open: %s\n", esp_err_to_name(err));
        heap_caps_free(adpcm);
        return 1;
    }
    printf("Playing back (volume %s)\n", lg_bsp_audio_volume_name(lg_bsp_audio_volume()));
    hh_adpcm_state_t dec = { 0, 0 };
    uint32_t played = 0;
    for (; played < got; played++) {
        hh_adpcm_decode(&dec, adpcm + (size_t)played * (MIC_FRAME / 2u), MIC_FRAME, frame);
        err = lg_bsp_audio_voice_write(frame, MIC_FRAME, 1000);
        if (err != ESP_OK) {
            printf("playback stopped: %s\n", esp_err_to_name(err));
            break;
        }
    }
    lg_bsp_audio_voice_stop();
    heap_caps_free(adpcm);
    printf("Played %" PRIu32 " of %" PRIu32 " frames in %" PRId64 " ms\n", played, got,
           (esp_timer_get_time() - t1) / 1000);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_mic(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (strcmp(what, "level") != 0 && strcmp(what, "loop") != 0 && strcmp(what, "gain") != 0) {
        printf("mic level [s] | mic loop [s] | mic gain [0-7]\n");
        return 1;
    }
    if (!lg_bsp_audio_can_record()) {
        printf("no microphone on this board\n");
        return 0;
    }
    if (strcmp(what, "gain") == 0) {
        if (argc > 2) {
            esp_err_t err = lg_bsp_audio_mic_gain_set((uint8_t)strtoul(argv[2], NULL, 10));
            if (err != ESP_OK) {
                printf("not set: %s (0 to 7)\n", esp_err_to_name(err));
                return 1;
            }
        }
        printf("mic digital gain %u (+%u dB) over +30 dB PGA and +4.5 dB ADC volume\n",
               (unsigned)lg_bsp_audio_mic_gain(), (unsigned)lg_bsp_audio_mic_gain() * 6u);
        return 0;
    }
    int16_t *frame = heap_caps_malloc(MIC_FRAME * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (frame == NULL) {
        printf("no room for a frame\n");
        return 1;
    }
    int rc = strcmp(what, "level") == 0 ? mic_level(mic_seconds(argc, argv, 5u), frame)
                                        : mic_loop(mic_seconds(argc, argv, 3u), frame);
    heap_caps_free(frame);
    return rc;
}

static esp_err_t mic_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "mic",
        .help = "mic level [s] | loop [s] | gain [0-7]: microphone level, record-and-play test, gain",
        .func = cmd_mic,
    };
    return esp_console_cmd_register(&cmd);
}
/* ------------------------------------------------------------------------- end of mic */

/* ---------------------------------------------------------------------------------------- *
 * gps: this handheld's GPS (D65), as MAIN's `gps` (D63).
 *
 *   gps        heard, fix, satellites, sentences, bad checksums, where the clock came from, and
 *              this handheld's own position: the only place it is ever printed
 *   gps raw    the next 600 bytes as they arrive (about two seconds at 9600 baud), garbage as <hex>
 * ---------------------------------------------------------------------------------------- */
#include "hh_gps.h"

/* Microdegrees as signed decimal degrees, without floating point. */
static void gps_print_deg(int32_t u)
{
    uint32_t a = u < 0 ? (uint32_t)(-(int64_t)u) : (uint32_t)u;
    printf("%s%" PRIu32 ".%06" PRIu32, u < 0 ? "-" : "", a / 1000000u, a % 1000000u);
}

static int cmd_gps(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "raw") == 0) {
        hh_gps_state_t st;
        hh_gps_state(&st);
        if (!st.started) {
            printf("GPS: this board has no GPS pin in its profile\n");
            return 1;
        }
        printf("GPS: %" PRIu32 " bytes received since boot; printing the next 600\n", hh_gps_bytes());
        hh_gps_dump(600);
        return 0;
    }
    if (argc >= 2) {
        printf("usage: gps [raw]\n");
        return 1;
    }
    hh_gps_state_t st;
    hh_gps_state(&st);
    if (!st.started) {
        printf("GPS: none (this board's profile has no GPS pin)\n");
    } else {
        char none[64];
        snprintf(none, sizeof(none), "none heard (not fitted, or check module TXD to GPIO%d)", st.rx_gpio);
        printf("GPS: %s, %u satellites, %" PRIu32 " sentences, %" PRIu32 " bad checksums\n",
               !st.heard ? none : !st.talking ? "silent now (was heard)" : st.fix ? "FIX" : "no fix yet",
               st.sats, st.sentences, st.bad);
        if (st.last_unix != 0) {
            printf("GPS: last fix %" PRIu32 ", %" PRIu32 " ms ago", st.last_unix, st.fix_age_ms);
            if (st.has_pos) {
                printf(", at ");
                gps_print_deg(st.lat_u);
                printf(", ");
                gps_print_deg(st.lon_u);
            }
            printf("\n");
        }
        /* D73: the schedule the grid set, and what handing the port back gives this handheld. */
        if (st.always) {
            printf("GPS: always on; the port is never released\n");
        } else if (st.phase == LG_GPS_PHASE_READING) {
            printf("GPS: reading now (every %u s)\n", st.interval_s);
        } else {
            printf("GPS: waiting, next reading in %" PRIu32 " s (every %u s)\n", st.next_in_s, st.interval_s);
        }
        printf("GPS: %" PRIu32 " readings, %" PRIu32 " with a fix", st.readings, st.fixes);
        if (st.last_ttf_ms != UINT32_MAX) {
            printf(", last fix took %" PRIu32 " ms", st.last_ttf_ms);
        }
        if (st.freed_bytes != 0) {
            printf(", %" PRIu32 " bytes of heap free between readings", st.freed_bytes);
        }
        printf("\n");
    }
    printf("Clock: from %s\n", hh_service_clock_source());
    return 0;
}

/*
 * D71/D76: the same `lora` the APs answer, minus what only an AP has. A handheld with no module,
 * or a board with no LoRa pins, says so in one line and does nothing.
 */
static int cmd_lora(int argc, char **argv)
{
    if (argc == 1) {
        hh_lora_print();
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        hh_lora_request_reset();
        printf("LoRa: resetting and reconfiguring the module; watch the [LORA] log\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        hh_lora_enable();
        printf("LoRa: on\n");
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        uint32_t seconds = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 60u;
        hh_lora_disable(seconds);
        printf("LoRa: off; it comes back on its own\n");
        return 0;
    }
    if (strcmp(argv[1], "force") == 0) {
        hh_lora_force_next();
        printf("LoRa: the next frame goes on the air whatever the policy would say\n");
        return 0;
    }
    if (strcmp(argv[1], "broadcast") == 0 && argc >= 3) {
        bool on = strcmp(argv[2], "on") == 0;
        hh_lora_set_broadcast(on);
        printf("LoRa: broadcast %s\n", on ? "on (one transmission to every AP)" : "off (the last AP heard)");
        return 0;
    }
    if (strcmp(argv[1], "at") == 0 && argc >= 3) {
        char cmd[64];
        size_t n = 0;
        for (int i = 2; i < argc && n + 1 < sizeof(cmd); i++) {
            n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "%s%s", i > 2 ? " " : "", argv[i]);
        }
        hh_lora_request_at(cmd);
        printf("LoRa: sent \"%s\"; the reply is logged at [LORA]\n", cmd);
        return 0;
    }
    printf("usage: lora | lora reset | lora off [seconds] | lora on | lora force | "
           "lora broadcast on|off | lora at <AT command>\n");
    return 1;
}

static esp_err_t lora_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "lora",
        .help = "lora [reset|off [s]|on|force|broadcast on|off|at <cmd>]: the optional LoRa module (D71, D76)",
        .func = cmd_lora,
    };
    return esp_console_cmd_register(&cmd);
}

static esp_err_t gps_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "gps",
        .help = "gps [raw]: this handheld's GPS, heard, fix, satellites, clock source (D65); raw dumps what arrives",
        .func = cmd_gps,
    };
    return esp_console_cmd_register(&cmd);
}
/* ------------------------------------------------------------------------- end of gps */

esp_err_t hh_console_start(const lg_identity_t *identity)
{
    s_identity = identity;
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "grid>";
    repl_config.max_cmdline_length = 128;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        return err;
    }
    const esp_console_cmd_t cmds[] = {
        { .command = "id",        .help = "Print this board's LocalGrid device ID",                  .func = cmd_id },
        { .command = "status",    .help = "Device, link, signal, grid time, memory, problem",        .func = cmd_status },
        { .command = "nodes",     .help = "Nodes in range with signal, load, and backbone health",   .func = cmd_nodes },
        { .command = "people",    .help = "Handhelds this device has heard about",                   .func = cmd_people },
        { .command = "node",      .help = "node <index> | node auto: choose which node to use",      .func = cmd_node },
        { .command = "scan",      .help = "Scan the grid channel now",                               .func = cmd_scan },
        { .command = "reconnect", .help = "Drop the node session and join again",                    .func = cmd_reconnect },
        { .command = "groups",    .help = "Groups, members, and whether this handheld belongs",      .func = cmd_groups },
        { .command = "group",     .help = "group new <name> <devices> | set <id> <name> <devices> | rm <id> (D52)", .func = cmd_group },
        { .command = "send",      .help = "send <device|group|all|urgent> <text>: send a message",   .func = cmd_send },
        { .command = "msgs",      .help = "msgs [count]: messages sent and received, newest first",  .func = cmd_msgs },
        { .command = "chat",      .help = "chat <device|group|all>: open that conversation on screen", .func = cmd_chat },
        { .command = "screen",    .help = "screen <home|status|messages|groups|settings>: open that screen", .func = cmd_screen },
        { .command = "touch",     .help = "touch [seconds]: what the panel reports and where it lands", .func = cmd_touch },
        { .command = "ui",        .help = "ui tap <x> <y> | scroll <px> | kb <on|off> | type <word> | page <p> | button <n> <press|hold> | log", .func = cmd_ui },
        { .command = "time",      .help = "Show grid time and the time restriction",                 .func = cmd_time },
        { .command = "tone",      .help = "tone [hz] [ms]: play one tone on the speaker",           .func = cmd_tone },
        { .command = "cue",       .help = "cue <sent|received|urgent>: play a notification sound",  .func = cmd_cue },
        { .command = "i2cscan",   .help = "Addresses answering on this board's I2C bus",            .func = cmd_i2cscan },
        { .command = "saver",     .help = "saver [on|off]: the screen saver, kept in NVS",         .func = cmd_saver },
        { .command = "volume",    .help = "volume [off|low|medium|high]: notification loudness, kept in NVS", .func = cmd_volume },
        { .command = "theme",     .help = "theme [night|day]: screen colours; day is for reading outdoors", .func = cmd_theme },
        { .command = "clear",     .help = "Clear every message this handheld is showing",            .func = cmd_clear },
        { .command = "demo",      .help = "demo [showcase|resilience|stop]: the self-running demonstration", .func = cmd_demo },
        { .command = "mem",       .help = "Heap now: free, lowest, largest block",                   .func = cmd_mem },
        { .command = "name",      .help = "name [new name]: show or change this handheld's name",     .func = cmd_name },
        { .command = "reboot",    .help = "Restart this handheld",                                   .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    (void)mic_register();                /* mic level | loop | gain, push-to-talk audio */
    (void)gps_register();                /* gps [raw], D65 */
    (void)lora_register();               /* lora ..., D71 and D76 */
    (void)lg_power_register_command();   /* power [-m s [-i ms] [-q]], shared with the APs */
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
