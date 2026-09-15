/*
 * test_screen.h - on-device results screen and touch check for the test app (decision D23).
 * Every function is a no-op on boards without a display.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lg_identity.h"

void test_screen_start(const lg_identity_t *identity);
void test_screen_suite(int index, int checks, int failures, uint32_t elapsed_ms, bool done);
void test_screen_finish(int checks, int failures, uint32_t min_heap_bytes);

/* After the results: a tap opens the touch check (calibrating resistive panels first).
 * Never returns on boards with touch; returns at once otherwise. */
void test_screen_touch_loop(void);
