/*
 * sessions.h - TCP control sessions for handhelds.
 *
 * Framing: u16 little-endian length, then one lg frame (32..1024 bytes).
 * A session must register within 5 s and is closed after 30 s without data;
 * handhelds send PING every 10 s.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sess_init(uint16_t port);

/* Waits up to timeout_ms for socket activity, then handles it. */
void    sess_poll(int timeout_ms);
void    sess_send(uint32_t device, const uint8_t *frame, size_t len);

/* Whether a handheld has an open TCP session here. A reply to one that does not may still have
 * somewhere to go: a handheld heard over LoRa (D76). */
bool    sess_has(uint32_t device);
uint8_t sess_registered_count(void);
void    sess_print(void);

/* Talk frames dropped because a listener's socket was full (D61): the AP never waits on one. */
uint32_t sess_voice_dropped(void);

/* D70: how busy the handheld side is, for the BLE admin link's TRAFFIC reply. Counts and sizes
 * only; no addresses (D21) and no frame content. */
typedef struct {
    uint8_t  open_now;
    uint8_t  registered_now;
    uint32_t opened;           /* sessions accepted since boot */
    uint32_t registrations;
    uint32_t disconnects;
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t slowest_send_ms;  /* the longest one send() made the core task wait */
    uint32_t send_timeouts;    /* sends abandoned after SEND_WAIT_MS, closing the session */
    uint32_t refused;          /* connections refused because the session table was full */
} sess_traffic_t;

void sess_traffic(sess_traffic_t *out);
