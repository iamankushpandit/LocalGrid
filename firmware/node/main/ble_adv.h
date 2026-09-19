/*
 * ble_adv.h - NimBLE broadcaster: non-connectable adverts carrying the
 * LocalGrid discovery payload in manufacturer data (company ID 0xFFFF), and
 * the sealed status beacon (D68, docs/ble-status.md) in the scan response.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_adv_init(const uint8_t *payload, size_t len);

/* Replaces the payload; takes effect on the next advert. */
void ble_adv_update(const uint8_t *payload, size_t len);

/*
 * D68: puts one sealed status frame (at most 27 bytes, the manufacturer data after the company ID)
 * in the scan response; len 0 empties it. Any task, never blocks on the radio: the frame is copied
 * and the NimBLE host task applies it. Does nothing if BLE did not start.
 */
void ble_adv_set_status(const uint8_t *frame, size_t len);
