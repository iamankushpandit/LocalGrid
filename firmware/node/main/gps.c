#include "gps.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "node_app.h"

static const char *TAG = "TIME";

#define GPS_UART      UART_NUM_2
#define GPS_BAUD      9600
#define GPS_RX_BUF    1024
#define GPS_LINE_MAX  96        /* NMEA allows 82 characters */
#define GPS_FRESH_MS  5000u     /* a fix older than this is lost: the module stopped reporting one */
#define TASK_STACK    3072
#define TASK_PRIORITY 3         /* below the core task (5): time can wait a few milliseconds */

static struct {
    portMUX_TYPE mux;
    gps_state_t  st;           /* fix_age_ms is filled in on read */
    uint32_t     fix_ms;       /* app_now_ms of the last fixed RMC, 0 never */
    int          rx;
    volatile uint32_t dump_bytes;   /* console `gps raw`: bytes still to print as they arrive */
    uint32_t     bytes;             /* every byte received, good or not */
} g = { .mux = portMUX_INITIALIZER_UNLOCKED, .rx = -1 };

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* "$....*HH": the XOR of everything between $ and * must equal HH. */
static bool checksum_ok(const char *line)
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

static int two(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
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
    if (len < (size_t)deg_digits + 2 || hemi[0] == '\0') {
        return false;
    }
    int deg = 0;
    for (int i = 0; i < deg_digits; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return false;
        }
        deg = deg * 10 + (v[i] - '0');
    }
    double minutes = strtod(v + deg_digits, NULL);
    if (minutes < 0.0 || minutes >= 60.0) {
        return false;
    }
    double d = deg + minutes / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') {
        d = -d;
    }
    *out = (int32_t)(d * 1e6 + (d < 0 ? -0.5 : 0.5));
    return true;
}

/* RMC: time hhmmss.sss, status A (fix) or V, lat, N/S, lon, E/W, ..., date ddmmyy. */
static void on_rmc(const char *line, uint32_t at_ms)
{
    char t[16], status[4], date[10];
    if (!field(line, 1, t, sizeof(t)) || !field(line, 2, status, sizeof(status)) ||
        !field(line, 9, date, sizeof(date)) || strlen(t) < 6 || strlen(date) != 6) {
        return;
    }
    if (status[0] != 'A') {
        return;   /* no fix: the module's own clock may be anything */
    }
    int hh = two(t), mm = two(t + 2), ss = two(t + 4);
    int ms = t[6] == '.' ? (int)(strtod(t + 6, NULL) * 1000.0 + 0.5) : 0;
    int day = two(date), mon = two(date + 2), yr = 2000 + two(date + 4);
    if (hh > 23 || mm > 59 || ss > 60 || mon < 1 || mon > 12 || day < 1 || day > 31 || ms > 999) {
        return;
    }
    uint32_t unix = (uint32_t)(days_from_civil(yr, mon, day) * 86400 + hh * 3600 + mm * 60 + ss);
    char lat[16], ns[4], lon[16], ew[4];
    int32_t lat_u = 0, lon_u = 0;
    bool pos = field(line, 3, lat, sizeof(lat)) && field(line, 4, ns, sizeof(ns)) && field(line, 5, lon, sizeof(lon)) &&
               field(line, 6, ew, sizeof(ew)) && coord(lat, ns, 2, &lat_u) && coord(lon, ew, 3, &lon_u);
    taskENTER_CRITICAL(&g.mux);
    if (pos) {
        g.st.has_pos = true;
        g.st.lat_u = lat_u;
        g.st.lon_u = lon_u;
    }
    g.st.fix = true;
    g.st.last_unix = unix;
    g.fix_ms = at_ms;
    taskEXIT_CRITICAL(&g.mux);
    node_cmd_t cmd = { .type = NODE_CMD_GPS_TIME, .value = unix, .millis = (uint16_t)ms, .at_ms = at_ms };
    (void)xQueueSend(g_app.cmd_queue, &cmd, 0);   /* a full queue skips one second; the next comes */
}

/* GGA field 7: satellites in use. */
static void on_gga(const char *line)
{
    char n[6];
    if (field(line, 7, n, sizeof(n)) && n[0] != '\0') {
        taskENTER_CRITICAL(&g.mux);
        g.st.sats = (uint8_t)atoi(n);
        taskEXIT_CRITICAL(&g.mux);
    }
}

static void on_line(const char *line, uint32_t at_ms)
{
    if (!checksum_ok(line)) {
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
    taskEXIT_CRITICAL(&g.mux);
    if (first) {
        ESP_LOGI(TAG, "[TIME] GPS heard on GPIO%d: %.6s", g.rx, line);
    }
    /* Talker IDs vary by module and constellation: GP, GN, GL, GA, BD. */
    if (strlen(line) > 6 && strncmp(line + 3, "RMC", 3) == 0) {
        on_rmc(line, at_ms);
    } else if (strlen(line) > 6 && strncmp(line + 3, "GGA", 3) == 0) {
        on_gga(line);
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    static char line[GPS_LINE_MAX];
    static uint8_t buf[128];
    size_t len = 0;
    uint32_t line_ms = 0;   /* when the line's '$' arrived: the time the sentence stands for is just before */
    for (;;) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        uint32_t now = app_now_ms();
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
        if (g.st.fix && now - g.fix_ms > GPS_FRESH_MS) {
            g.st.fix = false;
            lost = true;
        }
        taskEXIT_CRITICAL(&g.mux);
        if (lost) {
            ESP_LOGW(TAG, "[TIME] GPS fix lost; grid time carries on from this AP's clock");
        }
    }
}

esp_err_t gps_start(int rx_gpio)
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
        err = uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* No GPS fitted is normal (D63). A pulled-up idle line reads as nothing, not as noise. */
        err = gpio_set_pull_mode((gpio_num_t)rx_gpio, GPIO_PULLUP_ONLY);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TIME] GPS reader not started: %s", esp_err_to_name(err));
        return err;
    }
    g.rx = rx_gpio;
    g.st.started = true;
    if (xTaskCreate(gps_task, "gps", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[TIME] Listening for a GPS on GPIO%d at %d baud", rx_gpio, GPS_BAUD);
    return ESP_OK;
}

void gps_state(gps_state_t *out)
{
    taskENTER_CRITICAL(&g.mux);
    *out = g.st;
    uint32_t fix_ms = g.fix_ms;
    taskEXIT_CRITICAL(&g.mux);
    out->fix_age_ms = fix_ms == 0 ? UINT32_MAX : app_now_ms() - fix_ms;
}

bool gps_has_fix(void)
{
    gps_state_t st;
    gps_state(&st);
    return st.fix && st.fix_age_ms <= GPS_FRESH_MS;
}

void gps_dump(uint32_t bytes)
{
    printf("GPS: %" PRIu32 " bytes received since boot; printing the next %" PRIu32 "\n", g.bytes, bytes);
    g.dump_bytes = bytes;
}
