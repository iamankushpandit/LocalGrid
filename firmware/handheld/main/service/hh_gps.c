/*
 * The handheld's GPS reader (D65). Line framing and dump as MAIN's firmware/node/main/gps.c; the
 * sentence parsing is the shared lg_nmea. Logs go under the TIME tag like MAIN's, and never carry
 * a position: this handheld's own position is printed only by `gps` on request.
 */
#include "hh_gps.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_nmea.h"
#include "sdkconfig.h"

static const char *TAG = "TIME";

/*
 * UART0 unless the console lives on it. The FNK0104B's console is USB-Serial-JTAG, so UART0 and its
 * own pins (GPIO43/44) are free; a board whose console is UART0 takes UART1 through the GPIO matrix.
 */
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART_NUM == 0
#define GPS_UART      UART_NUM_1
#else
#define GPS_UART      UART_NUM_0
#endif
#define GPS_BAUD      9600
#define GPS_RX_BUF    1024
#define TASK_STACK    3072
#define TASK_PRIORITY 2         /* below the service task (5) and the UI: a fix can wait a few ms */
#define IDLE_STEP_MS  200u      /* how often a released reader looks at the clock */
#define PENDING_NONE  0xFFFEu   /* not a plan anyone can set: LG_GPS_PLAN_ALWAYS is 0xFFFF */

static struct {
    portMUX_TYPE     mux;
    hh_gps_state_t   st;              /* fix_age_ms, talking, and a lost fix are worked out on read */
    uint32_t         heard_ms;        /* last valid sentence, 0 never */
    uint8_t          gsv_tracked;     /* the GSV set being read: satellites with a signal so far */
    uint8_t          gsv_best;
    hh_gps_notify_t  notify;
    volatile uint32_t dump_bytes;     /* console `gps raw`: bytes still to print as they arrive */
    volatile uint32_t bytes;          /* every byte received, good or not */
    /* D73. The plan belongs to the GPS task; other tasks leave a number in `pending` and it picks
     * it up, so nothing locks on the path that matters and no reading is cut in half. */
    lg_gps_plan_t     plan;
    volatile uint16_t pending;        /* a plan to adopt, or PENDING_NONE */
    volatile bool     wake;           /* someone asked for a reading now */
    bool              open;           /* the UART driver is installed */
    uint32_t          open_free;      /* free heap just after the last install */
    int               tx;             /* the board's GPS TX pin, -1 none */
} g = { .mux = portMUX_INITIALIZER_UNLOCKED, .st = { .rx_gpio = -1 }, .tx = -1 };

static uint32_t now_ms(void)
{
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ms == 0 ? 1 : ms;   /* 0 means never */
}

static void on_line(const char *line, uint32_t at_ms)
{
    lg_nmea_t n;
    lg_nmea_parse(line, &n);
    if (!n.valid) {
        taskENTER_CRITICAL(&g.mux);
        g.st.bad++;
        taskEXIT_CRITICAL(&g.mux);
        return;
    }
    bool first = false;
    bool plan_fix = false;
    taskENTER_CRITICAL(&g.mux);
    first = !g.st.heard;
    g.st.heard = true;
    g.st.sentences++;
    g.heard_ms = at_ms;
    if (n.kind == LG_NMEA_GGA && n.has_sats) {
        g.st.sats = n.sats;
    }
    if (n.kind == LG_NMEA_GGA) {
        g.st.quality = n.quality;
        g.st.hdop_c = n.hdop_c;
        g.st.has_alt = n.has_alt;
        g.st.alt_dm = n.alt_dm;
    }
    if (n.kind == LG_NMEA_GSA) {
        g.st.fix_type = n.fix_type;
        g.st.pdop_c = n.pdop_c;
    }
    if (n.kind == LG_NMEA_GSV && n.gsv_total > 0) {
        if (n.gsv_num == 1) {
            g.gsv_tracked = 0;
            g.gsv_best = 0;
        }
        for (uint8_t k = 0; k < n.gsv_n; k++) {
            if (n.snr[k] > 0) {
                g.gsv_tracked++;
                g.gsv_best = n.snr[k] > g.gsv_best ? n.snr[k] : g.gsv_best;
            }
        }
        if (n.gsv_num == n.gsv_total) {   /* the set is complete: publish it whole */
            g.st.in_view = n.in_view;
            g.st.tracked = g.gsv_tracked;
            g.st.best_snr = g.gsv_best;
        }
    }
    if (n.kind == LG_NMEA_RMC) {
        g.st.speed_cms = n.fix ? n.speed_cms : LG_NMEA_NONE_U16;
        g.st.course_cd = n.fix ? n.course_cd : LG_NMEA_NONE_U16;
    }
    if (n.kind == LG_NMEA_RMC && n.fix) {
        plan_fix = true;
        g.st.fix = true;
        g.st.last_unix = n.unix_s;
        g.st.last_millis = n.millis;
        g.st.fix_ms = at_ms;
        g.st.has_pos = n.has_pos;
        if (n.has_pos) {
            g.st.lat_u = n.lat_u;
            g.st.lon_u = n.lon_u;
        }
    }
    taskEXIT_CRITICAL(&g.mux);
    if (plan_fix) {
        lg_gps_plan_on_fix(&g.plan, at_ms);   /* D73: this reading has what it came for */
    }
    if (first) {
        ESP_LOGI(TAG, "[TIME] GPS heard on GPIO%d: %.6s", g.st.rx_gpio, line);
    }
    if (n.kind == LG_NMEA_RMC && n.fix && g.notify != NULL) {
        g.notify();
    }
}

/* ---- opening and closing the port (D73) ---- */

static esp_err_t uart_open(void)
{
    const uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, GPS_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(GPS_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(GPS_UART, g.tx >= 0 ? g.tx : UART_PIN_NO_CHANGE, g.st.rx_gpio, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* No GPS fitted is normal. A pulled-up idle line reads as nothing, not as noise. */
        err = gpio_set_pull_mode((gpio_num_t)g.st.rx_gpio, GPIO_PULLUP_ONLY);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(GPS_UART);
        return err;
    }
    g.open = true;
    g.open_free = esp_get_free_heap_size();
    return ESP_OK;
}

static void uart_close(bool had_fix)
{
    if (!g.open) {
        return;
    }
    (void)uart_driver_delete(GPS_UART);
    g.open = false;
    uint32_t after = esp_get_free_heap_size();
    uint32_t freed = after > g.open_free ? after - g.open_free : 0u;
    taskENTER_CRITICAL(&g.mux);
    g.st.freed_bytes = freed;
    g.st.fix = false;   /* nothing is being read: there is no live fix, only the last one's age */
    taskEXIT_CRITICAL(&g.mux);
    ESP_LOGI(TAG, "[TIME] GPS reading %s in %" PRIu32 " ms; port released (%" PRIu32 " bytes back), next in %" PRIu32 " s",
             had_fix ? "done" : "found no fix", g.plan.last_open_ms, freed, lg_gps_plan_next_in_s(&g.plan, now_ms()));
    if (g.notify != NULL) {
        g.notify();   /* the screens say "waiting" rather than "no fix" (D23) */
    }
}

/* A plan another task left for this one, and a request for a reading now. */
static void take_orders(uint32_t now)
{
    uint16_t want = g.pending;
    if (want != PENDING_NONE) {
        g.pending = PENDING_NONE;
        if (lg_gps_plan_set(&g.plan, want, now)) {
            uint16_t every = lg_gps_plan_seconds(want);
            if (every == 0u) {
                ESP_LOGI(TAG, "[TIME] GPS plan from the grid: always on");
            } else {
                ESP_LOGI(TAG, "[TIME] GPS plan from the grid: a reading every %u s", every);
            }
        }
    }
    if (g.wake) {
        g.wake = false;
        lg_gps_plan_wake(&g.plan, now);
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    static char line[LG_NMEA_LINE_MAX];
    static uint8_t buf[128];
    size_t len = 0;
    uint32_t line_ms = 0;   /* when the line's '$' arrived: the time the sentence stands for is just before */
    for (;;) {
        take_orders(now_ms());
        switch (lg_gps_plan_tick(&g.plan, now_ms())) {
        case LG_GPS_ACT_OPEN:
            len = 0;   /* a reading starts on a clean line, never on half of the last one's */
            if (uart_open() != ESP_OK) {
                ESP_LOGE(TAG, "[TIME] GPS port would not open; trying again at the next reading");
            }
            break;
        case LG_GPS_ACT_CLOSE:
            uart_close(g.plan.fix);
            break;
        default:
            break;
        }
        if (!g.open) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_STEP_MS));
            continue;
        }
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        uint32_t now = now_ms();
        if (n > 0) {
            g.bytes += (uint32_t)n;
        }
        for (int i = 0; i < n && g.dump_bytes > 0; i++, g.dump_bytes--) {
            char c = (char)buf[i];
            if (c == '\n' || (c >= 0x20 && c < 0x7F)) {
                putchar(c);
            } else if (c != '\r') {
                printf("<%02X>", (unsigned)(uint8_t)c);   /* garbage shows as hex: wrong baud or a bad line */
            }
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') {
                len = 0;
                line_ms = now;
            }
            if (c == '\r' || c == '\n') {
                if (len > 0) {
                    line[len] = '\0';
                    on_line(line, line_ms);
                    len = 0;
                }
                continue;
            }
            if (len < sizeof(line) - 1u) {
                line[len++] = c;
            } else {
                len = 0;   /* longer than NMEA allows: noise, not a sentence */
            }
        }
        bool lost = false;
        taskENTER_CRITICAL(&g.mux);
        if (g.st.fix && now - g.st.fix_ms > HH_GPS_FRESH_MS) {
            g.st.fix = false;
            lost = true;
        }
        taskEXIT_CRITICAL(&g.mux);
        if (lost && lg_gps_plan_always(&g.plan)) {
            /* Only worth saying while the module is meant to report all the time; on a schedule a
             * fix going quiet between readings is the point, not a fault (D73). */
            ESP_LOGW(TAG, "[TIME] GPS fix lost; this handheld's clock carries on by itself");
        }
        if (lost && g.notify != NULL) {
            g.notify();
        }
    }
}

esp_err_t hh_gps_start(int rx_gpio, int tx_gpio, uint16_t plan, hh_gps_notify_t notify)
{
    if (rx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    g.notify = notify;
    g.tx = tx_gpio;
    g.pending = PENDING_NONE;
    taskENTER_CRITICAL(&g.mux);
    g.st.rx_gpio = rx_gpio;
    taskEXIT_CRITICAL(&g.mux);
    lg_gps_plan_init(&g.plan, plan, now_ms());
    esp_err_t err = uart_open();   /* the first reading starts at boot, whatever the interval is */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TIME] GPS reader not started: %s", esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&g.mux);
    g.st.started = true;
    taskEXIT_CRITICAL(&g.mux);
    if (xTaskCreate(gps_task, "hh_gps", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        uart_close(false);
        taskENTER_CRITICAL(&g.mux);
        g.st.started = false;
        taskEXIT_CRITICAL(&g.mux);
        return ESP_ERR_NO_MEM;
    }
    uint16_t every = lg_gps_plan_seconds(plan);
    ESP_LOGI(TAG, "[TIME] Listening for a GPS on GPIO%d at %d baud, %s", rx_gpio, GPS_BAUD,
             every == 0u ? "always on" : "a reading on a schedule");
    if (every != 0u) {
        ESP_LOGI(TAG, "[TIME] GPS read every %u s; the port is released in between (D73)", every);
    }
    return ESP_OK;
}

void hh_gps_set_plan(uint16_t plan)
{
    g.pending = lg_gps_plan_canon(plan);
}

void hh_gps_wake(void)
{
    g.wake = true;
}

uint32_t hh_gps_hold_ms(void)
{
    return lg_gps_plan_hold_ms(&g.plan);
}

void hh_gps_state(hh_gps_state_t *out)
{
    taskENTER_CRITICAL(&g.mux);
    *out = g.st;
    uint32_t heard_ms = g.heard_ms;
    taskEXIT_CRITICAL(&g.mux);
    uint32_t now = now_ms();   /* after the copy, so no stamp in it is newer than now */
    out->fix_age_ms = out->fix_ms == 0 ? UINT32_MAX : now - out->fix_ms;
    out->talking = heard_ms != 0 && now - heard_ms <= HH_GPS_FRESH_MS;
    if (out->fix && out->fix_age_ms > HH_GPS_FRESH_MS) {
        out->fix = false;   /* the task has not looked yet; the reader must not see a stale fix */
    }
    /* D73: the plan is the GPS task's, read without a lock. Each field is a word written only by
     * that one task, and a reader that catches one pass old is harmless. */
    out->phase = g.plan.phase;
    out->always = lg_gps_plan_always(&g.plan);
    out->plan = g.plan.stored;
    out->interval_s = out->always ? 0u : lg_gps_plan_seconds(g.plan.stored);
    out->next_in_s = lg_gps_plan_next_in_s(&g.plan, now);
    out->readings = g.plan.readings;
    out->fixes = g.plan.fixes;
    out->last_ttf_ms = g.plan.last_ttf_ms;
}

void hh_gps_dump(uint32_t bytes)
{
    hh_gps_wake();   /* D73: between readings there are no bytes to print until one is open */
    g.dump_bytes = bytes;
}

uint32_t hh_gps_bytes(void)
{
    return g.bytes;
}
