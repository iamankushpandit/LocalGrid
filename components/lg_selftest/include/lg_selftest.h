/*
 * lg_selftest.h - the quick check every device runs at boot (decision D24).
 *
 * Owner decision: product firmware keeps the self test. The full test app needs one 136 KB
 * block for its simulated grid, which a classic ESP32 cannot spare once Wi-Fi is up, so this
 * is the part that always runs: the envelope, duplicate detection, message bodies, UTF-8,
 * and known-answer tests for the three primitives, in well under a second.
 *
 * The full suite still lives in tests/target and is flashed with `--firmware tests`.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LG_SELFTEST_NAME_MAX 40

typedef struct {
    uint16_t checks;
    uint16_t failures;
    uint32_t ms;
    char     first_failure[LG_SELFTEST_NAME_MAX];   /* empty when everything passed */
} lg_selftest_result_t;

/* Runs the checks and remembers the result. Crypto must be initialised first
 * (lg_crypto_init), and the radio must be started for strong randomness. */
const lg_selftest_result_t *lg_selftest_quick(void);

/* The last result, or NULL if the check has never run. */
const lg_selftest_result_t *lg_selftest_last(void);

/* "25 checks passed in 96 ms" or "2 of 25 failed: aead tag". Never NULL. */
const char *lg_selftest_summary(void);

#ifdef __cplusplus
}
#endif
