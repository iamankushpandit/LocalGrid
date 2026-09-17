/*
 * spike_chat.h - the no-LVGL spike's 1:1 chat: scrolling bubbles, an input field, a keyboard.
 * Called from the spike task only (lg_draw has one drawing task).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hh_service.h"

/* Opens a conversation (LG_SCOPE_DIRECT with a device, LG_SCOPE_GROUP with a group id, or
 * LG_SCOPE_BROADCAST) and draws the whole screen. */
void spike_chat_open(uint16_t w, uint16_t h, uint8_t scope, uint32_t target, const char *title);

/* Draws the chat again where the reader left it (after an overlay). */
void spike_chat_redraw(void);

/* True when this chat is showing that conversation. */
bool spike_chat_is(uint8_t scope, uint32_t target);

/* Leaves the chat: turns the hardware scroll area off before another screen draws. */
void spike_chat_close(void);

/* Repaints the list if the messages changed; flashes the new-message arrow when scrolled up. */
void spike_chat_refresh(const hh_status_t *st);

/* Call every loop: runs the arrow's slow flash. */
void spike_chat_tick(uint32_t now_ms);

/* Console: 0 letters, 1 numbers, 2 emoji, 3 press shift. */
void spike_chat_page(int page);

/* A touch sample: true when the back arrow was tapped (back to the conversation list). */
bool spike_chat_touch(int16_t x, int16_t y, bool down);

/* Console-driven measurements: scroll, show or hide the keyboard, type through the keys. */
void spike_chat_scroll_by(int16_t dy);
void spike_chat_keyboard(bool show);
void spike_chat_type(const char *text);
void spike_chat_log(void);
