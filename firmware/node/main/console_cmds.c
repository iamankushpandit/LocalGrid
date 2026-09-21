/*
 * Serial console for the node. Commands are posted to the core task so that
 * lg_core state is only touched from one task.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "lg_power.h"
#include "gps.h"
#include "backbone.h"
#include "lora.h"
#include "node_app.h"
#include "sdkconfig.h"

/* No chaos hook may take a radio out for longer than this, whatever was typed. */
#define CHAOS_MAX_S 3600ul

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

/* A board running another board's identity did not start its radios, so only "id" and "power"
 * have anything to report; everything else would read state that was never built. */
static bool identity_blocked(void)
{
    return lg_identity_verified() == LG_IDENTITY_MISMATCH;
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

static int cmd_gps(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "raw") == 0) {
        gps_dump(600);   /* about two seconds of NMEA at 9600 baud */
        return 0;
    }
    return post(NODE_CMD_GPS, 0, NULL);
}

/* The second backbone (D71). Status and a test send go through the core task; the rest are flags
 * and one-shot requests the LoRa task picks up, so the console never waits on the radio. */
static int cmd_lora(int argc, char **argv)
{
    if (argc == 1) {
        return post(NODE_CMD_LORA, 0, NULL);
    }
    if (strcmp(argv[1], "send") == 0) {
        char text[LG_TEXT_MAX + 1] = "lora send";
        if (argc > 2) {
            text[0] = '\0';
            for (int i = 2; i < argc; i++) {
                if (i > 2) {
                    strncat(text, " ", sizeof(text) - strlen(text) - 1);
                }
                strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
            }
        }
        return post(NODE_CMD_LORA_TEST, 0, text);
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        /* Chaos hook. Console only, and it restores itself, so a tool that dies cannot leave an
         * AP deaf. Seconds are capped so a typo cannot take the radio out for a day. */
        unsigned long sec = argc > 2 ? strtoul(argv[2], NULL, 10) : 60ul;
        lora_disable((uint32_t)(sec == 0 || sec > CHAOS_MAX_S ? CHAOS_MAX_S : sec));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        lora_enable();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        lora_request_reset();
        printf("Resetting and reconfiguring the module\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "broadcast") == 0 &&
        (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        bool on = strcmp(argv[2], "on") == 0;
        lora_set_broadcast(on);
        printf("Frames for every AP go %s\n", on ? "once to the broadcast address" : "once to each peer");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "at") == 0) {
        /* Bring-up only: whether this firmware's broadcast address works is a question only the
         * module can answer, and this is how to ask it. Nothing here touches a key or a message. */
        char at[64] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strncat(at, " ", sizeof(at) - strlen(at) - 1);
            }
            strncat(at, argv[i], sizeof(at) - strlen(at) - 1);
        }
        lora_request_at(at);
        printf("Sent \"%s\"; the reply is logged at [LORA]\n", at);
        return 0;
    }
    printf("usage: lora | lora send [text] | lora off [seconds] | lora on | lora reset"
           " | lora broadcast on|off | lora at <AT command>\n");
    return 1;
}

/* The ESP-NOW backbone's chaos hook, the mirror of `lora off` (docs/lora.md). Console only. */
static int cmd_bb(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        unsigned long sec = argc > 2 ? strtoul(argv[2], NULL, 10) : 60ul;
        lgbb_disable((uint32_t)(sec == 0 || sec > CHAOS_MAX_S ? CHAOS_MAX_S : sec));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        lgbb_enable();
        return 0;
    }
    printf("ESP-NOW backbone is %s\n", lgbb_is_off() ? "OFF (chaos hook)" : "on");
    printf("usage: bb | bb off [seconds] | bb on\n");
    return argc > 1 ? 1 : 0;
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
        { .command = "config",  .help = "Grid name, time zone, LocalGrid Access Point network, admin state (read-only)", .func = cmd_config },
        { .command = "ping",    .help = "ping [text]: flood a diagnostic echo to every AP", .func = cmd_ping },
        { .command = "time",    .help = "time | time set <unix seconds>: show or set grid time", .func = cmd_time },
        { .command = "gps",     .help = "The GPS on MAIN: heard, fix, satellites (D63); gps raw dumps what arrives", .func = cmd_gps },
        { .command = "lora",    .help = "The LoRa backbone (D71): status; lora send, off/on, reset, broadcast on|off, at <cmd>", .func = cmd_lora },
        { .command = "bb",      .help = "bb | bb off [seconds] | bb on: stop using the ESP-NOW backbone, for chaos runs", .func = cmd_bb },
        { .command = "groups",  .help = "Groups, their members, and the table's version (D52)", .func = cmd_groups },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (identity_blocked() && strcmp(cmds[i].command, "id") != 0) {
            continue;   /* the grid never started; these would report on state that does not exist */
        }
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    if (identity_blocked()) {
        printf("\nThis board is running another board's identity, so it is not serving the grid.\n"
               "Type \"id\" to see it, then give this board its own: "
               "python tools/flash.py <name> --new-id\n\n");
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
