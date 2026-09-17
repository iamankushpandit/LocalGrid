/*
 * LocalGrid handheld firmware (prototype milestones P5, P6).
 *
 * Boot: identity, NVS, network service, the quick self test (decision D24), display, touch
 * calibration if needed, then the launcher. service/ and ui/ never include each other's
 * layers (D27); they meet only through hh_service.h.
 *
 * This task stays alive afterwards to run the jobs the drawing task must not: touch
 * calibration waits for presses, and the self test runs for tens of milliseconds.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hh_console.h"
#include "hh_mem.h"
#include "hh_service.h"
#include "lg_board.h"
#include "lg_bsp_audio.h"
#include "lg_bsp_touch.h"
#include "lg_display.h"
#include "lg_identity.h"
#include "lg_power.h"
#include "lg_selftest.h"
#include "lg_theme.h"
#include "lg_ui_input.h"
#include "lg_ui_screensaver.h"
#include "nvs_flash.h"
#include "ui_launcher.h"
#include "ui_settings.h"
#include "spike_ui.h"

static const char *TAG = "HH";

#define SCREENSAVER_IDLE_MS 60000u   /* a minute untouched, then the rain (D35) */

void app_main(void)
{
    hh_mem_mark("boot");
    static lg_identity_t identity;
    if (lg_identity_load(&identity) != ESP_OK) {
        identity.present = false;
    }
    lg_identity_print(&identity);

    /*
     * NVS first, because everything that remembers a choice reads it from here. Audio starts
     * a few lines below and loads its mute flag, and while this ran later that read failed
     * silently: a handheld muted from Settings came back with the sound on.
     */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(nvs));
    }

    /* Sound starts before the console can be asked for one. It needs only the board profile
     * and a GPIO, while the self test and the display take about five seconds on the slower
     * board, and a cue command arriving inside that window was being told this board had no
     * speaker when the truth was that audio had not started yet. */
    const lg_board_t *board = identity.present ? lg_board_find(identity.board) : NULL;

    /*
     * Before the console, so that a cue asked for in the first seconds is played rather than
     * refused: the self test and the display take about five seconds on the slower board.
     *
     * Starting it here was suspected of costing that board 20 KB of heap. It does not: moving
     * the call after the display measured 96 KB against 94 KB, and the same reading swung from
     * 147 KB to 169 KB on the other board across those builds with no audio change at all.
     * The free-heap line at Online is taken at a moment that varies with Wi-Fi and peer state,
     * so it cannot settle a 20 KB question; a settled `status` reading can.
     */
    lg_bsp_audio_start(board);
    (void)lg_power_init(board != NULL ? board->supply_sense : -1, board != NULL ? board->supply_divider_milli : 0);
    hh_mem_mark("after NVS and audio");

    if (hh_console_start(&identity) != ESP_OK) {   /* answers id, status, nodes, send, ... (D28) */
        ESP_LOGW(TAG, "serial console unavailable");
    }
    hh_mem_mark("after console");

    /* NVS is started earlier now, above, because settings are read before this point. */

    /* The service first, so the launcher's first paint already says what is happening. */
    esp_err_t err = hh_service_start(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[NET] Network service not started: %s", esp_err_to_name(err));
    }
    hh_mem_mark("after network service start (Wi-Fi driver)");

    /* Decision D24: the product firmware keeps the self test. This is the quick part, which
     * needs no simulated grid, so it runs at every boot. */
    const lg_selftest_result_t *test = lg_selftest_quick();
    if (test->failures == 0) {
        ESP_LOGI("TEST", "[TEST] Self test: %s", lg_selftest_summary());
    } else {
        ESP_LOGE("TEST", "[TEST] Self test: %s", lg_selftest_summary());
    }
    hh_mem_mark("after self test");

#if CONFIG_LG_HH_UI_SPIKE
    /* The no-LVGL spike: LVGL is linked for its font tables but never started. */
    if (board == NULL || spike_ui_start(board) != ESP_OK) {
        ESP_LOGE(TAG, "[UI] Spike did not start");
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    static lg_display_t display;
    if (!lg_board_has_display(board) || lg_display_start(board, &display) != ESP_OK) {
        ESP_LOGE(TAG, "[UI] No display on this board; decision D23 needs one on every handheld");
        return;
    }
    hh_mem_mark("after display, LVGL, touch");
    lg_theme_init(display.width, display.height, display.px_per_10mm);
    while (lg_bsp_touch_needs_calibration()) {
        lg_ui_calibrate();
    }
    ui_launcher_start(&identity);

    /* This task is not the drawing task, so the saver's widgets are built under the
     * lock. It lives on the top layer and leaves the launcher underneath untouched. */
    lg_display_lock(1000);
    lg_ui_screensaver_start(SCREENSAVER_IDLE_MS);
    lg_display_unlock();
    hh_mem_mark("after screen saver");

    for (;;) {
        switch (ui_settings_take_job()) {
        case UI_JOB_CALIBRATE:
            lg_ui_calibrate();
            ui_settings_open();   /* Settings was freed when calibration replaced it; build it again */
            break;
        case UI_JOB_SELFTEST:
            ESP_LOGI("TEST", "[TEST] Self test: %s", (lg_selftest_quick(), lg_selftest_summary()));
            break;
        default:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
