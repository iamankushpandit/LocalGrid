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

static struct {
    portMUX_TYPE     mux;
    hh_gps_state_t   st;              /* fix_age_ms, talking, and a lost fix are worked out on read */
    uint32_t         heard_ms;        /* last valid sentence, 0 never */
    uint8_t          gsv_tracked;     /* the GSV set being read: satellites with a signal so far */
    uint8_t          gsv_best;
    hh_gps_notify_t  notify;
    volatile uint32_t dump_bytes;     /* console `gps raw`: bytes still to print as they arrive */
    volatile uint32_t bytes;          /* every byte received, good or not */
} g = { .mux = portMUX_INITIALIZER_UNLOCKED, .st = { .rx_gpio = -1 } };

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
    if (first) {
        ESP_LOGI(TAG, "[TIME] GPS heard on GPIO%d: %.6s", g.st.rx_gpio, line);
    }
    if (n.kind == LG_NMEA_RMC && n.fix && g.notify != NULL) {
        g.notify();
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
        if (lost) {
            ESP_LOGW(TAG, "[TIME] GPS fix lost; this handheld's clock carries on by itself");
            if (g.notify != NULL) {
                g.notify();
            }
        }
    }
}

esp_err_t hh_gps_start(int rx_gpio, int tx_gpio, hh_gps_notify_t notify)
{
    if (rx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
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
        err = uart_set_pin(GPS_UART, tx_gpio >= 0 ? tx_gpio : UART_PIN_NO_CHANGE, rx_gpio, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* No GPS fitted is normal. A pulled-up idle line reads as nothing, not as noise. */
        err = gpio_set_pull_mode((gpio_num_t)rx_gpio, GPIO_PULLUP_ONLY);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TIME] GPS reader not started: %s", esp_err_to_name(err));
        return err;
    }
    g.notify = notify;
    taskENTER_CRITICAL(&g.mux);
    g.st.rx_gpio = rx_gpio;
    g.st.started = true;
    taskEXIT_CRITICAL(&g.mux);
    if (xTaskCreate(gps_task, "hh_gps", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[TIME] Listening for a GPS on GPIO%d at %d baud", rx_gpio, GPS_BAUD);
    return ESP_OK;
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
}

void hh_gps_dump(uint32_t bytes)
{
    g.dump_bytes = bytes;
}

uint32_t hh_gps_bytes(void)
{
    return g.bytes;
}
