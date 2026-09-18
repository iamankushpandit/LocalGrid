/*
 * NMEA parsing shared by every firmware that reads a GPS (D63, D65). Moved out of MAIN's
 * firmware/node/main/gps.c: same checksum rule, same fields, same limits, plus digit and range
 * checks so a corrupted but checksummed field is refused rather than read as a number.
 */
#include "lg_nmea.h"

#include <stdlib.h>
#include <string.h>

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool lg_nmea_checksum_ok(const char *line)
{
    const char *star = strchr(line, '*');
    if (line[0] != '$' || star == NULL || hex(star[1]) < 0 || hex(star[2]) < 0) {
        return false;
    }
    uint8_t x = 0;
    for (const char *p = line + 1; p < star; p++) {
        x ^= (uint8_t)*p;
    }
    return x == (uint8_t)(hex(star[1]) << 4 | hex(star[2]));
}

/* The n-th comma-separated field (0 = the sentence name), copied into out. */
static bool field(const char *line, int n, char *out, size_t cap)
{
    const char *p = line;
    for (int i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (p == NULL) {
            return false;
        }
        p++;
    }
    size_t len = strcspn(p, ",*");
    if (len >= cap) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool digits(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

static int two(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

int64_t lg_nmea_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* NMEA's ddmm.mmmmm (or dddmm.mmmmm) and a hemisphere letter, to signed microdegrees. */
static bool coord(const char *v, const char *hemi, int deg_digits, int32_t *out)
{
    size_t len = strlen(v);
    if (len < (size_t)deg_digits + 2 || hemi[0] == '\0' || !digits(v, (size_t)deg_digits)) {
        return false;
    }
    int deg = 0;
    for (int i = 0; i < deg_digits; i++) {
        deg = deg * 10 + (v[i] - '0');
    }
    double minutes = strtod(v + deg_digits, NULL);
    if (minutes < 0.0 || minutes >= 60.0 || deg > (deg_digits == 2 ? 90 : 180)) {
        return false;
    }
    double d = deg + minutes / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') {
        d = -d;
    }
    *out = (int32_t)(d * 1e6 + (d < 0 ? -0.5 : 0.5));
    return true;
}

static uint16_t scaled(const char *line, int n, double scale);

/* RMC: time hhmmss.sss, status A (fix) or V, lat, N/S, lon, E/W, speed, course, date ddmmyy. */
static void rmc(const char *line, lg_nmea_t *out)
{
    char t[16], status[4], date[10];
    if (!field(line, 1, t, sizeof(t)) || !field(line, 2, status, sizeof(status)) ||
        !field(line, 9, date, sizeof(date)) || strlen(t) < 6 || strlen(date) != 6 || !digits(t, 6) ||
        !digits(date, 6)) {
        return;
    }
    if (status[0] != 'A') {
        return;   /* no fix: the module's own clock may be anything */
    }
    int hh = two(t), mm = two(t + 2), ss = two(t + 4);
    int ms = t[6] == '.' ? (int)(strtod(t + 6, NULL) * 1000.0 + 0.5) : 0;
    int day = two(date), mon = two(date + 2), yr = 2000 + two(date + 4);
    if (hh > 23 || mm > 59 || ss > 60 || mon < 1 || mon > 12 || day < 1 || day > 31 || ms < 0 || ms > 999) {
        return;
    }
    out->fix = true;
    out->unix_s = (uint32_t)(lg_nmea_days_from_civil(yr, mon, day) * 86400 + hh * 3600 + mm * 60 + ss);
    uint16_t knots_c = scaled(line, 7, 100.0);   /* 1 knot = 51.444 cm/s */
    out->speed_cms = knots_c == LG_NMEA_NONE_U16 ? LG_NMEA_NONE_U16 : (uint16_t)((uint32_t)knots_c * 51444u / 100000u);
    out->course_cd = scaled(line, 8, 100.0);
    out->millis = (uint16_t)ms;
    char lat[16], ns[4], lon[16], ew[4];
    out->has_pos = field(line, 3, lat, sizeof(lat)) && field(line, 4, ns, sizeof(ns)) &&
                   field(line, 5, lon, sizeof(lon)) && field(line, 6, ew, sizeof(ew)) &&
                   coord(lat, ns, 2, &out->lat_u) && coord(lon, ew, 3, &out->lon_u);
    if (!out->has_pos) {
        out->lat_u = 0;
        out->lon_u = 0;
    }
}

/* A small non-negative integer field; -1 when empty or not a number. */
static int small_int(const char *line, int n)
{
    char v[8];
    if (!field(line, n, v, sizeof(v)) || v[0] == '\0' || !digits(v, strlen(v))) {
        return -1;
    }
    int x = atoi(v);
    return x > 255 ? 255 : x;
}

/* A decimal field times scale, as an unsigned 16-bit number; LG_NMEA_NONE_U16 when empty. */
static uint16_t scaled(const char *line, int n, double scale)
{
    char v[16];
    if (!field(line, n, v, sizeof(v)) || v[0] == '\0') {
        return LG_NMEA_NONE_U16;
    }
    double x = strtod(v, NULL) * scale + 0.5;
    return x < 0.0 ? LG_NMEA_NONE_U16 : x >= 65534.0 ? (uint16_t)65534u : (uint16_t)x;
}

/* GGA: 6 quality, 7 satellites in use, 8 HDOP, 9 altitude (metres). */
static void gga(const char *line, lg_nmea_t *out)
{
    int v = small_int(line, 7);
    if (v >= 0) {
        out->sats = (uint8_t)v;
        out->has_sats = true;
    }
    int q = small_int(line, 6);
    out->quality = q < 0 ? 0u : (uint8_t)q;
    out->hdop_c = scaled(line, 8, 100.0);
    char a[16];
    if (field(line, 9, a, sizeof(a)) && a[0] != '\0') {
        double m = strtod(a, NULL);
        out->alt_dm = (int32_t)(m * 10.0 + (m < 0 ? -0.5 : 0.5));
        out->has_alt = true;
    }
}

/* GSA: 2 fix type (1 none, 2 2D, 3 3D), 15 PDOP. */
static void gsa(const char *line, lg_nmea_t *out)
{
    int t = small_int(line, 2);
    out->fix_type = t < 0 ? 0u : (uint8_t)t;
    out->pdop_c = scaled(line, 15, 100.0);
}

/* GSV: 1 sentences in the set, 2 this one's number, 3 satellites in view, then four fields a
 * satellite (number, elevation, azimuth, SNR) for up to four satellites. */
static void gsv(const char *line, lg_nmea_t *out)
{
    int total = small_int(line, 1), num = small_int(line, 2), view = small_int(line, 3);
    if (total <= 0 || num <= 0 || num > total || view < 0) {
        return;
    }
    out->gsv_total = (uint8_t)total;
    out->gsv_num = (uint8_t)num;
    out->in_view = (uint8_t)view;
    for (int k = 0; k < 4; k++) {
        char id[6];
        if (!field(line, 4 + 4 * k, id, sizeof(id)) || id[0] == '\0') {
            break;
        }
        int snr = small_int(line, 7 + 4 * k);
        out->snr[out->gsv_n++] = snr < 0 ? 0u : (uint8_t)snr;
    }
}

lg_nmea_kind_t lg_nmea_parse(const char *line, lg_nmea_t *out)
{
    memset(out, 0, sizeof(*out));
    out->speed_cms = out->course_cd = out->hdop_c = out->pdop_c = LG_NMEA_NONE_U16;
    if (line == NULL || strlen(line) >= LG_NMEA_LINE_MAX || !lg_nmea_checksum_ok(line)) {
        out->kind = LG_NMEA_BAD;
        return out->kind;
    }
    out->valid = true;
    out->kind = LG_NMEA_OTHER;
    /* Talker IDs vary by module and constellation: GP, GN, GL, GA, BD. */
    if (strlen(line) > 6 && strncmp(line + 3, "RMC", 3) == 0) {
        out->kind = LG_NMEA_RMC;
        rmc(line, out);
    } else if (strlen(line) > 6 && strncmp(line + 3, "GGA", 3) == 0) {
        out->kind = LG_NMEA_GGA;
        gga(line, out);
    } else if (strlen(line) > 6 && strncmp(line + 3, "GSA", 3) == 0) {
        out->kind = LG_NMEA_GSA;
        gsa(line, out);
    } else if (strlen(line) > 6 && strncmp(line + 3, "GSV", 3) == 0) {
        out->kind = LG_NMEA_GSV;
        gsv(line, out);
    }
    return out->kind;
}
