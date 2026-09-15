/*
 * settings.h - master node settings persisted in NVS (namespace "lgcfg").
 *
 * The admin password is stored only as PBKDF2-HMAC-SHA256(password, salt, iterations).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_GRID_NAME_MAX  32
#define SETTINGS_TZ_MAX         47
#define SETTINGS_SALT_LEN       16
#define SETTINGS_HASH_LEN       32

typedef struct {
    uint16_t version;
    bool     configured;
    char     grid_name[SETTINGS_GRID_NAME_MAX + 1];
    char     timezone[SETTINGS_TZ_MAX + 1];      /* IANA name from the admin's browser, display only */
    uint8_t  salt[SETTINGS_SALT_LEN];
    uint32_t iterations;
    uint8_t  hash[SETTINGS_HASH_LEN];
} node_settings_t;

/* Loads settings; an absent or incompatible record yields configured == false. */
esp_err_t settings_load(node_settings_t *out);
esp_err_t settings_save(const node_settings_t *in);
