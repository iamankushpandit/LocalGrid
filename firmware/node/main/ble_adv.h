/*
 * ble_adv.h - NimBLE broadcaster: non-connectable adverts carrying the
 * LocalGrid discovery payload in manufacturer data (company ID 0xFFFF).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_adv_init(const uint8_t *payload, size_t len);

/* Replaces the payload; takes effect on the next advert. */
void ble_adv_update(const uint8_t *payload, size_t len);
