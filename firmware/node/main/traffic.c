/* traffic.c - the packed TRAFFIC record of D70. Layout: docs/ble-link.md. */
#include "traffic.h"

#include <string.h>

#include "backbone.h"
#include "lora.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lg_envelope.h"
#include "lg_node.h"
#include "node_app.h"
#include "sessions.h"

#define HDR_LEN       24u
#define CLASS_LEN     12u
#define FAULTS        10u
#define SESS_LEN      28u
#define PERF_LEN      40u
#define BUCKET_LEN    4u
#define LINK_LEN      28u
#define LORA_LEN      LORA_SECTION_LEN
#define CPU_UNKNOWN   255u

static struct {
    SemaphoreHandle_t lock;
    uint8_t  rec[TRAFFIC_RECORD_MAX];
    size_t   rec_len;
    /* Rates: one bucket every TRAFFIC_BUCKET_MS, oldest first when the record is built. */
    uint16_t bucket_in[TRAFFIC_BUCKETS];
    uint16_t bucket_out[TRAFFIC_BUCKETS];
    size_t   bucket_head;          /* the bucket being filled */
    uint32_t bucket_started_ms;
    uint32_t last_in, last_out;    /* totals at the start of the bucket being filled */
    uint32_t loop_max_us;
    uint64_t loop_total_us;
    uint32_t loop_passes;
    TaskHandle_t core_task, link_task;
    uint16_t queue_high;
} t;

_Static_assert(HDR_LEN + LG_TC_COUNT * CLASS_LEN + FAULTS * 4u + SESS_LEN + PERF_LEN +
                   TRAFFIC_BUCKETS * BUCKET_LEN + LG_MAX_NODES * LINK_LEN + LORA_LEN <= TRAFFIC_RECORD_MAX,
               "the traffic record no longer fits its buffer");

static uint16_t sat16(uint32_t v)
{
    return (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
}

esp_err_t traffic_init(void)
{
    t.lock = xSemaphoreCreateMutex();
    return t.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void traffic_set_tasks(TaskHandle_t core, TaskHandle_t link)
{
    t.core_task = core;
    t.link_task = link;
}

void traffic_loop_pass(uint32_t us)
{
    if (us > t.loop_max_us) {
        t.loop_max_us = us;
    }
    t.loop_total_us += us;
    t.loop_passes++;
}

static uint32_t class_total(const uint32_t *a)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < LG_TC_COUNT; i++) {
        sum += a[i];
    }
    return sum;
}

/* Closes the bucket being filled when its time is up, and opens the next. */
static void roll_buckets(uint32_t now_ms, uint32_t in_total, uint32_t out_total)
{
    if (t.bucket_started_ms == 0) {
        t.bucket_started_ms = now_ms;
        t.last_in = in_total;
        t.last_out = out_total;
        return;
    }
    while (now_ms - t.bucket_started_ms >= TRAFFIC_BUCKET_MS) {
        t.bucket_in[t.bucket_head] = sat16(in_total - t.last_in);
        t.bucket_out[t.bucket_head] = sat16(out_total - t.last_out);
        t.bucket_head = (t.bucket_head + 1u) % TRAFFIC_BUCKETS;
        t.bucket_in[t.bucket_head] = 0;
        t.bucket_out[t.bucket_head] = 0;
        t.last_in = in_total;
        t.last_out = out_total;
        t.bucket_started_ms += TRAFFIC_BUCKET_MS;
    }
    /* The bucket being filled shows what it has so far, so a rate is never a whole bucket stale. */
    t.bucket_in[t.bucket_head] = sat16(in_total - t.last_in);
    t.bucket_out[t.bucket_head] = sat16(out_total - t.last_out);
}

void traffic_publish(uint32_t now_ms)
{
    if (t.lock == NULL) {
        return;
    }
    const lg_node_traffic_t *tr = lg_node_traffic(&g_app.core);
    uint32_t in_total = class_total(tr->in);
    uint32_t out_total = class_total(tr->out) + class_total(tr->relayed);
    roll_buckets(now_ms, in_total, out_total);

    lgbb_radio_traffic_t radio;
    lgbb_radio_traffic(&radio);
    lgbb_link_traffic_t links[LG_MAX_NODES];
    size_t n_links = lgbb_link_traffic(links, LG_MAX_NODES, now_ms);
    sess_traffic_t se;
    sess_traffic(&se);

    UBaseType_t waiting = g_app.cmd_queue != NULL ? uxQueueMessagesWaiting(g_app.cmd_queue) : 0;
    if (waiting > t.queue_high) {
        t.queue_high = (uint16_t)waiting;
    }

    if (xSemaphoreTake(t.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;   /* the watcher reads the previous second's record instead */
    }
    uint8_t *o = t.rec;
    memset(o, 0, sizeof(t.rec));
    o[0] = TRAFFIC_LAYOUT;
    o[1] = (uint8_t)g_app.index;
    o[2] = (uint8_t)n_links;
    o[3] = (uint8_t)LG_TC_COUNT;
    o[4] = (uint8_t)TRAFFIC_BUCKETS;
    o[5] = CPU_UNKNOWN;   /* no CPU busy meter on this build */
    lg_wr16(o + 6, (uint16_t)TRAFFIC_BUCKET_MS);
    lg_wr32(o + 8, now_ms / 1000u);
    lg_wr32(o + 12, app_grid_time());
    lg_wr32(o + 16, in_total);
    lg_wr32(o + 20, out_total);
    size_t n = HDR_LEN;

    for (size_t i = 0; i < LG_TC_COUNT; i++, n += CLASS_LEN) {
        lg_wr32(o + n, tr->in[i]);
        lg_wr32(o + n + 4, tr->out[i]);
        lg_wr32(o + n + 8, tr->relayed[i]);
    }

    const uint32_t faults[FAULTS] = {
        g_app.core.stats.duplicates,
        tr->table_full,
        tr->unknown_recipient,
        radio.rx_auth_fail,                                        /* failed to decrypt */
        tr->ttl_expired,
        radio.tx_dropped + radio.tx_nomem + radio.rx_queue_full,   /* queue full */
        se.send_timeouts,
        sess_voice_dropped(),
        g_app.core.stats.malformed,
        g_app.core.stats.rejected,
    };
    for (size_t i = 0; i < FAULTS; i++, n += 4) {
        lg_wr32(o + n, faults[i]);
    }

    o[n] = se.open_now;
    o[n + 1] = se.registered_now;
    lg_wr32(o + n + 4, se.opened);
    lg_wr32(o + n + 8, se.registrations);
    lg_wr32(o + n + 12, se.disconnects);
    lg_wr32(o + n + 16, se.bytes_in);
    lg_wr32(o + n + 20, se.bytes_out);
    lg_wr32(o + n + 24, se.slowest_send_ms);
    n += SESS_LEN;

    lg_wr32(o + n, esp_get_free_heap_size());
    lg_wr32(o + n + 4, esp_get_minimum_free_heap_size());
    lg_wr32(o + n + 8, (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    lg_wr16(o + n + 12, (uint16_t)waiting);
    lg_wr16(o + n + 14, t.queue_high);
    lg_wr32(o + n + 16, t.core_task != NULL ? uxTaskGetStackHighWaterMark(t.core_task) : 0u);
    lg_wr32(o + n + 20, t.link_task != NULL ? uxTaskGetStackHighWaterMark(t.link_task) : 0u);
    lg_wr32(o + n + 24, t.loop_max_us / 1000u);
    lg_wr32(o + n + 28, t.loop_passes > 0 ? (uint32_t)(t.loop_total_us / t.loop_passes) : 0u);
    lg_wr32(o + n + 32, radio.tx_fail);          /* ESP-NOW errors */
    lg_wr32(o + n + 36, radio.rx_replay + se.refused);   /* Wi-Fi side errors */
    n += PERF_LEN;

    for (size_t i = 0; i < TRAFFIC_BUCKETS; i++, n += BUCKET_LEN) {
        size_t k = (t.bucket_head + 1u + i) % TRAFFIC_BUCKETS;   /* oldest first, newest last */
        lg_wr16(o + n, t.bucket_in[k]);
        lg_wr16(o + n + 2, t.bucket_out[k]);
    }

    for (size_t i = 0; i < n_links; i++, n += LINK_LEN) {
        uint8_t *e = o + n;
        e[0] = (uint8_t)links[i].node;
        e[1] = links[i].up ? 1u : 0u;
        e[2] = (uint8_t)links[i].rssi;
        lg_wr32(e + 4, links[i].tx_frames);
        lg_wr32(e + 8, links[i].tx_bytes);
        lg_wr32(e + 12, links[i].rx_frames);
        lg_wr32(e + 16, links[i].rx_bytes);
        lg_wr32(e + 20, links[i].tx_fail);
        lg_wr32(e + 24, links[i].heard_age_ms);
    }

    /* D71: the second backbone, appended after the links (see traffic.h and docs/ble-link.md). */
    lora_traffic_t lr;
    lora_traffic(&lr);
    uint8_t *e = o + n;
    e[0] = lr.flags;
    e[1] = lr.address;
    e[2] = (uint8_t)lr.rssi;
    e[3] = (uint8_t)lr.snr;
    lg_wr32(e + 4, lr.frames_out);
    lg_wr32(e + 8, lr.frames_in);
    lg_wr32(e + 12, lr.parts_out);
    lg_wr32(e + 16, lr.parts_in);
    lg_wr32(e + 20, lr.parts_dropped);
    lg_wr32(e + 24, lr.reasm_timeouts);
    lg_wr32(e + 28, lr.seal_fail);
    lg_wr16(e + 32, lr.queue_depth);
    lg_wr16(e + 34, lr.queue_high);
    lg_wr32(e + 36, lr.airtime_ms);
    lg_wr32(e + 40, lr.retries);
    lg_wr32(e + 44, lr.queue_dropped);
    lg_wr32(e + 48, lr.heard_age_ms);
    e[52] = lr.peers;
    e[53] = lr.restarts;
    e[54] = lr.networkid;
    lg_wr32(e + 56, lr.frames_first);
    lg_wr32(e + 60, lr.refused_big);
    n += LORA_LEN;

    t.rec_len = n;
    xSemaphoreGive(t.lock);
}

size_t traffic_record(uint8_t *out, size_t cap, uint32_t wait_ms)
{
    if (t.lock == NULL || cap < TRAFFIC_RECORD_MAX) {
        return 0;
    }
    if (xSemaphoreTake(t.lock, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return 0;
    }
    size_t n = t.rec_len;
    memcpy(out, t.rec, n);
    xSemaphoreGive(t.lock);
    return n;
}
