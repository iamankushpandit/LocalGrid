/*
 * Serial console for the handheld (decision D28): every device answers status and
 * configuration questions over its serial port, the way `id` does at flash time.
 *
 * Commands read a status snapshot and post changes through hh_service.h, so the
 * network task stays the only owner of Wi-Fi and lg_client state.
 */
#include "hh_console.h"

#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_system.h"
#include "lg_envelope.h"
#include "ui_chat.h"
#include "hh_service.h"
#include "lg_identity.h"

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

static int cmd_id(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lg_identity_print(s_identity);   /* identity is read-only after boot */
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static hh_status_t st;
    hh_service_status(&st);
    printf("Handheld status\n");
    printf("  device %" PRIu32 " (%s), id %s\n", st.device, st.name[0] ? st.name : "not in roster",
           s_identity->present ? s_identity->id : "none");
    printf("  link: %s", link_word(st.link));
    if (st.node >= 0) {
        printf(" on %s (node %d)", st.node_ssid[0] ? st.node_ssid : "node", st.node);
    }
    printf("\n");
    if (st.link == HH_LINK_ONLINE || st.link == HH_LINK_REGISTERING) {
        printf("  signal: %d dBm (%s), address %u.%u.%u.%u\n", st.rssi, signal_word(st.rssi), st.ip[0], st.ip[1],
               st.ip[2], st.ip[3]);
    }
    print_time(&st);
    if (st.preferred_node < 0) {
        printf("  node choice: automatic\n");
    } else {
        printf("  node choice: fixed to node %d\n", st.preferred_node);
    }
    printf("  registrations since boot: %" PRIu32 "\n", st.joins);
    printf("  memory: %" PRIu32 " KB free, %" PRIu32 " KB lowest\n", st.free_heap / 1024, st.min_free_heap / 1024);
    printf("  problem: %s\n", st.problem[0] ? st.problem : "none");
    return 0;
}

static int cmd_nodes(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static hh_status_t st;
    hh_service_status(&st);
    printf("Nodes in range: %u\n", st.n_nodes);
    printf("  NODE  SSID              RSSI  ATTACHED  BACKBONE  STATE\n");
    for (uint8_t i = 0; i < st.n_nodes; i++) {
        const hh_node_seen_t *n = &st.nodes[i];
        const char *state = st.link == HH_LINK_ONLINE && st.node == (int)n->node ? "connected"
                            : st.preferred_node == (int)n->node                  ? "chosen"
                                                                                 : "";
        printf("  %-4u  %-16s  %-4d  %-8u  %-8s  %s\n", n->node, n->ssid, n->rssi, n->clients,
               n->backbone ? "yes" : "no", state);
    }
    return 0;
}

static int cmd_people(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static hh_status_t st;
    hh_service_status(&st);
    printf("Handhelds this device has heard about: %u\n", st.n_people);
    printf("  DEVICE  NAME              STATE\n");
    for (uint8_t i = 0; i < st.n_people; i++) {
        const hh_person_t *p = &st.people[i];
        if (!p->online) {
            printf("  %-6" PRIu32 "  %-16s  offline\n", p->device, p->name);
        } else if ((int)p->node == st.node) {
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
    static hh_status_t st;
    hh_service_status(&st);
    print_time(&st);
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
    static hh_message_t msgs[HH_MESSAGES];
    static hh_status_t st;
    hh_service_status(&st);
    size_t n = hh_service_messages(msgs, want);
    printf("Messages, newest first: %u\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const hh_message_t *m = &msgs[i];
        if (m->mine) {
            printf("  to ");
            print_target(m, &st);
        } else {
            printf("  from %s (device %" PRIu32 ")", person_name(&st, m->author), m->author);
            if (m->scope == LG_SCOPE_GROUP || m->scope == LG_SCOPE_BROADCAST) {
                printf(" to ");
                print_target(m, &st);
            }
        }
        printf("%s [%s]: %s\n", m->urgent ? " URGENT" : "", hh_message_state_text(m), m->text);
    }
    return 0;
}

static int cmd_groups(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static hh_status_t st;
    hh_service_status(&st);
    printf("Groups in the grid roster: %u\n", st.n_groups);
    printf("  ID  NAME              THIS HANDHELD\n");
    for (uint8_t i = 0; i < st.n_groups; i++) {
        printf("  %-2u  %-16s  %s\n", st.groups[i].id, st.groups[i].name,
               st.groups[i].member ? "member" : "not a member");
    }
    return 0;
}

/* send <device index | group name | all | urgent> <text...> */
static int cmd_send(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: send <device index | group name | all | urgent> <text>\n");
        return 1;
    }
    static hh_status_t st;
    hh_service_status(&st);

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
        for (uint8_t i = 0; i < st.n_groups; i++) {
            if (strcmp(st.groups[i].name, who) == 0) {
                found = st.groups[i].id;
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
    static hh_status_t st;
    hh_service_status(&st);
    const char *who = argv[1];
    char *end = NULL;
    long device = strtol(who, &end, 10);
    if (strcmp(who, "all") == 0) {
        ui_chat_open_conversation(LG_SCOPE_BROADCAST, LG_TARGET_ALL, "Everyone");
        printf("opened Everyone\n");
        return 0;
    }
    if (end != who && *end == '\0' && device > 0) {
        ui_chat_open_conversation(LG_SCOPE_DIRECT, (uint32_t)device, person_name(&st, (uint32_t)device));
        printf("opened the chat with device %ld\n", device);
        return 0;
    }
    for (uint8_t i = 0; i < st.n_groups; i++) {
        if (strcmp(st.groups[i].name, who) == 0) {
            ui_chat_open_conversation(LG_SCOPE_GROUP, st.groups[i].id, st.groups[i].name);
            printf("opened group %s\n", st.groups[i].name);
            return 0;
        }
    }
    printf("unknown conversation '%s'; run 'groups' or 'people'\n", who);
    return 1;
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
        { .command = "groups",    .help = "Groups in the roster and whether this handheld belongs",  .func = cmd_groups },
        { .command = "send",      .help = "send <device|group|all|urgent> <text>: send a message",   .func = cmd_send },
        { .command = "msgs",      .help = "msgs [count]: messages sent and received, newest first",  .func = cmd_msgs },
        { .command = "chat",      .help = "chat <device|group|all>: open that conversation on screen", .func = cmd_chat },
        { .command = "time",      .help = "Show grid time and the time restriction",                 .func = cmd_time },
        { .command = "reboot",    .help = "Restart this handheld",                                   .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
