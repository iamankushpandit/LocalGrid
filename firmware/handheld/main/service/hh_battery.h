/*
 * hh_battery.h - the service side of the battery badge (D62). Screens read the result through
 * hh_service_battery_percent() in hh_service.h; this header is the service's own.
 */
#pragma once

#include <stdint.h>

/* Takes one reading now, so the first screen drawn already has a real value, then samples every
 * 2 s on a low-priority task of its own. Safe to call once; does nothing on a board without a
 * supply sense pin (the percentage stays -1). */
void hh_battery_start(void);

/* The filtered supply in millivolts that the percentage was taken from; 0 before any reading. */
uint32_t hh_battery_mv(void);
