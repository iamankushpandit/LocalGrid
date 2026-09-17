#include "settings.h"

#include <string.h>

#include "nvs.h"

#define SETTINGS_NAMESPACE  "lgcfg"
#define SETTINGS_KEY        "admin"
#define SETTINGS_VERSION    2

/* Version 1, written by master-only firmware before D45. Read once and carried forward. */
typedef struct {
    uint16_t version;
    bool     configured;
    char     grid_name[SETTINGS_GRID_NAME_MAX + 1];
    char     timezone[SETTINGS_TZ_MAX + 1];
    uint8_t  salt[SETTINGS_SALT_LEN];
    uint32_t iterations;
    uint8_t  hash[SETTINGS_HASH_LEN];
} settings_v1_t;

esp_err_t settings_load(node_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* never configured */
    }
    if (err != ESP_OK) {
        return err;
    }
    node_settings_t tmp;
    size_t len = 0;
    err = nvs_get_blob(h, SETTINGS_KEY, NULL, &len);
    if (err == ESP_OK && len == sizeof(settings_v1_t)) {
        settings_v1_t old;
        err = nvs_get_blob(h, SETTINGS_KEY, &old, &len);
        nvs_close(h);
        if (err != ESP_OK || old.version != 1) {
            return err;
        }
        /* Only the master could be set up before D45, and it is AP 0. */
        memset(&tmp, 0, sizeof(tmp));
        tmp.version = SETTINGS_VERSION;
        tmp.configured = old.configured;
        memcpy(tmp.grid_name, old.grid_name, sizeof(tmp.grid_name));
        memcpy(tmp.timezone, old.timezone, sizeof(tmp.timezone));
        memcpy(tmp.salt, old.salt, sizeof(tmp.salt));
        tmp.iterations = old.iterations;
        memcpy(tmp.hash, old.hash, sizeof(tmp.hash));
        tmp.seq = old.configured ? 1u : 0u;
        tmp.author = 0;
    } else {
        len = sizeof(tmp);
        if (err == ESP_OK) {
            err = nvs_get_blob(h, SETTINGS_KEY, &tmp, &len);
        }
        nvs_close(h);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        if (err != ESP_OK) {
            return err;
        }
        if (len != sizeof(tmp) || tmp.version != SETTINGS_VERSION) {
            return ESP_OK;   /* incompatible record: treated as unconfigured */
        }
    }
    tmp.grid_name[SETTINGS_GRID_NAME_MAX] = '\0';
    tmp.timezone[SETTINGS_TZ_MAX] = '\0';
    *out = tmp;
    return ESP_OK;
}

esp_err_t settings_save(const node_settings_t *in)
{
    node_settings_t tmp = *in;
    tmp.version = SETTINGS_VERSION;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, SETTINGS_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
