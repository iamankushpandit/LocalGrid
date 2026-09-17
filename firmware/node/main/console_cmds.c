/*
 * Serial console for the node. Commands are posted to the core task so that
 * lg_core state is only touched from one task.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "lg_power.h"
#include "node_app.h"
#include "sdkconfig.h"

static int post(node_cmd_type_t type, uint32_t value, const char *text)
{
    node_cmd_t cmd = { .type = type, .value = value };
    if (text != NULL) {
        strncpy(cmd.text, text, sizeof(cmd.text) - 1);
    }
    if (xQueueSend(g_app.cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        printf("busy, try again\n");
        return 1;
    }
    return 0;
}

static int cmd_id(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lg_identity_print(&g_app.identity);   /* identity is read-only after boot */
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return post(NODE_CMD_STATUS, 0, NULL);
}

static int cmd_nodes(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return post(NODE_CMD_NODES, 0, NULL);
}

static int cmd_devices(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return post(NODE_CMD_DEVICES, 0, NULL);
}

/* Read-only: settings are changed on the admin page of any AP, never over serial (D45). */
static int cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return post(NODE_CMD_CONFIG, 0, NULL);
}

static int cmd_ping(int argc, char **argv)
{
    char text[LG_TEXT_MAX + 1] = "ping";
    if (argc > 1) {
        text[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                strncat(text, " ", sizeof(text) - strlen(text) - 1);
            }
            strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        }
    }
    return post(NODE_CMD_PING, 0, text);
}

static int cmd_time(int argc, char **argv)
{
    if (argc == 1) {
        return post(NODE_CMD_TIME_SHOW, 0, NULL);
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || v < 1700000000ul) {
            printf("usage: time set <unix seconds>, e.g. time set 1790000000\n");
            return 1;
        }
        return post(NODE_CMD_TIME_SET, (uint32_t)v, NULL);
    }
    printf("usage: time | time set <unix seconds>\n");
    return 1;
}

static int cmd_groups(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return post(NODE_CMD_GROUPS, 0, NULL);
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "grid>";
    repl_config.max_cmdline_length = 256;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "id",      .help = "Print this board's LocalGrid device ID",        .func = cmd_id },
        { .command = "status",  .help = "AP identity, time, memory, links, sessions", .func = cmd_status },
        { .command = "nodes",   .help = "Backbone links to other APs, loss counters",  .func = cmd_nodes },
        { .command = "devices", .help = "Grid presence table and local sessions",       .func = cmd_devices },
        { .command = "config",  .help = "Grid name, time zone, LocalMesh Access Point network, admin state (read-only)", .func = cmd_config },
        { .command = "ping",    .help = "ping [text]: flood a diagnostic echo to every AP", .func = cmd_ping },
        { .command = "time",    .help = "time | time set <unix seconds>: show or set grid time", .func = cmd_time },
        { .command = "groups",  .help = "Groups, their members, and the table's version (D52)", .func = cmd_groups },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    /* power: this board has no supply sense unless CONFIG_LG_NODE_SUPPLY_SENSE_GPIO names a
     * divider on an ADC1 pin; either way it reports restarts by cause, which is where a
     * sagging supply shows on an AP. */
    (void)lg_power_init(CONFIG_LG_NODE_SUPPLY_SENSE_GPIO, CONFIG_LG_NODE_SUPPLY_DIVIDER_MILLI);
    lg_power_set_report_hook(node_print_restarts);
    ESP_ERROR_CHECK(lg_power_register_command());
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
