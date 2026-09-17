/*
 * Small stored preferences.
 *
 * Deliberately thin: open, read or write one byte, commit, close. No cached handle, because a
 * preference is read once at start and written when somebody taps a row -- holding NVS open
 * for the life of the device to save a few microseconds a day would be a poor trade.
 *
 * A read that fails for any reason returns the caller's fallback rather than an error. The
 * caller is a screen asking "is the saver on?", and the useful answer when flash cannot be
 * read is "behave as if nobody has changed it", not "refuse to draw".
 */
#include "lg_bsp_settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "BSP";

/* Shared with the touch calibration and the audio volume, so preferences live in one place. */
#define NVS_NAMESPACE "lgui"

bool lg_bsp_setting_get_bool(const char *key, bool fallback)
{
    if (key == NULL) {
        return fallback;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* No namespace yet is normal on a handheld with no stored preferences at all. */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "[SET] %s unreadable (%s); using the default", key, esp_err_to_name(err));
        }
        return fallback;
    }
    uint8_t stored = 0;
    bool value = nvs_get_u8(h, key, &stored) == ESP_OK ? stored != 0 : fallback;
    nvs_close(h);
    return value;
}

esp_err_t lg_bsp_setting_set_bool(const char *key, bool value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[SET] %s not stored (%s); the choice holds until the next boot", key,
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, key, value ? 1u : 0u);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
