/*
 * web_admin.h - the admin web page, served by every AP (plain HTTP, D11; no master, D45).
 *
 * Each AP serves it at its own address on its own network. First visit on any AP runs
 * setup (grid name, admin password, time from the browser); afterwards the admin logs in on
 * any AP to see status and set time. Settings live in grid_state.c and reach every AP over
 * the backbone. Login sessions belong to the AP that issued them.
 *
 * Threading: HTTP handlers run in the httpd task. They read a snapshot that the core task
 * publishes, read and commit settings through grid_state (behind its mutex), and change grid
 * time only by posting node_cmd_t commands, so lg_core keeps a single owner.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t web_admin_start(void);

/* Call from the core task about once a second. */
void web_admin_publish_snapshot(void);

/*
 * D70: the bytes of GET /api/status and GET /api/history, for a reader that is not httpd.
 *
 * The admin page and the BLE admin link must never drift apart, so both go through one emitter and
 * one field list; the only difference is where the bytes land. A sink takes them piece by piece and
 * returns false once its destination has failed, which stops the emitter.
 *
 * The emitter works from a snapshot the core task publishes and from one shared scratch buffer, so
 * only one may run at a time: web_admin_emit_* takes that lock for wait_ms and returns false if it
 * could not. The BLE link passes a short wait and answers "busy"; httpd waits.
 */
typedef struct {
    void *ctx;
    bool (*write)(void *ctx, const void *data, size_t len);
} web_sink_t;

bool web_admin_emit_status(const web_sink_t *sink, uint32_t wait_ms);
bool web_admin_emit_history(const web_sink_t *sink, uint32_t wait_ms);
