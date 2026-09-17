/*
 * spike_overlay.h - what the no-LVGL UI draws over whatever screen is showing: the new-message
 * banner, the announcement and emergency alerts (D41), and the screen saver (D35). It also keeps
 * the unread counts. Spike task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void spike_overlay_start(uint16_t w, uint16_t h);

/* A touch sample. True when the overlay took it (the screen underneath must not see it). */
bool spike_overlay_touch(int16_t x, int16_t y, bool down);

/* True while an alert or the screen saver covers the panel: screens must not paint. */
bool spike_overlay_covering(void);

/* Call every loop: watches for new messages, flashes alerts, times the banner, runs the saver. */
void spike_overlay_tick(uint32_t now_ms, uint32_t last_touch_ms);

/* A screen repainted something: put the banner back on top if it is showing. */
void spike_overlay_screen_painted(void);

void spike_overlay_log(uint32_t now_ms, uint32_t last_touch_ms);

uint32_t spike_notify_unread(uint8_t scope, uint32_t target);
uint32_t spike_notify_unread_total(void);
void     spike_notify_mark_seen(uint8_t scope, uint32_t target);
