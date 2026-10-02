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
    uint32_t budget_drops;     /* frames dropped because this pass had already waited long enough */
    uint32_t slowest_pass_ms;  /* the longest all of one pass's sends made the core task wait */
} sess_traffic_t;

void sess_traffic(sess_traffic_t *out);

/*
 * Called once at the top of each core-task pass, before anything can send.
 *
 * Why it has to exist. SEND_WAIT_MS bounds how long ONE handheld may hold up the core task, and
 * that was read as bounding the task. It does not: a pass can send to every session, and three
 * handhelds whose sockets are full cost SEND_WAIT_MS each. The task watchdog bounds the pass at
 * 5 s and panics, so 3 x 2 s restarted the AP -- observed on Everest with three handhelds joining
 * at once, two sessions closing with "send failed" two seconds apart and the abort right after
 * the second. The per-send cap was never the dangerous number; the missing per-pass one was.
 */
void sess_pass_begin(void);
