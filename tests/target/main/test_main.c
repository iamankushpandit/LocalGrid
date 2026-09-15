#include <ctype.h>
#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_crypto.h"
#include "lg_identity.h"
#include "lg_test.h"
#include "nvs_flash.h"
#include "sim.h"
#include "test_screen.h"

int lg_checks = 0;
int lg_failures = 0;

size_t hex2bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (; *hex; hex++) {
        int c = (unsigned char)*hex;
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue;   /* skip spaces and colons */
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n < cap) {
                out[n] = (uint8_t)((hi << 4) | v);
            }
            n++;
            hi = -1;
        }
    }
    return n;
}

typedef struct {
    const char *name;
    void (*run)(void);
} suite_t;

void app_main(void)
{
    static lg_identity_t identity;
    if (lg_identity_load(&identity) != ESP_OK) {
        identity.present = false;
    }
    lg_identity_print(&identity);
    if (lg_identity_start_responder(&identity) != ESP_OK) {
        printf("id responder unavailable\n");
    }
    bool reserved = sim_reserve();   /* before the display fragments the heap */
    esp_err_t nvs = nvs_flash_init();   /* touch calibration lives in the default NVS partition */
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        printf("nvs unavailable: %s\n", esp_err_to_name(nvs));
    }
    test_screen_start(&identity);   /* handhelds show results on their own screen (D23) */

    printf("\nLG_TESTS_START\n");
    printf("heap before tests: %u bytes free, largest block %u bytes, simulation memory %s\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           reserved ? "reserved" : "NOT reserved");
    /* The suites stay CPU-bound for minutes on a classic ESP32 (PBKDF2 vectors, simulated grid).
     * At idle priority they time-slice with the idle task, so the task watchdog stays fed. */
    UBaseType_t priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    CHECK_EQ(lg_crypto_init(), 0);

    static const suite_t suites[] = {
        { "core", test_core },
        { "crypto", test_crypto },
        { "messaging", test_messaging },
    };
    for (int i = 0; i < (int)(sizeof(suites) / sizeof(suites[0])); i++) {
        int checks_before = lg_checks;
        int failures_before = lg_failures;
        int64_t started = esp_timer_get_time();
        test_screen_suite(i, 0, 0, 0, false);
        suites[i].run();
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
        test_screen_suite(i, lg_checks - checks_before, lg_failures - failures_before, elapsed_ms, true);
        printf("%s: %d checks, %d failures so far, %" PRIu32 " ms\n", suites[i].name, lg_checks, lg_failures,
               elapsed_ms);
    }

    vTaskPrioritySet(NULL, priority);

    uint32_t min_heap = esp_get_minimum_free_heap_size();
    printf("LG_TESTS: %d checks, %d failures, min free heap %" PRIu32 " bytes\n", lg_checks, lg_failures, min_heap);
    printf("LG_TESTS_RESULT: %s\n", lg_failures == 0 ? "PASS" : "FAIL");
    test_screen_finish(lg_checks, lg_failures, min_heap);

    test_screen_touch_loop();   /* returns at once on boards without touch */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
