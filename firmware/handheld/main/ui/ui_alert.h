/*
 * ui_alert.h - the two things a handheld shouts about, as against the things it mentions.
 *
 * A 1:1 or group message gets a banner and a bell: you look when you look. Two kinds of
 * broadcast do not wait to be noticed:
 *
 *   ANNOUNCEMENT  something everyone wants to know now -- the food is ready. The screen
 *                 flashes with the words on it and chimes once, then settles and stays until
 *                 somebody taps its X. Meant to be caught out of the corner of an eye, from
 *                 the other side of a tent, or read later by whoever picks the handheld up.
 *
 *   EMERGENCY     an urgent broadcast (D6 lets these through when nothing else gets out).
 *                 The screen is taken over until somebody taps its X, it keeps flashing, the
 *                 siren repeats, and it sounds at full volume even on a silenced handheld.
 *
 * Only the X closes either one (D41): a touch anywhere else is swallowed, so a pocket or a
 * stray finger cannot clear an alert nobody has read.
 *
 * Keeping the takeover for emergencies is deliberate. If every broadcast seized the screen,
 * people would learn to dismiss them unread, which is exactly how a real alert loses its
 * force -- and then the one that matters is the one nobody reads.
 */
#pragma once

#include "lvgl.h"

typedef enum {
    UI_ALERT_ANNOUNCEMENT = 0,
    UI_ALERT_EMERGENCY,
} ui_alert_kind_t;

/* Builds the overlay once, on LVGL's top layer. Call with the display lock held. */
void ui_alert_start(void);

/*
 * Raises an alert over whatever is on screen. `who` is the sender as it should be read, `text`
 * the message. An emergency replaces an announcement already showing; an announcement never
 * replaces an emergency, because the urgent one has not been acknowledged yet.
 */
void ui_alert_show(ui_alert_kind_t kind, const char *who, const char *text);

/* True while an alert is up, so the notification banner does not also fire for it. */
bool ui_alert_showing(void);
