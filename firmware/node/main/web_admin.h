/*
 * web_admin.h - configuration web page on the master node (plain HTTP, decision D11).
 *
 * Served at http://192.168.4.1/ on the LG-MAIN network. First visit runs setup
 * (grid name, admin password, time from the browser); afterwards the admin logs in
 * to see grid status and set time.
 *
 * Threading: HTTP handlers run in the httpd task. They read a snapshot that the
 * core task publishes, and they change grid state only by posting node_cmd_t
 * commands, so lg_core keeps a single owner.
 */
#pragma once

#include "esp_err.h"

esp_err_t web_admin_start(void);

/* Call from the core task about once a second (master only). */
void web_admin_publish_snapshot(void);
