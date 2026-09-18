/*
 * The battery percentage behind the top-bar badge (D62).
 *
 * The supply is read through lg_power (the board's sense pin behind its 2:1 divider) every 2 s on
 * a priority-1 task of its own, never on the UI task: the filter and the deadband have memory, and
 * they must advance on a clock rather than on however often a screen happens to ask. Readers get
 * a published byte and nothing else.
 *
 * The numbers are Braino's (see THIRD_PARTY.md), restated:
 * - a single-cell LiPo discharge curve, 4.20 V = 100 % down to 3.20 V = 0 %, interpolated
 *   between its points;
 * - a low-pass on the voltage with a time constant of about 40 s at 2 s sampling (alpha 1/20), so
 *   radio bursts and backlight steps do not move it, primed with the first reading so the badge
 *   does not ramp up from zero after every boot;
 * - a 2-point deadband on the displayed percentage, because on the curve's plateau one ADC count
 *   is most of a percent and a value resting on a boundary would otherwise flap for ever;
 * - a filtered reading below 3.0 V or above 4.5 V is a sensor fault, shown as no badge (-1)
 *   at once rather than held back.
 *
 * Without a battery fitted the sense pin reads the charger's output, about 4.1 to 4.2 V, so the
 * badge says nearly full. Nothing here can tell a missing cell from a full one, and that reading
 * is the honest one.
 */
#include "hh_battery.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hh_service.h"
#include "lg_power.h"

static const char *TAG = "POWER";

#define SAMPLE_MS        2000u
#define TASK_STACK       3072
#define TASK_PRIORITY    1
#define FILTER_DIV       20      /* alpha 1/20: about 40 s at one sample every 2 s */
#define DEADBAND_PCT     2
#define FAULT_LOW_MV     3000u
#define FAULT_HIGH_MV    4500u

typedef struct {
    uint16_t mv;
    uint8_t  pct;
} curve_point_t;

/* Highest first. */
static const curve_point_t CURVE[] = {
    { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3930, 70 }, { 3870, 60 }, { 3820, 50 },
    { 3790, 40 },  { 3770, 30 }, { 3740, 20 }, { 3680, 10 }, { 3550, 5 },  { 3200, 0 },
};

static struct {
    bool              started;
    bool              primed;
    uint32_t          filtered_mv_x16;   /* the low-pass, in 1/16 mV so small steps are not lost */
    int8_t            shown;             /* what the deadband let through; touched by the sampler only */
    volatile int8_t   published;         /* one byte, so a reader always sees a whole value */
    volatile uint32_t published_mv;
} s = { .shown = -1, .published = -1 };

static int8_t curve_percent(uint32_t mv)
{
    const size_t n = sizeof(CURVE) / sizeof(CURVE[0]);
    if (mv >= CURVE[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        const curve_point_t *hi = &CURVE[i - 1u];
        const curve_point_t *lo = &CURVE[i];
        if (mv >= lo->mv) {
            uint32_t span = (uint32_t)(hi->mv - lo->mv);
            uint32_t rise = (uint32_t)(hi->pct - lo->pct);
            return (int8_t)(lo->pct + ((mv - lo->mv) * rise + span / 2u) / span);
        }
    }
    return 0;
}

static void sample(void)
{
    uint32_t mv = 0;
    if (lg_power_read_mv(&mv, NULL) != ESP_OK) {
        return;   /* no reading this time: keep what was shown */
    }
    if (!s.primed) {
        s.primed = true;
        s.filtered_mv_x16 = mv * 16u;
    } else {
        int32_t diff = (int32_t)(mv * 16u) - (int32_t)s.filtered_mv_x16;
        s.filtered_mv_x16 = (uint32_t)((int32_t)s.filtered_mv_x16 + diff / FILTER_DIV);
    }
    uint32_t filtered = s.filtered_mv_x16 / 16u;
    int8_t before = s.shown;
    if (filtered < FAULT_LOW_MV || filtered > FAULT_HIGH_MV) {
        s.shown = -1;
    } else {
        int8_t raw = curve_percent(filtered);
        int drift = raw > s.shown ? raw - s.shown : s.shown - raw;
        if (s.shown < 0 || raw >= 100 || raw <= 0 || drift >= DEADBAND_PCT) {
            s.shown = raw;
        }
    }
    s.published_mv = filtered;
    s.published = s.shown;
    if (s.shown != before) {
        if (s.shown < 0) {
            ESP_LOGW(TAG, "[POWER] Battery reading %" PRIu32 " mV is outside 3.0 to 4.5 V: no badge", filtered);
        } else {
            ESP_LOGI(TAG, "[POWER] Battery %d%% (%" PRIu32 " mV filtered)", s.shown, filtered);
        }
    }
}

static void battery_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        sample();
    }
}

void hh_battery_start(void)
{
    if (s.started) {
        return;
    }
    s.started = true;
    if (!lg_power_available()) {
        ESP_LOGI(TAG, "[POWER] No supply sense on this board: no battery badge");
        return;
    }
    sample();   /* one now, so the first screen has a real value */
    if (xTaskCreate(battery_task, "hh_batt", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "[POWER] Battery task did not start: the badge keeps its first reading");
    }
}

uint32_t hh_battery_mv(void)
{
    return s.published_mv;
}

int8_t hh_service_battery_percent(void)
{
    return s.published;
}
