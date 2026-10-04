#include "power_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"
#include "sdkconfig.h"

static const char *TAG = "UP";

/* lwIP's connection lists belong to its thread, so they are walked there and only the totals kept. */
static volatile unsigned s_tcp_open, s_tcp_lingering, s_tcp_queued;

static void count_tcp(void *arg)
{
    (void)arg;
    unsigned active = 0, lingering = 0, queued = 0;
    for (const struct tcp_pcb *q = tcp_active_pcbs; q != NULL; q = q->next) {
        active++;
        queued += q->snd_queuelen;
    }
    for (const struct tcp_pcb *q = tcp_tw_pcbs; q != NULL; q = q->next) {
        lingering++;
    }
    s_tcp_open = active;
    s_tcp_lingering = lingering;
    s_tcp_queued = queued;
}

#define TRACE_SECONDS  16u
#define TRACE_MARK     0x4C475054u   /* "LGPT" */

typedef struct {
    uint32_t uptime_s;
    uint16_t heap_kb;
    uint16_t min_heap_kb;
    uint16_t events;
    uint8_t  stations;
    uint8_t  links;
    uint8_t  tx;          /* backbone frames sent in this second, saturating */
    uint8_t  reserved[3];
} snapshot_t;

/* Kept across every reset except a real power loss. mark and a length check guard the rest. */
static RTC_NOINIT_ATTR uint32_t   s_mark;
static RTC_NOINIT_ATTR uint32_t   s_next;       /* slot the next snapshot goes into */
static RTC_NOINIT_ATTR uint32_t   s_count;      /* snapshots recorded, capped at TRACE_SECONDS */
static RTC_NOINIT_ATTR snapshot_t s_ring[TRACE_SECONDS];

static volatile uint16_t s_pending_events;
static uint32_t          s_prev_run_s = UINT32_MAX;   /* uptime at the last snapshot before this boot */
static uint32_t          s_last_tx_total;
static bool              s_have_tx;

static const struct {
    uint16_t    bit;
    const char *name;
} EVENT_NAMES[] = {
    { PTRACE_STA_JOIN, "sta_join" },   { PTRACE_STA_LEAVE, "sta_leave" }, { PTRACE_SESSION, "session" },
    { PTRACE_LINK_UP, "link_up" },     { PTRACE_LINK_DOWN, "link_down" }, { PTRACE_NVS_WRITE, "nvs_write" },
    { PTRACE_WEB, "web" },             { PTRACE_BLE_UPDATE, "ble_update" }, { PTRACE_GRID_ANNOUNCE, "grid_state" },
    { PTRACE_TIME, "time" },           { PTRACE_SCAN_BURST, "tx_burst" },
};

static void event_names(uint16_t events, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]); i++) {
        if ((events & EVENT_NAMES[i].bit) != 0 && n < cap) {
            n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? "," : "", EVENT_NAMES[i].name);
        }
    }
    if (n == 0) {
        snprintf(out, cap, "-");
    }
}

void ptrace_boot(const char *reset_cause)
{
    if (s_mark == TRACE_MARK && s_count > 0 && s_count <= TRACE_SECONDS && s_next < TRACE_SECONDS) {
        ESP_LOGW("PWR", "[PWR] The last %" PRIu32 " seconds before this restart (%s), oldest first:", s_count,
                 reset_cause);
        s_prev_run_s = s_ring[(s_next + TRACE_SECONDS - 1u) % TRACE_SECONDS].uptime_s;
        for (uint32_t k = 0; k < s_count; k++) {
            const snapshot_t *p = &s_ring[(s_next + TRACE_SECONDS - s_count + k) % TRACE_SECONDS];
            char ev[96];
            event_names(p->events, ev, sizeof(ev));
            ESP_LOGW("PWR", "[PWR]   %" PRIu32 " s heap %u KB min %u KB sta %u links %u tx %u ev %s", p->uptime_s,
                     p->heap_kb, p->min_heap_kb, p->stations, p->links, p->tx, ev);
        }
    } else {
        ESP_LOGI("PWR", "[PWR] No trace from before this restart (%s): first boot, or power was lost", reset_cause);
    }
    s_mark = TRACE_MARK;
    s_next = 0;
    s_count = 0;
}

uint32_t ptrace_prev_run_s(void)
{
    return s_prev_run_s;
}

void ptrace_event(ptrace_event_t ev)
{
    s_pending_events |= (uint16_t)ev;
}

void ptrace_second(uint32_t uptime_s, uint32_t backbone_tx_total, uint8_t links, const char *time_quality)
{
    uint32_t tx = s_have_tx ? backbone_tx_total - s_last_tx_total : 0;
    s_last_tx_total = backbone_tx_total;
    s_have_tx = true;
    if (tx > 10u) {
        s_pending_events |= PTRACE_SCAN_BURST;
    }

    wifi_sta_list_t stas;
    uint8_t stations = esp_wifi_ap_get_sta_list(&stas) == ESP_OK ? (uint8_t)stas.num : 0u;

    uint16_t events = s_pending_events;
    s_pending_events = 0;

    snapshot_t *p = &s_ring[s_next];
    memset(p, 0, sizeof(*p));
    p->uptime_s = uptime_s;
    p->heap_kb = (uint16_t)(esp_get_free_heap_size() / 1024u);
    p->min_heap_kb = (uint16_t)(esp_get_minimum_free_heap_size() / 1024u);
    p->events = events;
    p->stations = stations;
    p->links = links;
    p->tx = (uint8_t)(tx > 255u ? 255u : tx);
    s_next = (s_next + 1u) % TRACE_SECONDS;
    if (s_count < TRACE_SECONDS) {
        s_count++;
    }

#if CONFIG_LG_NODE_HEARTBEAT
    char ev[96];
    event_names(events, ev, sizeof(ev));
    ESP_LOGI(TAG, "[UP] %" PRIu32 " s heap %u KB min %u KB sta %u links %u tx %u time %s ev %s", uptime_s,
             p->heap_kb, p->min_heap_kb, stations, links, p->tx, time_quality, ev);
    /*
     * Every tenth second, where the heap might be: on 2026-10-04 an access point's resting heap fell by
     * 5 to 8 KB with nobody joining or leaving, stayed there for up to 45 minutes, and came back, and
     * nothing in the log said why. TCP connections still open or lingering, and what they have queued,
     * are the first suspects, so they are counted here; the largest free block shows fragmentation.
     */
    if (uptime_s % 10u == 0u) {
        ESP_LOGI(TAG, "[MEM] tcp open %u lingering %u queued %u, largest block %u KB", s_tcp_open, s_tcp_lingering,
                 s_tcp_queued, (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024u));
        (void)tcpip_callback(count_tcp, NULL);   /* counted on lwIP's own thread; printed next time */
    }
#else
    (void)time_quality;
#endif
}
