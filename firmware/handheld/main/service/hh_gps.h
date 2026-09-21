/*
 * hh_gps.h - a GPS module on a handheld (D65).
 *
 * A GT-U7 (u-blox class) sends NMEA at 9600 baud into the board profile's gps_rx pin. Its own
 * low-priority task reads lines, parses them with lg_nmea, keeps the state below, and calls the
 * service's callback, which only posts to the service task's queue: the service task decides what
 * the clock and the grid do with a fix. Only boards whose profile names a gps_rx pin start it;
 * with nothing fitted the pin idles pulled up, nothing is heard, and nothing is shown or sent.
 *
 * D73: a handheld's GPS costs battery as well as memory, and Wi-Fi, BLE and LoRa come first for
 * both. The reader follows the same interval the grid replicates to every AP and pushes to every
 * handheld: between readings the UART driver and its buffers are released, and each reading lasts
 * until there is a fix or a bounded window passes. "Always on" behaves exactly as before D73.
 *
 * Service-side only (D27): screens learn about the GPS through hh_service.h, never from here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_gps_plan.h"

#define HH_GPS_FRESH_MS 5000u   /* a fix older than this is lost: the module stopped reporting one */

typedef struct {
    bool     started;       /* this board has a GPS pin and the reader runs */
    bool     heard;         /* a valid sentence has arrived since boot */
    bool     talking;       /* a valid sentence within HH_GPS_FRESH_MS: fitted and talking now */
    bool     fix;           /* a fixed RMC within HH_GPS_FRESH_MS */
    uint8_t  sats;          /* satellites in use, from GGA */
    uint32_t sentences;     /* valid sentences since boot */
    uint32_t bad;           /* sentences whose checksum failed */
    bool     has_pos;       /* lat_u and lon_u hold the last fix's position */
    int32_t  lat_u;         /* microdegrees, north positive */
    int32_t  lon_u;         /* microdegrees, east positive */
    uint32_t last_unix;     /* Unix seconds of the last fixed RMC, 0 never */
    uint16_t last_millis;   /* past last_unix */
    uint32_t fix_ms;        /* esp_timer ms when that RMC's '$' arrived, 0 never */
    uint32_t fix_age_ms;    /* since then, filled in on read; UINT32_MAX never */
    /* Everything else the module says (D65, Status): see lg_nmea_t for units. */
    uint8_t  quality;       /* GGA: 0 none, 1 GPS, 2 differential (SBAS/WAAS) */
    uint8_t  fix_type;      /* GSA: 1 none, 2 2D, 3 3D */
    uint16_t hdop_c;        /* x100, LG_NMEA_NONE_U16 unknown */
    uint16_t pdop_c;
    bool     has_alt;
    int32_t  alt_dm;
    uint16_t speed_cms;     /* LG_NMEA_NONE_U16 unknown */
    uint16_t course_cd;
    uint8_t  in_view;       /* the last complete GSV set */
    uint8_t  tracked;       /* of those, with a signal */
    uint8_t  best_snr;      /* dB-Hz */
    int      rx_gpio;       /* -1 when not started */
    /* D73: the schedule, and what it saves. */
    uint8_t  phase;         /* lg_gps_phase_t: reading now, or waiting for the next reading */
    bool     always;        /* the plan never releases the module */
    uint16_t plan;          /* the stored form the grid agreed */
    uint16_t interval_s;    /* what that means in seconds; 0 when always on */
    uint32_t next_in_s;     /* until the next reading; 0 while one is running */
    uint32_t readings;      /* readings begun since boot */
    uint32_t fixes;         /* of those, ones that reached a fix */
    uint32_t last_ttf_ms;   /* time to a fix in the last reading that got one; UINT32_MAX never */
    uint32_t freed_bytes;   /* heap the last release gave back; 0 until one has happened */
} hh_gps_state_t;

/* Called on the GPS task after each fixed RMC and when a fix is lost. Must only copy and return. */
typedef void (*hh_gps_notify_t)(void);

/* Starts the reader on rx_gpio (and tx_gpio, or -1) with the plan this handheld last saved (D73).
 * ESP_ERR_INVALID_ARG for a negative rx pin: a board with no GPS connector. */
esp_err_t hh_gps_start(int rx_gpio, int tx_gpio, uint16_t plan, hh_gps_notify_t notify);

/* D73: a new plan from the AP. Any task; the GPS task picks it up at its next pass, so no reading
 * is ever cut in half by a setting arriving. */
void hh_gps_set_plan(uint16_t plan);

/* Bring the next reading forward to now: the console asking for raw bytes, or a screen that wants
 * a fresh position rather than an ageing one. */
void hh_gps_wake(void);

/* How long a fix from the last reading stays this handheld's answer, in milliseconds (D65). */
uint32_t hh_gps_hold_ms(void);

/* Copies the state; safe from any task, and before or without a start (all zero, rx_gpio -1). */
void hh_gps_state(hh_gps_state_t *out);

/* Console `gps raw`: prints the next bytes as they arrive, garbage as <hex>, to diagnose wiring or baud. */
void hh_gps_dump(uint32_t bytes);

/* Every byte received since boot, good or not. */
uint32_t hh_gps_bytes(void);
