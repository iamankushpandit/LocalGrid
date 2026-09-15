#include "settings.h"

#include <string.h>

#include "nvs.h"

#define SETTINGS_NAMESPACE  "lgcfg"
#define SETTINGS_KEY        "admin"
#define SETTINGS_VERSION    1

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
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, SETTINGS_KEY, &tmp, &len);
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
