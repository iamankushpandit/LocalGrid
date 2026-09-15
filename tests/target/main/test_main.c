#include <ctype.h>
#include <inttypes.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_crypto.h"
#include "lg_test.h"

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

void app_main(void)
{
    printf("\nLG_TESTS_START\n");
    CHECK_EQ(lg_crypto_init(), 0);

    test_core();
    printf("core: %d checks, %d failures so far\n", lg_checks, lg_failures);
    test_crypto();
    printf("crypto: %d checks, %d failures so far\n", lg_checks, lg_failures);
    test_messaging();
    printf("messaging: %d checks, %d failures so far\n", lg_checks, lg_failures);

    printf("LG_TESTS: %d checks, %d failures, min free heap %" PRIu32 " bytes\n",
           lg_checks, lg_failures, esp_get_minimum_free_heap_size());
    printf("LG_TESTS_RESULT: %s\n", lg_failures == 0 ? "PASS" : "FAIL");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
