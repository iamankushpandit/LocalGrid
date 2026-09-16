/*
 * LocalGrid handheld firmware (prototype milestone P5).
 *
 * Boot: identity, NVS, display, touch calibration if needed, home screen, then the
 * network service. service/ and ui/ never include each other's layers (D27); they
 * meet only through hh_service.h.
 */
#include "esp_log.h"
#include "hh_console.h"
#include "hh_service.h"
#include "lg_board.h"
#include "lg_bsp_touch.h"
#include "lg_display.h"
#include "lg_identity.h"
#include "lg_theme.h"
#include "lg_ui_input.h"
#include "nvs_flash.h"
#include "ui_home.h"

static const char *TAG = "HH";

void app_main(void)
{
    static lg_identity_t identity;
    if (lg_identity_load(&identity) != ESP_OK) {
        identity.present = false;
    }
    lg_identity_print(&identity);
    if (hh_console_start(&identity) != ESP_OK) {   /* answers id, status, nodes, people, node, ... (D28) */
        ESP_LOGW(TAG, "serial console unavailable");
    }

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(nvs));
    }

    /* The service first, so the home screen's first snapshot already says what is happening. */
    esp_err_t err = hh_service_start(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[NET] Network service not started: %s", esp_err_to_name(err));
    }

    const lg_board_t *board = identity.present ? lg_board_find(identity.board) : NULL;
    static lg_display_t display;
    if (!lg_board_has_display(board) || lg_display_start(board, &display) != ESP_OK) {
        ESP_LOGE(TAG, "[UI] No display on this board; decision D23 needs one on every handheld");
        return;
    }
    lg_theme_init(display.width, display.height, display.px_per_10mm);
    while (lg_bsp_touch_needs_calibration()) {
        lg_ui_calibrate();
    }
    ui_home_start(&identity);
}
