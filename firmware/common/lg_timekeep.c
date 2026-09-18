#include "lg_timekeep.h"

#include <sys/time.h>

#include "esp_attr.h"

/*
 * The system clock is the store. ESP-IDF keeps it on the RTC timer, which runs through a software
 * reset, a panic and a brownout, so a time written here is still right when the firmware comes
 * back: the RTC counted the seconds we were away. A power cut stops the RTC, and the clock then
 * reads 1970, which the floor below rejects.
 *
 * The marker in RTC memory says the value came from this firmware rather than from whatever was
 * in the counter. RTC memory is not cleared by a reset, and is meaningless after a power-on, so
 * the two checks together mean "we set this, and the chip has been running since".
 */
#define TIMEKEEP_MARK 0x4C47544Bu   /* "LGTK" */
#define TIME_FLOOR    1700000000u   /* 2023-11-14: earlier than any grid time this project can see */

static RTC_NOINIT_ATTR uint32_t s_mark;

void lg_timekeep_save(uint32_t unix_s)
{
    if (unix_s < TIME_FLOOR) {
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)unix_s, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_mark = TIMEKEEP_MARK;
}

uint32_t lg_timekeep_restore(void)
{
    if (s_mark != TIMEKEEP_MARK) {
        return 0;
    }
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || (uint32_t)tv.tv_sec < TIME_FLOOR) {
        return 0;
    }
    return (uint32_t)tv.tv_sec;
}
