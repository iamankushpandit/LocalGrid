/*
 * ui_notify.h - tells the owner about a message that arrived while another screen was up.
 *
 * A banner names the sender and shows a preview; tapping it opens that conversation.
 * Unread counts appear on the Messages button and on each conversation row.
 */
#pragma once

#include <stdint.h>

/* Starts the watcher. Call once, after the home screen exists. */
void ui_notify_start(void);

/* Unread messages across every conversation, and for one conversation. */
uint32_t ui_notify_unread_total(void);
uint32_t ui_notify_unread(uint8_t scope, uint32_t target);

/* Everything in this conversation has been seen. */
void ui_notify_mark_seen(uint8_t scope, uint32_t target);
