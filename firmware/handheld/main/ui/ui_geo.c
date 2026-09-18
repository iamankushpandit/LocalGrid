/*
 * Distance, direction, and their words between two microdegree positions (D65). Float is plenty:
 * the differences are taken in integer microdegrees first, so a few metres apart stays exact.
 */
#include "ui_geo.h"

#include <math.h>
#include <stdio.h>

#define EARTH_R_M   6371000.0f
#define DEG_TO_RAD  0.017453292519943295f
#define U_TO_RAD    (DEG_TO_RAD / 1000000.0f)
#define NEAR_M      10u   /* closer than this, a direction is GPS noise */

/* Longitude difference in microdegrees, taken the short way round (-180..180 degrees). */
static int64_t lon_diff_u(int32_t lon1_u, int32_t lon2_u)
{
    int64_t d = (int64_t)lon2_u - (int64_t)lon1_u;
    while (d > 180000000) {
        d -= 360000000;
    }
    while (d < -180000000) {
        d += 360000000;
    }
    return d;
}

uint32_t ui_geo_distance_m(int32_t lat1_u, int32_t lon1_u, int32_t lat2_u, int32_t lon2_u)
{
    float phi1 = (float)lat1_u * U_TO_RAD;
    float phi2 = (float)lat2_u * U_TO_RAD;
    float dphi = (float)((int64_t)lat2_u - (int64_t)lat1_u) * U_TO_RAD;
    float dlam = (float)lon_diff_u(lon1_u, lon2_u) * U_TO_RAD;
    float sp = sinf(dphi / 2.0f);
    float sl = sinf(dlam / 2.0f);
    float a = sp * sp + cosf(phi1) * cosf(phi2) * sl * sl;
    if (a > 1.0f) {
        a = 1.0f;
    }
    float c = 2.0f * asinf(sqrtf(a));
    float m = EARTH_R_M * c;
    return m < 0.0f ? 0u : (uint32_t)(m + 0.5f);
}

uint16_t ui_geo_bearing_deg(int32_t lat1_u, int32_t lon1_u, int32_t lat2_u, int32_t lon2_u)
{
    float phi1 = (float)lat1_u * U_TO_RAD;
    float phi2 = (float)lat2_u * U_TO_RAD;
    float dlam = (float)lon_diff_u(lon1_u, lon2_u) * U_TO_RAD;
    float y = sinf(dlam) * cosf(phi2);
    float x = cosf(phi1) * sinf(phi2) - sinf(phi1) * cosf(phi2) * cosf(dlam);
    float deg = atan2f(y, x) / DEG_TO_RAD;
    int d = (int)(deg + (deg < 0.0f ? -0.5f : 0.5f));
    d %= 360;
    if (d < 0) {
        d += 360;
    }
    return (uint16_t)d;
}

const char *ui_geo_compass(uint16_t deg)
{
    static const char *const POINTS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return POINTS[((uint32_t)(deg % 360u) * 10u + 225u) / 450u % 8u];
}

void ui_geo_distance_text(uint32_t metres, char *out, size_t cap)
{
    if (metres < 100u) {
        snprintf(out, cap, "%u m", (unsigned)metres);
        return;
    }
    uint32_t tens = (metres + 5u) / 10u * 10u;
    if (tens < 1000u) {
        snprintf(out, cap, "%u m", (unsigned)tens);
        return;
    }
    uint32_t tenths = (metres + 50u) / 100u;   /* tenths of a kilometre */
    if (tenths < 100u) {
        snprintf(out, cap, "%u.%u km", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
        return;
    }
    snprintf(out, cap, "%u km", (unsigned)((metres + 500u) / 1000u));
}

/* One coordinate with five decimals, rounded, from integers (no float formatting). */
static int coord_part(int32_t u, char *out, size_t cap)
{
    int64_t v = u;
    const char *sign = v < 0 ? "-" : "";
    if (v < 0) {
        v = -v;
    }
    int64_t fifths = (v + 5) / 10;   /* hundred-thousandths of a degree */
    if (fifths == 0) {
        sign = "";
    }
    return snprintf(out, cap, "%s%ld.%05ld", sign, (long)(fifths / 100000), (long)(fifths % 100000));
}

void ui_geo_coord_text(int32_t lat_u, int32_t lon_u, char *out, size_t cap)
{
    char lat[16];
    char lon[16];
    (void)coord_part(lat_u, lat, sizeof(lat));
    (void)coord_part(lon_u, lon, sizeof(lon));
    snprintf(out, cap, "%s, %s", lat, lon);
}

void ui_geo_where_text(int32_t from_lat_u, int32_t from_lon_u, int32_t to_lat_u, int32_t to_lon_u, char *out,
                       size_t cap)
{
    uint32_t m = ui_geo_distance_m(from_lat_u, from_lon_u, to_lat_u, to_lon_u);
    if (m < NEAR_M) {
        snprintf(out, cap, "within %u m", (unsigned)NEAR_M);
        return;
    }
    char dist[16];
    ui_geo_distance_text(m, dist, sizeof(dist));
    snprintf(out, cap, "%s %s", dist, ui_geo_compass(ui_geo_bearing_deg(from_lat_u, from_lon_u, to_lat_u, to_lon_u)));
}

void ui_geo_age_text(uint32_t seconds, char *out, size_t cap)
{
    if (seconds < 60u) {
        snprintf(out, cap, "%u s", (unsigned)seconds);
    } else if (seconds < 3600u) {
        snprintf(out, cap, "%u min", (unsigned)(seconds / 60u));
    } else if (seconds < 172800u) {
        snprintf(out, cap, "%u h", (unsigned)(seconds / 3600u));
    } else {
        snprintf(out, cap, "%u days", (unsigned)(seconds / 86400u));
    }
}
