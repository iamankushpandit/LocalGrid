/*
 * power_trace.h - what an AP was doing in the seconds before it lost power.
 *
 * The bench APs brown out: the supply dips below the ESP32's detector threshold and the chip
 * resets. Their boards cannot measure their own supply (lg_power.h), so the evidence has to be
 * what the AP was doing when it happened. Radio transmit bursts, a station associating, and
 * flash writes each draw current spikes, and a weak USB hub sags under them.
 *
 * Every second the core task records one snapshot into RTC memory that a brownout or any other
 * reset keeps (a real power loss clears it): uptime, heap, associated stations, backbone links,
 * backbone frames sent that second, and which events happened. It also prints that second:
 *     [UP] 523 s heap 58 KB min 47 KB sta 2 links 2 tx 3 time CARRIED ev sta_join,nvs_write
 * On the next boot, if the record survived, the last snapshots are printed oldest first under
 * "[PWR] The last N seconds before this restart (<cause>)", so a brownout names its moment even
 * when nobody was capturing the serial port. CONFIG_LG_NODE_HEARTBEAT silences the [UP] line
 * but keeps recording.
 */
#pragma once

#include <stdint.h>

typedef enum {
    PTRACE_STA_JOIN      = 1u << 0,   /* a station associated with this SoftAP */
    PTRACE_STA_LEAVE     = 1u << 1,
    PTRACE_SESSION       = 1u << 2,   /* a handheld TCP session opened */
    PTRACE_LINK_UP       = 1u << 3,   /* backbone link confirmed */
    PTRACE_LINK_DOWN     = 1u << 4,
    PTRACE_NVS_WRITE     = 1u << 5,   /* a flash commit */
    PTRACE_WEB           = 1u << 6,   /* the admin page served a request */
    PTRACE_BLE_UPDATE    = 1u << 7,   /* BLE advertising data changed */
    PTRACE_GRID_ANNOUNCE = 1u << 8,   /* grid state flooded */
    PTRACE_TIME          = 1u << 9,   /* grid time set or adopted */
    PTRACE_SCAN_BURST    = 1u << 10,  /* more than 10 backbone frames in the second */
} ptrace_event_t;

/* Call once at boot, after the reset cause is known: prints the surviving record, starts anew. */
void ptrace_boot(const char *reset_cause);

/* How long the run before this boot lasted, from the surviving record; UINT32_MAX when unknown. */
uint32_t ptrace_prev_run_s(void);

/* Any task: marks an event in the current second. */
void ptrace_event(ptrace_event_t ev);

/* Core task, once a second: record the snapshot and print the [UP] line. */
void ptrace_second(uint32_t uptime_s, uint32_t backbone_tx_total, uint8_t links, const char *time_quality);
