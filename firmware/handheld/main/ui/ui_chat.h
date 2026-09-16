/*
 * ui_chat.h - conversation list and chat screens (P6).
 * Reads hh_service.h only; never touches the network directly (D27).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Shows the conversation list, building the screens on first use. */
void ui_chat_open_list(void);

/* Opens one conversation directly, for a tapped notification. */
void ui_chat_open_conversation(uint8_t scope, uint32_t target, const char *title);

/* True when that conversation is the one on screen. */
bool ui_chat_conversation_open(uint8_t scope, uint32_t target);
