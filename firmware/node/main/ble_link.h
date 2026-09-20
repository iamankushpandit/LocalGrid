/*
 * ble_link.h - the read-only BLE GATT admin link (D70, docs/ble-link.md).
 *
 * A watcher (tools/grid_watch.py or the Android app) connects to one AP over BLE and asks for
 * what the admin page shows. It never joins the grid's Wi-Fi, never carries a grid message, and
 * cannot change anything: no opcode writes, and none returns any message text or audio.
 *
 * Threading: the NimBLE host task only copies a written chunk into a queue and returns. The link
 * task unseals it, builds the reply from the snapshot the core task publishes (web_admin's one
 * emitter, so the bytes match the admin page exactly) and from the packed traffic record, and
 * notifies it back in sealed chunks. The core task is never blocked by a watcher.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Registers the GATT service. NimBLE host task setup: call between nimble_port_init() and
 * nimble_port_freertos_init(). */
esp_err_t ble_link_gatt_register(void);

/* Derives K_link from the backbone key and starts the link task. After ble_adv_init. */
esp_err_t ble_link_init(const uint8_t backbone_key[32]);

/* The link task, for the stack headroom the TRAFFIC reply reports. NULL until it starts. */
TaskHandle_t ble_link_task(void);

/* ---- called from the NimBLE host task by ble_adv.c's GAP callback ---- */
void ble_link_on_connect(uint16_t conn_handle);
void ble_link_on_disconnect(uint16_t conn_handle);
void ble_link_on_mtu(uint16_t conn_handle, uint16_t mtu);
void ble_link_on_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_on);
void ble_link_on_notify_tx(void);

/* True while a watcher holds the connection: ble_adv then advertises non-connectably, so the
 * status beacon keeps going and no second watcher can connect. */
bool ble_link_busy(void);
