/*
 * lg_nmea.h - one NMEA 0183 sentence from a GPS module into numbers (D63, D65).
 *
 * Portable C11: no ESP-IDF, no I/O, no state. The caller collects a line (from '$' up to, not
 * including, CR/LF) and hands it here; what to do with the result is the caller's business. MAIN
 * and the handhelds read the same GT-U7 module, so they share this parser.
 *
 * Understood, from any talker (GP, GN, GL, GA, BD): RMC (time, date, fix status, position, speed,
 * course), GGA (fix quality, satellites in use, HDOP, altitude), GSA (2D or 3D, PDOP) and GSV
 * (satellites in view and their signal strength, one of several sentences). Everything else with a
 * good checksum is LG_NMEA_OTHER.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LG_NMEA_LINE_MAX 96   /* NMEA allows 82 characters; longer is noise */

typedef enum {
    LG_NMEA_BAD = 0,    /* not a sentence, or its checksum failed */
    LG_NMEA_OTHER,      /* a good sentence this parser does not read */
    LG_NMEA_RMC,
    LG_NMEA_GGA,
    LG_NMEA_GSA,
    LG_NMEA_GSV,
} lg_nmea_kind_t;

#define LG_NMEA_NONE_U16 0xFFFFu   /* a number the sentence left empty */

typedef struct {
    lg_nmea_kind_t kind;
    bool     valid;      /* the checksum held: something is wired and talking at the right baud */
    bool     fix;        /* RMC with status A and a sane time and date; unix_s and millis are set */
    uint32_t unix_s;     /* Unix seconds of the fix (UTC) */
    uint16_t millis;     /* 0..999 past unix_s */
    bool     has_pos;    /* RMC with a fix and a readable position */
    int32_t  lat_u;      /* microdegrees, north positive */
    int32_t  lon_u;      /* microdegrees, east positive */
    bool     has_sats;   /* GGA carried a satellite count */
    uint8_t  sats;       /* satellites in use */
    /* RMC, with a fix */
    uint16_t speed_cms;  /* ground speed, cm/s; LG_NMEA_NONE_U16 if empty */
    uint16_t course_cd;  /* course over ground, centidegrees true; LG_NMEA_NONE_U16 if empty */
    /* GGA */
    uint8_t  quality;    /* 0 no fix, 1 GPS, 2 differential (SBAS/WAAS), 4-5 RTK, 6 estimated */
    uint16_t hdop_c;     /* horizontal dilution x100; LG_NMEA_NONE_U16 if empty */
    bool     has_alt;
    int32_t  alt_dm;     /* altitude above mean sea level, decimetres */
    /* GSA */
    uint8_t  fix_type;   /* 1 none, 2 2D, 3 3D; 0 if empty */
    uint16_t pdop_c;     /* position dilution x100; LG_NMEA_NONE_U16 if empty */
    /* GSV: one of gsv_total sentences, numbered gsv_num from 1 */
    uint8_t  gsv_total;
    uint8_t  gsv_num;
    uint8_t  in_view;    /* satellites in view, repeated in every GSV of a set */
    uint8_t  gsv_n;      /* satellites described in this sentence, up to 4 */
    uint8_t  snr[4];     /* their signal strength, dB-Hz; 0 when not tracked */
} lg_nmea_t;

/* "$....*HH": true when the XOR of everything between '$' and '*' equals HH. */
bool lg_nmea_checksum_ok(const char *line);

/* Parses one NUL-terminated line into out (always fully written) and returns out->kind. */
lg_nmea_kind_t lg_nmea_parse(const char *line, lg_nmea_t *out);

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
int64_t lg_nmea_days_from_civil(int y, int m, int d);

#ifdef __cplusplus
}
#endif
