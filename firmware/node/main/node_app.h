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

typedef enum {
    NODE_CMD_STATUS,
    NODE_CMD_NODES,
    NODE_CMD_DEVICES,
    NODE_CMD_PING,
    NODE_CMD_TIME_SHOW,
    NODE_CMD_TIME_SET,
    NODE_CMD_CONFIG,
} node_cmd_type_t;

typedef struct {
    node_cmd_type_t type;
    uint32_t        value;
    char            text[LG_TEXT_MAX + 1];
} node_cmd_t;

typedef struct {
    lg_identity_t identity;        /* written by tools/flash.py; read-only after boot */
    uint16_t      index;
    const char   *name;
    uint32_t      boot;
    lg_node_t     core;
    int64_t       time_offset_s;   /* grid time = monotonic seconds + offset */
    uint8_t       time_quality;    /* lg_time_quality_t */
    QueueHandle_t cmd_queue;
} node_app_t;

extern node_app_t g_app;

uint32_t app_now_ms(void);
uint32_t app_grid_time(void);
void     console_start(void);
