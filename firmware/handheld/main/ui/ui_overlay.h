/*
 * ui_overlay.h - what the UI draws over whatever screen is showing: the new-message
 * banner, the announcement and emergency alerts (D41), and the screen saver (D35). It also keeps
 * the unread counts. UI task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void ui_overlay_start(uint16_t w, uint16_t h);

/* A touch sample. True when the overlay took it (the screen underneath must not see it). */
bool ui_overlay_touch(int16_t x, int16_t y, bool down);

/* True while an alert or the screen saver covers the panel: screens must not paint. */
bool ui_overlay_covering(void);

/* Call every loop: watches for new messages, flashes alerts, times the banner, runs the saver. */
void ui_overlay_tick(uint32_t now_ms, uint32_t last_touch_ms);

/* A screen repainted something: put the banner back on top if it is showing. */
void ui_overlay_screen_painted(void);

/* Starts the screen saver now, if its setting is on and nothing covers the panel (the lock hands
 * over to it, D62). True when it started. */
bool ui_overlay_saver_now(uint32_t now_ms);

/* Takes the new-message banner away without a redraw: the lock screen is about to cover it. */
void ui_overlay_drop_banner(void);

void ui_overlay_log(uint32_t now_ms, uint32_t last_touch_ms);

/* The alert and distress unit (D66): only broadcasts are shown (no banner, no chats), there is no
 * screen saver, and alerts carry read_hint under Read ("Press button to read"), or nothing if NULL. */
void ui_overlay_set_alert_unit(const char *read_hint);

/* True while an announcement or urgent alert is on screen. */
bool ui_overlay_alert_showing(void);

/* Reads the alert on screen, as tapping Read does: closes it and reports it read (D58). False when
 * no alert is showing. For a board's button (D66). */
bool ui_overlay_ack(void);

uint32_t ui_notify_unread(uint8_t scope, uint32_t target);
uint32_t ui_notify_unread_total(void);
void     ui_notify_mark_seen(uint8_t scope, uint32_t target);
