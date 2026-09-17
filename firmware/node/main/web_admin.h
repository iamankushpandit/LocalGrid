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

#include "esp_err.h"

esp_err_t web_admin_start(void);

/* Call from the core task about once a second. */
void web_admin_publish_snapshot(void);
