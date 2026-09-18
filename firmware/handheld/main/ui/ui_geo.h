/*
 * ui_geo.h - distance, direction, and the words for them, between two GPS positions in
 * microdegrees (D65). Portable C: only the C standard library, no ESP-IDF, no drawing.
 *
 * The screens use it to say where someone is: "240 m NE of you", "1.2 km SW of MAIN", or the
 * coordinates "38.86593, -94.68011" when there is nothing to measure from.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Great-circle distance in metres (haversine on a 6371 km sphere). */
uint32_t ui_geo_distance_m(int32_t lat1_u, int32_t lon1_u, int32_t lat2_u, int32_t lon2_u);

/* Initial bearing from point 1 to point 2 in whole degrees, 0 = north, clockwise, 0..359. */
uint16_t ui_geo_bearing_deg(int32_t lat1_u, int32_t lon1_u, int32_t lat2_u, int32_t lon2_u);

/* The nearest of eight compass points for a bearing: "N", "NE", "E", "SE", "S", "SW", "W", "NW". */
const char *ui_geo_compass(uint16_t deg);

/* "35 m", "240 m", "1.2 km", "14 km". */
void ui_geo_distance_text(uint32_t metres, char *out, size_t cap);

/* "38.86593, -94.68011": five decimals, about a metre. */
void ui_geo_coord_text(int32_t lat_u, int32_t lon_u, char *out, size_t cap);

/* Where point 2 is from point 1: "240 m NE", or "within 10 m" when a direction would be GPS noise. */
void ui_geo_where_text(int32_t from_lat_u, int32_t from_lon_u, int32_t to_lat_u, int32_t to_lon_u, char *out,
                       size_t cap);

/* An age in words: "45 s", "5 min", "3 h", "2 days". */
void ui_geo_age_text(uint32_t seconds, char *out, size_t cap);
