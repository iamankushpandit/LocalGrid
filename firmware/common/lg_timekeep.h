/*
 * lg_timekeep.h - grid time that survives a restart (D48, D60).
 *
 * Grid time lived only in RAM, so a reset, a crash or a brownout lost it: an AP came back with no
 * clock, and a handheld came back with nothing to carry to it (D53). The chip's RTC timer keeps
 * counting through every reset that does not cut power, which is what these two calls use.
 *
 * What it does NOT do is outlive a real power-off: the RTC stops with the supply, the restored
 * value fails the sanity check, and D6 stands -- the admin sets the time again. That is the point:
 * a clock is only worth restoring when the chip can say how much time has passed.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Remembers a grid time (Unix seconds) so a restart can pick it up. Call whenever the clock is
 * set or corrected; cheap enough for every correction. */
void lg_timekeep_save(uint32_t unix_s);

/* The time this device would have now, or 0 when nothing trustworthy survived. */
uint32_t lg_timekeep_restore(void);

#ifdef __cplusplus
}
#endif
