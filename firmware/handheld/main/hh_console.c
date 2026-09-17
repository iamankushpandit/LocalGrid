/*
 * Serial console for the handheld (decision D28): every device answers status and
 * configuration questions over its serial port, the way `id` does at flash time.
 *
 * Commands read a status snapshot and post changes through hh_service.h, so the
 * network task stays the only owner of Wi-Fi and lg_client state.
 */
#include "hh_console.h"
#include "hh_mem.h"
#include "lg_power.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lg_envelope.h"
#include "ui_chat.h"
#include "ui_group.h"
#include "spike_ui.h"
#include "hh_service.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_touch.h"
#include "lg_display.h"
#include "lg_identity.h"
#include "lg_ui_screensaver.h"
#include "ui_home.h"
#include "ui_launcher.h"
#include "ui_settings.h"


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
        printf("  grid time: %02u:%02u:%02u UTC (%" PRIu32 ")\n", (unsigned)(day / 3600u),
               (unsigned)(day / 60u % 60u), (unsigned)(day % 60u), st->grid_time);
    }
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
        lg_ui_screensaver_set_enabled(on);   /* kept in NVS, so it survives a reboot */
    }
    printf("Screen saver: %s\n", lg_ui_screensaver_enabled() ? "on" : "off");
    return 0;
}

static int cmd_id(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lg_identity_print(s_identity);   /* identity is read-only after boot */
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
    /* Screens are built and refreshed on the drawing task, and LVGL's layout recurses, so a
     * deeply nested screen can overflow that stack. The panic says a stack overflowed but
     * never how close the boards that survive are, which is the figure worth watching. */
    printf("  drawing task: %" PRIu32 " bytes of stack unused at its worst\n",
           lg_display_stack_headroom());
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
        ui_chat_open_conversation(LG_SCOPE_BROADCAST, LG_TARGET_ALL, "Everyone");
        printf("opened Everyone\n");
        return 0;
    }
    if (end != who && *end == '\0' && device > 0) {
        ui_chat_open_conversation(LG_SCOPE_DIRECT, (uint32_t)device, person_name(st, (uint32_t)device));
        printf("opened the chat with device %ld\n", device);
        return 0;
    }
    for (uint8_t i = 0; i < st->n_groups; i++) {
        if (strcmp(st->groups[i].name, who) == 0) {
            ui_chat_open_conversation(LG_SCOPE_GROUP, st->groups[i].id, st->groups[i].name);
            printf("opened group %s\n", st->groups[i].name);
            return 0;
        }
    }
    printf("unknown conversation '%s'; run 'groups' or 'people'\n", who);
    return 1;
}

/*
 * screen <settings|status|groups|home>: opens that screen, so the screens can be driven from serial
 * while testing (D23, D25, D28), the way `chat` already opens a conversation.
 *
 * Added for a specific reason: Settings is the deepest tree this firmware builds -- screen,
 * tabview, content, page, list, row, label -- and it is what overflowed the drawing task's
 * stack. Without this there was no way to reach it except by tapping the glass, so the one
 * screen whose stack cost matters most was the one that could not be measured.
 */
#if CONFIG_LG_HH_UI_SPIKE
/* spike chat | spike scroll <px> | spike kb <on|off> | spike type <text> | spike log */
static int cmd_spike(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "chat") == 0) {
        spike_ui_request_cmd(SPIKE_CHAT, 0, NULL);
    } else if (argc == 3 && strcmp(argv[1], "scroll") == 0) {
        spike_ui_request_cmd(SPIKE_SCROLL, (int)strtol(argv[2], NULL, 10), NULL);
    } else if (argc == 3 && strcmp(argv[1], "kb") == 0) {
        spike_ui_request_cmd(SPIKE_KEYBOARD, strcmp(argv[2], "on") == 0, NULL);
    } else if (argc >= 3 && strcmp(argv[1], "type") == 0) {
        spike_ui_request_cmd(SPIKE_TYPE, 0, argv[2]);
    } else if (argc >= 2 && strcmp(argv[1], "log") == 0) {
        spike_ui_request_cmd(SPIKE_LOG, 0, NULL);
    } else if (argc == 3 && strcmp(argv[1], "go") == 0) {
        static const char *const names[] = { "home", "status", "messages", "chat", "groups", "edit", "settings" };
        int to = -1;
        for (int i = 0; i < 7; i++) {
            if (strcmp(argv[2], names[i]) == 0) {
                to = i;
            }
        }
        if (to < 0 || to == 3 || to == 5) {
            printf("go home|status|messages|groups|settings\n");
            return 1;
        }
        spike_ui_request_cmd(SPIKE_GO, to, NULL);
    } else if (argc == 4 && strcmp(argv[1], "tap") == 0) {
        long x = strtol(argv[2], NULL, 10);
        long y = strtol(argv[3], NULL, 10);
        spike_ui_request_cmd(SPIKE_TAP, (int)((x << 16) | (y & 0xFFFF)), NULL);
    } else if (argc == 3 && strcmp(argv[1], "page") == 0) {
        int page = strcmp(argv[2], "numbers") == 0 ? 1 : strcmp(argv[2], "emoji") == 0 ? 2
                 : strcmp(argv[2], "shift") == 0 ? 3 : 0;
        spike_ui_request_cmd(SPIKE_PAGE, page, NULL);
    } else {
        printf("usage: spike chat | go <screen> | tap <x> <y> | scroll <px> | kb <on|off> | type <word> | page <letters|numbers|emoji|shift> | log\n");
        return 1;
    }
    return 0;
}
#endif

static int cmd_screen(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: screen <settings|status|groups|home>\n");
        return 1;
    }
    const char *which = argv[1];
#if CONFIG_LG_HH_UI_SPIKE
    if (strcmp(which, "status") == 0 || strcmp(which, "home") == 0) {
        spike_ui_request(strcmp(which, "status") == 0);
        printf("asked the spike for %s\n", which);
        return 0;
    }
#endif
    if (strcmp(which, "settings") == 0) {
        ui_settings_open();
        printf("opened Settings; run 'status' to read the drawing task's stack after it drew\n");
        return 0;
    }
    if (strcmp(which, "status") == 0) {
        ui_home_open();
        printf("opened Status\n");
        return 0;
    }
    if (strcmp(which, "groups") == 0) {
        ui_group_open_list();
        printf("opened Groups\n");
        return 0;
    }
    if (strcmp(which, "home") == 0) {
        ui_launcher_open();
        printf("opened the launcher\n");
        return 0;
    }
    printf("unknown screen '%s'; try settings, status, groups or home\n", which);
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
        { .command = "screen",    .help = "screen <settings|status|groups|home>: open that screen",          .func = cmd_screen },
#if CONFIG_LG_HH_UI_SPIKE
        { .command = "spike",     .help = "spike chat | scroll <px> | kb <on|off> | type <word> | log (no-LVGL spike)", .func = cmd_spike },
#endif
        { .command = "time",      .help = "Show grid time and the time restriction",                 .func = cmd_time },
        { .command = "tone",      .help = "tone [hz] [ms]: play one tone on the speaker",           .func = cmd_tone },
        { .command = "cue",       .help = "cue <sent|received|urgent>: play a notification sound",  .func = cmd_cue },
        { .command = "i2cscan",   .help = "Addresses answering on this board's I2C bus",            .func = cmd_i2cscan },
        { .command = "saver",     .help = "saver [on|off]: the screen saver, kept in NVS",         .func = cmd_saver },
        { .command = "volume",    .help = "volume [off|low|medium|high]: notification loudness, kept in NVS", .func = cmd_volume },
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
    (void)lg_power_register_command();   /* power [-m s [-i ms] [-q]], shared with the APs */
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
