/* tz_names.c - see tz_names.h. Data only: flash, no RAM. */
#include "tz_names.h"

#include <stddef.h>
#include <string.h>

/* Rules that many names share. Daylight-saving dates are today's law in each place. */
#define US_EASTERN  "EST5EDT,M3.2.0,M11.1.0"
#define US_CENTRAL  "CST6CDT,M3.2.0,M11.1.0"
#define US_MOUNTAIN "MST7MDT,M3.2.0,M11.1.0"
#define US_PACIFIC  "PST8PDT,M3.2.0,M11.1.0"
#define EU_WEST     "WET0WEST,M3.5.0/1,M10.5.0"
#define EU_CENTRAL  "CET-1CEST,M3.5.0,M10.5.0/3"
#define EU_EAST     "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define AU_EAST     "AEST-10AEDT,M10.1.0,M4.1.0/3"

static const struct {
    const char *name;
    const char *rule;
} ZONES[] = {
    { "UTC", "UTC0" },
    { "Etc/UTC", "UTC0" },
    /* North America */
    { "America/New_York", US_EASTERN },
    { "America/Detroit", US_EASTERN },
    { "America/Toronto", US_EASTERN },
    { "America/Chicago", US_CENTRAL },
    { "America/Winnipeg", US_CENTRAL },
    { "America/Denver", US_MOUNTAIN },
    { "America/Boise", US_MOUNTAIN },
    { "America/Edmonton", US_MOUNTAIN },
    { "America/Phoenix", "MST7" },
    { "America/Los_Angeles", US_PACIFIC },
    { "America/Vancouver", US_PACIFIC },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "Pacific/Honolulu", "HST10" },
    { "America/Mexico_City", "CST6" },
    /* South America */
    { "America/Bogota", "<-05>5" },
    { "America/Lima", "<-05>5" },
    { "America/Sao_Paulo", "<-03>3" },
    { "America/Argentina/Buenos_Aires", "<-03>3" },
    /* Europe */
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon", EU_WEST },
    { "Europe/Berlin", EU_CENTRAL },
    { "Europe/Paris", EU_CENTRAL },
    { "Europe/Madrid", EU_CENTRAL },
    { "Europe/Rome", EU_CENTRAL },
    { "Europe/Amsterdam", EU_CENTRAL },
    { "Europe/Brussels", EU_CENTRAL },
    { "Europe/Vienna", EU_CENTRAL },
    { "Europe/Zurich", EU_CENTRAL },
    { "Europe/Prague", EU_CENTRAL },
    { "Europe/Warsaw", EU_CENTRAL },
    { "Europe/Budapest", EU_CENTRAL },
    { "Europe/Stockholm", EU_CENTRAL },
    { "Europe/Oslo", EU_CENTRAL },
    { "Europe/Copenhagen", EU_CENTRAL },
    { "Europe/Athens", EU_EAST },
    { "Europe/Helsinki", EU_EAST },
    { "Europe/Kyiv", EU_EAST },
    { "Europe/Bucharest", EU_EAST },
    { "Europe/Sofia", EU_EAST },
    { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Moscow", "MSK-3" },
    /* Africa */
    { "Africa/Lagos", "WAT-1" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Africa/Nairobi", "EAT-3" },
    /* Asia */
    { "Asia/Dubai", "<+04>-4" },
    { "Asia/Karachi", "PKT-5" },
    { "Asia/Kolkata", "IST-5:30" },
    { "Asia/Calcutta", "IST-5:30" },
    { "Asia/Dhaka", "<+06>-6" },
    { "Asia/Bangkok", "<+07>-7" },
    { "Asia/Ho_Chi_Minh", "<+07>-7" },
    { "Asia/Jakarta", "WIB-7" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Hong_Kong", "HKT-8" },
    { "Asia/Taipei", "CST-8" },
    { "Asia/Singapore", "<+08>-8" },
    { "Asia/Manila", "PST-8" },
    { "Asia/Tokyo", "JST-9" },
    { "Asia/Seoul", "KST-9" },
    /* Australia and the Pacific */
    { "Australia/Perth", "AWST-8" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" },
    { "Australia/Sydney", AU_EAST },
    { "Australia/Melbourne", AU_EAST },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};

const char *tz_rule_for_name(const char *name)
{
    for (size_t i = 0; name != NULL && name[0] != '\0' && i < sizeof(ZONES) / sizeof(ZONES[0]); i++) {
        if (strcmp(name, ZONES[i].name) == 0) {
            return ZONES[i].rule;
        }
    }
    return NULL;
}
