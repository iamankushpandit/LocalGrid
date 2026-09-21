/*
 * gps.h - grid time from a GPS module on MAIN (D63), read on a schedule (D73).
 *
 * A u-blox-class module (the GT-U7) sends NMEA at 9600 baud on one wire into
 * CONFIG_LG_NODE_GPS_RX_GPIO. Its own task reads the sentences, checks their checksums, and
 * takes the time and date from RMC once the module reports a fix, and the satellite count from
 * GGA. Each fixed RMC is posted to the core task as NODE_CMD_GPS_TIME; the core task decides what
 * the grid does with it (node_main.c). Only MAIN starts it.
 *
 * D73: Wi-Fi, BLE and LoRa are first-class for RAM and the GPS gives way. The admin sets an
 * interval in the grid settings (replicated between APs, so every AP agrees); between readings
 * this task deletes the UART driver and gives its buffers back, and at each reading it installs
 * them again, reads until there is a fix or a bounded window passes, and releases them. On
 * "always on" nothing is ever released and the behaviour is exactly what it was before D73.
 *
 * Precision is NMEA's: about a second, well inside D6's 120 s tolerance. The PPS pin is not used.
 *
 * Position (D64, D65): the fix's latitude and longitude are kept in RAM here. The core task shares
 * them with the grid as this AP's POSITION (every 30 s, or at once when it moves), so handhelds
 * can point to MAIN and every admin page can map it. Between readings the position simply ages,
 * and its age is already carried. Never written to flash.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_gps_plan.h"

typedef struct {
    bool     started;       /* MAIN, with a pin configured */
    bool     heard;         /* a valid sentence has arrived: something is wired and talking */
    bool     fix;           /* a fixed RMC within GPS_FRESH_MS, while a reading is open */
    uint8_t  sats;          /* satellites in use, from GGA */
    uint32_t last_unix;     /* time of the last fixed RMC, 0 never */
    uint32_t fix_age_ms;    /* since that RMC; UINT32_MAX never */
    uint32_t sentences;     /* valid sentences since boot */
    uint32_t bad;           /* sentences whose checksum failed */
    bool     has_pos;       /* lat_u and lon_u hold the last fix's position */
    int32_t  lat_u;         /* microdegrees, north positive */
    int32_t  lon_u;         /* microdegrees, east positive */
    /* D73: the schedule and what it costs. */
    uint8_t  phase;         /* lg_gps_phase_t: reading now, or waiting for the next reading */
    bool     always;        /* the plan never releases the module */
    uint16_t plan;          /* the stored form the grid agreed */
    uint16_t interval_s;    /* what that means in seconds; 0 when always on */
    uint32_t next_in_s;     /* until the next reading; 0 while one is running */
    uint32_t readings;      /* readings begun since boot */
    uint32_t fixes;         /* of those, ones that reached a fix */
    uint32_t last_ttf_ms;   /* time to a fix in the last reading that got one; UINT32_MAX never */
    uint32_t freed_bytes;   /* heap the last release gave back; 0 until one has happened */
} gps_state_t;

/* Starts the reader on rx_gpio (UART2) with the grid's plan. ESP_ERR_INVALID_ARG for a negative pin. */
esp_err_t gps_start(int rx_gpio, uint16_t plan);

/* The grid's plan changed (the admin page here, or another AP's settings). Any task; the GPS task
 * picks it up at its next pass, so no reading is ever cut in half by a setting. */
void gps_set_plan(uint16_t plan);

/* Bring the next reading forward to now: the console asking for raw bytes, or a fresh position. */
void gps_wake(void);

/* Copies the state; safe from any task. */
void gps_state(gps_state_t *out);

/*
 * True while the GPS is the grid's timekeeper, so grid time cannot be set by hand (D63). Reading
 * continuously that means a fix within the last few seconds; on a schedule it means the last
 * reading found one and the next is not overdue (lg_gps_plan_hold_ms), because a module that is
 * released between readings has no live fix to show and has not stopped being the source.
 */
bool gps_has_fix(void);

/* Console `gps raw`: prints the next bytes as they arrive, garbage as <hex>, to diagnose wiring or
 * baud. Wakes the module when it is between readings, so the bytes actually come. */
void gps_dump(uint32_t bytes);
