/*
 * node_app.h - shared state of the node firmware.
 *
 * Threading model: the lg_core node, the backbone, and the TCP sessions are
 * owned by one task ("lg_core"). Wi-Fi, ESP-NOW, and BLE callbacks only copy
 * data into queues. Console commands are posted to node_cmd_queue.
 */
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lg_identity.h"
#include "lg_node.h"
#include "sdkconfig.h"

/* Build-time A/B switches (main/Kconfig.projbuild, see sdkconfig.defaults). Both default on. */
#ifdef CONFIG_LG_NODE_BLE_ADV
#define NODE_BLE_ADV_ENABLED  1
#else
#define NODE_BLE_ADV_ENABLED  0
#endif
#ifdef CONFIG_LG_NODE_PMF_CAPABLE
#define NODE_PMF_CAPABLE      1
#else
#define NODE_PMF_CAPABLE      0
#endif

typedef enum {
    NODE_CMD_STATUS,
    NODE_CMD_NODES,
    NODE_CMD_DEVICES,
    NODE_CMD_PING,
    NODE_CMD_TIME_SHOW,
    NODE_CMD_TIME_SET,
    NODE_CMD_CONFIG,
    NODE_CMD_GRID_ANNOUNCE,   /* settings changed on this AP: flood grid state now */
    NODE_CMD_GROUP_EDIT,      /* the admin page changed a group (D52) */
    NODE_CMD_GROUPS,          /* console: print the group table */
    NODE_CMD_GPS_TIME,        /* the GPS reader: a fixed RMC (D63); value, millis, at_ms */
    NODE_CMD_GPS,             /* console: print the GPS state */
} node_cmd_type_t;

typedef struct {
    node_cmd_type_t type;
    uint32_t        value;
    uint16_t        millis;        /* NODE_CMD_TIME_SET, _GPS_TIME: milliseconds into value's second */
    uint32_t        at_ms;         /* NODE_CMD_GPS_TIME: app_now_ms when the sentence arrived */
    char            text[LG_TEXT_MAX + 1];
    lg_group_edit_t group;
} node_cmd_t;

typedef struct {
    lg_identity_t identity;        /* written by tools/flash.py; read-only after boot */
    uint16_t      index;
    const char   *name;
    uint32_t      boot;
    lg_roster_t   roster;          /* users are fixed; groups change and are saved (D52) */
    lg_node_t     core;
    int64_t       time_offset_ms;  /* grid time in ms = monotonic ms + offset */
    int32_t       time_slew_ms;    /* correction still being spread into the offset */
    uint8_t       time_quality;    /* lg_time_quality_t */
    uint8_t       time_stratum;    /* 0 where the admin set it, +1 per hop, LG_STRATUM_UNKNOWN */
    QueueHandle_t cmd_queue;
} node_app_t;

extern node_app_t g_app;

uint32_t app_now_ms(void);
uint32_t app_grid_time(void);
/* Grid time as Unix milliseconds, 0 when unset. */
uint64_t app_grid_time_ms(void);
/* A newer admin setting of the time exists elsewhere: forget this AP's distance from the old one. */
void     app_time_follow_new_generation(void);
/* Core task, about ten times a second: spreads a small correction instead of stepping. */
void     app_time_slew(uint32_t elapsed_ms);
void     console_start(void);
void     node_print_restarts(void);   /* restart counts by cause; safe from the console task */
