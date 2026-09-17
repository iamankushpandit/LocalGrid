/*
 * spike_chat.h - the no-LVGL spike's 1:1 chat: scrolling bubbles, an input field, a keyboard.
 * Called from the spike task only (lg_draw has one drawing task).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hh_service.h"

/* Opens a chat with the first handheld in the status snapshot and draws the whole screen. */
void spike_chat_open(uint16_t w, uint16_t h, const hh_status_t *st);

/* Leaves the chat: turns the hardware scroll area off before another screen draws. */
void spike_chat_close(void);

/* Repaints the list if the messages changed; flashes the new-message arrow when scrolled up. */
void spike_chat_refresh(const hh_status_t *st);

/* Call every loop: runs the arrow's slow flash. */
void spike_chat_tick(uint32_t now_ms);

/* Console: 0 letters, 1 numbers, 2 emoji, 3 press shift. */
void spike_chat_page(int page);

/* A touch sample: true when the back arrow was tapped. */
bool spike_chat_touch(int16_t x, int16_t y, bool down);

/* Console-driven measurements: scroll, show or hide the keyboard, type through the keys. */
void spike_chat_scroll_by(int16_t dy);
void spike_chat_keyboard(bool show);
void spike_chat_type(const char *text);
void spike_chat_log(void);
