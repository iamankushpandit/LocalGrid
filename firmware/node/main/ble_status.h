/*
 * ble_status.h - the sealed BLE status beacon (D68, docs/ble-status.md).
 *
 * Every 500 ms the core task builds the next frame of the rotation (AP health every other frame;
 * handhelds, alert, batteries and names in turn) from state it owns, seals it with
 * ChaCha20-Poly1305 under a key derived from the backbone secret, cuts the tag to 4 bytes, and
 * hands it to ble_adv for the scan response. A laptop holding this grid's lg_secrets.h reads it
 * with tools/grid_watch.py without joining the grid (D25).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Derives the status key from the backbone key. Call once at boot, before ble_status_poll. */
esp_err_t ble_status_init(const uint8_t backbone_key[32]);

/* Core task, every loop: sends the next frame when 500 ms have passed. */
void ble_status_poll(uint32_t now_ms);
