/*
 * gps.h - grid time from a GPS module on MAIN (D63).
 *
 * A u-blox-class module (the GT-U7) sends NMEA at 9600 baud on one wire into
 * CONFIG_LG_NODE_GPS_RX_GPIO. Its own task reads the sentences, checks their checksums, and
 * takes the time and date from RMC once the module reports a fix, and the satellite count from
 * GGA. Each fixed RMC is posted to the core task as NODE_CMD_GPS_TIME; the core task decides what
 * the grid does with it (node_main.c). Only MAIN starts it.
 *
 * Precision is NMEA's: about a second, well inside D6's 120 s tolerance. The PPS pin is not used.
 *
 * Position (D64): the fix's latitude and longitude are kept in RAM here, for MAIN's own admin page
 * only. Never written to flash, never put on the backbone or sent to a handheld.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool     started;       /* MAIN, with a pin configured */
    bool     heard;         /* a valid sentence has arrived: something is wired and talking */
    bool     fix;           /* a fixed RMC within GPS_FRESH_MS */
    uint8_t  sats;          /* satellites in use, from GGA */
    uint32_t last_unix;     /* time of the last fixed RMC, 0 never */
    uint32_t fix_age_ms;    /* since that RMC; UINT32_MAX never */
    uint32_t sentences;     /* valid sentences since boot */
    uint32_t bad;           /* sentences whose checksum failed */
    bool     has_pos;       /* lat_u and lon_u hold the last fix's position */
    int32_t  lat_u;         /* microdegrees, north positive */
    int32_t  lon_u;         /* microdegrees, east positive */
} gps_state_t;

/* Starts the reader on rx_gpio (UART2). ESP_ERR_INVALID_ARG for a negative pin. */
esp_err_t gps_start(int rx_gpio);

/* Copies the state; safe from any task. */
void gps_state(gps_state_t *out);

/* True while the module has a fix: grid time comes from it and cannot be set by hand. */
bool gps_has_fix(void);

/* Console `gps raw`: prints the next bytes as they arrive, garbage as <hex>, to diagnose wiring or baud. */
void gps_dump(uint32_t bytes);
