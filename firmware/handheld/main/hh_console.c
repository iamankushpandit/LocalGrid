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
