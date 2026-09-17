#include "lg_power.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "POWER";

#define SAMPLES_PER_READING  16u
#define MONITOR_MAX_S        3600u
#define INTERVAL_MIN_MS      50u
#define INTERVAL_DEFAULT_MS  1000u

static struct {
    bool                      ready;
    int                       gpio;
    uint32_t                  divider_milli;
    adc_oneshot_unit_handle_t unit;
    adc_channel_t             channel;
    adc_cali_handle_t         cali;      /* NULL: uncalibrated, raw scaled to 3.3 V full scale */
    void                    (*hook)(void);
} s = { .gpio = -1 };

esp_err_t lg_power_init(int gpio, uint32_t divider_milli)
{
    s.gpio = gpio;
    s.divider_milli = divider_milli;
    if (gpio < 0 || divider_milli == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &channel);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
        /* ADC2 cannot be read while Wi-Fi runs, and Wi-Fi always runs on these boards. */
        ESP_LOGW(TAG, "[POWER] GPIO%d is not an ADC1 input; supply not measurable", gpio);
        return ESP_ERR_NOT_SUPPORTED;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if ((err = adc_oneshot_new_unit(&unit_cfg, &s.unit)) != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if ((err = adc_oneshot_config_channel(s.unit, channel, &chan_cfg)) != ESP_OK) {
        return err;
    }
    s.channel = channel;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = { .unit_id = ADC_UNIT_1, .chan = channel,
                                                 .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s.cali) != ESP_OK) {
        s.cali = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                                .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s.cali) != ESP_OK) {
        s.cali = NULL;
    }
#endif
    s.ready = true;
    ESP_LOGI(TAG, "[POWER] Supply sense on GPIO%d, divider %" PRIu32 ".%03" PRIu32 ", %s", gpio,
             divider_milli / 1000u, divider_milli % 1000u, s.cali != NULL ? "calibrated" : "uncalibrated");
    return ESP_OK;
}

bool lg_power_available(void)
{
    return s.ready;
}

void lg_power_set_report_hook(void (*hook)(void))
{
    s.hook = hook;
}

esp_err_t lg_power_read_mv(uint32_t *supply_mv, uint32_t *pin_mv)
{
    if (!s.ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SAMPLES_PER_READING; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s.unit, s.channel, &raw);
        if (err != ESP_OK) {
            return err;
        }
        int mv = 0;
        if (s.cali == NULL || adc_cali_raw_to_voltage(s.cali, raw, &mv) != ESP_OK) {
            mv = raw * 3300 / 4095;
        }
        sum += (uint32_t)(mv < 0 ? 0 : mv);
    }
    uint32_t pin = sum / SAMPLES_PER_READING;
    if (pin_mv != NULL) {
        *pin_mv = pin;
    }
    *supply_mv = (uint32_t)((uint64_t)pin * s.divider_milli / 1000u);
    return ESP_OK;
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on or reset";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "crash (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "crash (task watchdog)";
    case ESP_RST_WDT:      return "crash (other watchdog)";
    case ESP_RST_BROWNOUT: return "low supply voltage (brownout)";
    case ESP_RST_USB:      return "USB reset";
    case ESP_RST_JTAG:     return "JTAG reset";
    default:               return "other";
    }
}

static void print_footer(void)
{
    printf("POWER: this boot started after: %s\n", reset_text(esp_reset_reason()));
    if (s.hook != NULL) {
        s.hook();
    }
}

static int cmd_power(int argc, char **argv)
{
    uint32_t seconds = 0;
    uint32_t interval_ms = INTERVAL_DEFAULT_MS;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            seconds = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = true;
        } else {
            printf("usage: power [-m seconds [-i interval_ms] [-q]]\n");
            return 1;
        }
    }
    seconds = seconds > MONITOR_MAX_S ? MONITOR_MAX_S : seconds;
    interval_ms = interval_ms < INTERVAL_MIN_MS ? INTERVAL_MIN_MS : interval_ms;

    if (!s.ready) {
        if (s.gpio < 0) {
            printf("POWER: not measurable on this board (no supply sense pin)\n");
        } else {
            printf("POWER: not measurable (GPIO%d could not be read as ADC1)\n", s.gpio);
        }
        if (seconds > 0) {
            printf("POWER_RESULT unavailable\n");
        }
        print_footer();
        return 0;
    }

    uint32_t supply = 0;
    uint32_t pin = 0;
    if (seconds == 0) {
        if (lg_power_read_mv(&supply, &pin) != ESP_OK) {
            printf("POWER: read failed\n");
            return 1;
        }
        printf("POWER: supply %" PRIu32 " mV (sense pin %" PRIu32 " mV x %" PRIu32 ".%03" PRIu32 ", GPIO%d)\n",
               supply, pin, s.divider_milli / 1000u, s.divider_milli % 1000u, s.gpio);
        print_footer();
        return 0;
    }

    /* Runs on the console task, which is free to wait; nothing else is held while it does. */
    uint32_t min_mv = UINT32_MAX;
    uint32_t max_mv = 0;
    uint64_t total = 0;
    uint32_t count = 0;
    int64_t start = esp_timer_get_time();
    int64_t end = start + (int64_t)seconds * 1000000;
    TickType_t wake = xTaskGetTickCount();
    printf("POWER: monitoring %" PRIu32 " s, one reading every %" PRIu32 " ms\n", seconds, interval_ms);
    while (esp_timer_get_time() < end) {
        if (lg_power_read_mv(&supply, NULL) == ESP_OK) {
            count++;
            total += supply;
            min_mv = supply < min_mv ? supply : min_mv;
            max_mv = supply > max_mv ? supply : max_mv;
            if (!quiet) {
                printf("POWER_SAMPLE %" PRId64 " ms %" PRIu32 " mV\n", (esp_timer_get_time() - start) / 1000, supply);
            }
        }
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(interval_ms));
    }
    if (count == 0) {
        printf("POWER_RESULT unavailable\n");
    } else {
        printf("POWER_RESULT samples=%" PRIu32 " min=%" PRIu32 " avg=%" PRIu32 " max=%" PRIu32 " mV over %" PRIu32 " s\n",
               count, min_mv, (uint32_t)(total / count), max_mv, seconds);
    }
    print_footer();
    return 0;
}

esp_err_t lg_power_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "power",
        .help = "power [-m seconds [-i interval_ms] [-q]]: supply voltage now, or monitored with min/avg/max",
        .func = cmd_power,
    };
    return esp_console_cmd_register(&cmd);
}
