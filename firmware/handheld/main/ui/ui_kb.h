/*
 * ui_kb.h - the on-screen keyboard, shared by chat, group names, and renaming.
 *
 * Four rows at the bottom of the panel. The phone keypad is the default (D59): four columns,
 * about 60 px a key, letters typed by tapping a key until the one you want shows, as SMS phones
 * did. The full qwerty keyboard is one key away, with shift (once for one capital, twice for caps
 * lock); there are also a numbers and symbols page and two emoji pages. Whichever letters page was
 * last used comes back. It edits a caller's UTF-8 buffer in place. A press repaints one key; a page
 * change repaints the keyboard. UI task only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lg_draw.h"

typedef enum {
    KB_NOTHING = 0,
    KB_TEXT_CHANGED,   /* the buffer changed: the caller repaints its field */
    KB_HIDE,           /* the hide key: the caller closes the keyboard */
} kb_event_t;

/* Pixel height the keyboard takes at the bottom of the panel. */
int16_t ui_kb_height(void);

/* Binds the keyboard to buf (cap bytes including the NUL) and lays it out at the bottom. */
void ui_kb_open(uint16_t screen_w, uint16_t screen_h, char *buf, size_t cap);

void ui_kb_draw(void);

/* A touch sample. Returns true when the touch belongs to the keyboard. */
bool ui_kb_touch(int16_t x, int16_t y, bool down, kb_event_t *event);

/* Console: type ASCII through the keys, or change page (0 letters, 1 numbers, 2 emoji, 3 shift,
 * 4 keypad). */
kb_event_t ui_kb_type(const char *text);
void ui_kb_page(int page);

void ui_kb_stats(uint32_t *paints, uint64_t *us);
