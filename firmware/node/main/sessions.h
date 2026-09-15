/*
 * sessions.h - TCP control sessions for handhelds.
 *
 * Framing: u16 little-endian length, then one lg frame (32..1024 bytes).
 * A session must register within 5 s and is closed after 30 s without data;
 * handhelds send PING every 10 s.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sess_init(uint16_t port);

/* Waits up to timeout_ms for socket activity, then handles it. */
void    sess_poll(int timeout_ms);
void    sess_send(uint32_t device, const uint8_t *frame, size_t len);
uint8_t sess_registered_count(void);
void    sess_print(void);
