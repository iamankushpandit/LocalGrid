#include "backbone.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lg_body.h"
#include "lg_crypto.h"
#include "lg_dedup.h"
#include "lg_envelope.h"

static const char *TAG = "BB";

#define BB_VERSION           1u
#define BB_OUTER_LEN         12u
#define BB_FRAME_MAX         (BB_OUTER_LEN + LG_FRAME_MAX + LG_AEAD_TAG_LEN)
#define BB_HELLO_MS          2000u
#define BB_HELLO_FAST_MS     200u
#define BB_HELLO_FAST_COUNT  5u
/*
 * Link loss. HELLOs are ESP-NOW broadcasts: no MAC-level ACK and no retries, and on the classic
 * ESP32 the radio is shared with BLE advertising, so a run of lost HELLOs does not mean the
 * neighbour is gone. Once a usable link has not been heard for BB_PROBE_AFTER_MS, this AP sends
 * its HELLO to that neighbour as a unicast every BB_PROBE_MS. A unicast is retried and ACKed by
 * the neighbour's radio, and the neighbour processes it as a HELLO, which also repairs the other
 * direction. The link is lost only when it has had neither a HELLO nor an ACK for
 * BB_LINK_TIMEOUT_MS, or no HELLO at all for BB_LINK_HARD_TIMEOUT_MS (an ACK proves the radio,
 * not the firmware). Links that are not yet confirmed keep the plain timeout.
 */
#define BB_LINK_TIMEOUT_MS       6000u
#define BB_LINK_HARD_TIMEOUT_MS  20000u
#define BB_PROBE_AFTER_MS        (BB_HELLO_MS + BB_HELLO_MS / 2u)
#define BB_PROBE_MS              1000u
#define BB_GAP_MS                (2u * BB_HELLO_MS)   /* a HELLO later than this counts as a gap */
#define BB_TX_QUEUE          16u
#define BB_TX_DEADMAN_MS     1000u
#define BB_RX_QUEUE          12u
#define BB_RX_PER_POLL       8u

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[BB_FRAME_MAX];
} lgbb_rx_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t len;
    uint8_t  data[BB_FRAME_MAX];
} lgbb_tx_t;

typedef struct {
    bool     in_use;
    bool     usable;
    bool     peer_added;
    uint16_t node;
    uint8_t  mac[6];
    uint32_t boot;
    uint32_t last_ms;        /* last HELLO */
    uint32_t last_ack_ms;    /* last MAC-level ACK of a unicast to this neighbour */
    uint32_t last_probe_ms;
    bool     hello_timeout_noted;
    int      rssi;
    uint32_t hellos;
} lgbb_link_t;

/* Per-neighbour counters, kept across link loss so `nodes` shows the history. */
typedef struct {
    uint32_t hellos;       /* HELLOs received (broadcast or unicast) */
    uint32_t gaps;         /* HELLOs that arrived more than BB_GAP_MS after the previous one */
    uint32_t max_gap_ms;
    uint32_t probes;       /* unicast keepalives sent */
    uint32_t acks;         /* unicasts to this neighbour ACKed at MAC level */
    uint32_t saves;        /* times an ACK kept a link up that the HELLO timeout alone would have dropped */
    uint32_t ups;
    uint32_t losses;
} lgbb_link_stats_t;

static const uint8_t BROADCAST_MAC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static struct {
    uint16_t          self;
    uint32_t          boot;
    uint32_t          frame_seq;
    uint32_t          hello_seq;
    uint8_t           key[32];
    QueueHandle_t     rxq;
    lgbb_tx_t           txq[BB_TX_QUEUE];
    size_t            tx_head;
    size_t            tx_count;
    bool              in_flight;
    uint32_t          in_flight_ms;
    volatile bool     tx_done;
    volatile bool     tx_ok;
    lgbb_link_t         links[LG_MAX_NODES];
    lgbb_link_stats_t   stats[LG_MAX_NODES];   /* indexed by node */
    uint8_t             in_flight_mac[6];
    lg_dedup_entry_t  replay_slots[LG_MAX_NODES];
    lg_dedup_t        replay;
    uint32_t          last_hello_ms;
    uint32_t          hellos_sent;
    lgbb_frame_cb_t     on_frame;
    lgbb_link_cb_t      on_link;
    lgbb_clients_cb_t   clients;
    uint32_t          tx_frames, tx_fail, tx_dropped, tx_nomem;
    uint32_t          rx_frames, rx_auth_fail, rx_replay, rx_queue_full;
} s;

/* ---- callbacks from the Wi-Fi task: copy and return ---- */

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    static lgbb_rx_t item;   /* the Wi-Fi task delivers callbacks one at a time */
    if (info == NULL || data == NULL || len <= (int)(BB_OUTER_LEN + LG_AEAD_TAG_LEN) || len > (int)BB_FRAME_MAX) {
        return;
    }
    memcpy(item.mac, info->src_addr, 6);
    item.rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
    item.len = (uint16_t)len;
    memcpy(item.data, data, (size_t)len);
    if (xQueueSend(s.rxq, &item, 0) != pdTRUE) {
        s.rx_queue_full++;
    }
}

static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    s.tx_ok = status == ESP_NOW_SEND_SUCCESS;
    s.tx_done = true;
}

/* ---- transmit ---- */

static void enqueue_sealed(const uint8_t *mac, const uint8_t *inner, size_t len)
{
    if (len == 0 || len > LG_FRAME_MAX) {
        return;
    }
    if (s.tx_count >= BB_TX_QUEUE) {
        s.tx_dropped++;
        return;
    }
    lgbb_tx_t *t = &s.txq[(s.tx_head + s.tx_count) % BB_TX_QUEUE];
    uint8_t *o = t->data;
    o[0] = BB_VERSION;
    o[1] = 0;
    lg_wr16(o + 2, s.self);
    lg_wr32(o + 4, s.boot);
    lg_wr32(o + 8, ++s.frame_seq);
    int n = lg_aead_seal(s.key, o, o, BB_OUTER_LEN, inner, len, o + BB_OUTER_LEN);
    if (n < 0) {
        ESP_LOGE(TAG, "seal failed");
        return;
    }
    memcpy(t->mac, mac, 6);
    t->len = (uint16_t)(BB_OUTER_LEN + (size_t)n);
    s.tx_count++;
}

static void note_ack(const uint8_t *mac, uint32_t now);

static void service_tx(uint32_t now)
{
    if (s.in_flight) {
        if (s.tx_done) {
            s.in_flight = false;
            if (!s.tx_ok) {
                s.tx_fail++;
            } else if (memcmp(s.in_flight_mac, BROADCAST_MAC, 6) != 0) {
                note_ack(s.in_flight_mac, now);
            }
        } else if (now - s.in_flight_ms > BB_TX_DEADMAN_MS) {
            ESP_LOGW(TAG, "send callback missing for %u ms; releasing", (unsigned)BB_TX_DEADMAN_MS);
            s.in_flight = false;
            s.tx_fail++;
        } else {
            return;
        }
    }
    if (s.tx_count == 0) {
        return;
    }
    lgbb_tx_t *t = &s.txq[s.tx_head];
    s.tx_done = false;
    esp_err_t err = esp_now_send(t->mac, t->data, t->len);
    if (err == ESP_ERR_ESPNOW_NO_MEM) {
        s.tx_nomem++;          /* back-pressure: keep the frame and retry on the next poll */
        return;
    }
    s.tx_head = (s.tx_head + 1) % BB_TX_QUEUE;
    s.tx_count--;
    if (err == ESP_OK) {
        memcpy(s.in_flight_mac, t->mac, 6);
        s.in_flight = true;
        s.in_flight_ms = now;
        s.tx_frames++;
    } else {
        s.tx_fail++;
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
    }
}

/* ---- links ---- */

static lgbb_link_t *link_find(uint16_t node)
{
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        if (s.links[i].in_use && s.links[i].node == node) {
            return &s.links[i];
        }
    }
    return NULL;
}

static lgbb_link_t *link_get(uint16_t node)
{
    lgbb_link_t *l = link_find(node);
    if (l != NULL) {
        return l;
    }
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        if (!s.links[i].in_use) {
            l = &s.links[i];
            memset(l, 0, sizeof(*l));
            l->in_use = true;
            l->node = node;
            return l;
        }
    }
    return NULL;
}

static void ensure_peer(lgbb_link_t *l)
{
    if (l->peer_added) {
        return;
    }
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, l->mac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        l->peer_added = true;
    } else {
        ESP_LOGW(TAG, "add peer node %u: %s", l->node, esp_err_to_name(err));
    }
}

static void note_ack(const uint8_t *mac, uint32_t now)
{
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        lgbb_link_t *l = &s.links[i];
        if (l->in_use && memcmp(l->mac, mac, 6) == 0) {
            l->last_ack_ms = now;
            s.stats[l->node].acks++;
            return;
        }
    }
}

bool lgbb_is_neighbor(uint16_t node)
{
    const lgbb_link_t *l = link_find(node);
    return l != NULL && l->usable;
}

uint8_t lgbb_link_count(void)
{
    uint8_t n = 0;
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        if (s.links[i].in_use && s.links[i].usable) {
            n++;
        }
    }
    return n;
}

void lgbb_send_unicast(uint16_t node, const uint8_t *frame, size_t len)
{
    lgbb_link_t *l = link_find(node);
    if (l != NULL && l->usable) {
        enqueue_sealed(l->mac, frame, len);
    }
}

void lgbb_flood(uint16_t except_node, const uint8_t *frame, size_t len)
{
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        lgbb_link_t *l = &s.links[i];
        if (l->in_use && l->usable && l->node != except_node) {
            enqueue_sealed(l->mac, frame, len);
        }
    }
}

/* Builds and queues a HELLO to dest: the broadcast address, or one neighbour as a keepalive probe. */
static bool send_hello_to(const uint8_t *dest)
{
    uint8_t body[LG_HELLO_LEN + 1 + LG_MAX_NODES * 6];
    lg_hello_t h = {
        .node = s.self,
        .role = s.self == 0 ? 1 : 0,
        .clients = s.clients != NULL ? s.clients() : 0,
    };
    size_t n = lg_hello_enc(&h, body);
    size_t count_pos = n++;
    uint8_t count = 0;
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        const lgbb_link_t *l = &s.links[i];
        if (l->in_use) {
            lg_wr16(body + n, l->node);
            lg_wr32(body + n + 2, l->boot);
            n += 6;
            count++;
        }
    }
    body[count_pos] = count;

    lg_env_t e = { 0 };
    e.type = LG_T_NODE_HELLO;
    e.scope = LG_SCOPE_SYSTEM;
    e.origin_id = lg_node_origin_id(s.self);
    e.origin_boot = s.boot;
    e.origin_seq = ++s.hello_seq;
    e.origin_node = s.self;
    uint8_t frame[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, n, frame, sizeof(frame));
    if (flen <= 0) {
        return false;
    }
    enqueue_sealed(dest, frame, (size_t)flen);
    return true;
}

static void send_hello(void)
{
    if (send_hello_to(BROADCAST_MAC)) {
        s.hellos_sent++;
    }
}

static void handle_hello(uint16_t src, const lgbb_rx_t *r, uint32_t boot, const lg_env_t *e,
                         const uint8_t *body, uint32_t now)
{
    lg_hello_t h;
    if (e->body_len < LG_HELLO_LEN + 1u || !lg_hello_dec(body, LG_HELLO_LEN, &h) || h.node != src) {
        return;
    }
    uint8_t count = body[LG_HELLO_LEN];
    if (e->body_len != LG_HELLO_LEN + 1u + count * 6u || count > LG_MAX_NODES) {
        return;
    }
    lgbb_link_t *l = link_get(src);
    if (l == NULL) {
        return;
    }
    if (l->boot != boot) {
        if (l->usable) {
            l->usable = false;
            ESP_LOGW(TAG, "[BB] Node %u rebooted; link down until confirmed", src);
            if (s.on_link != NULL) {
                s.on_link(src, false);
            }
        }
        l->boot = boot;
    }
    lgbb_link_stats_t *st = &s.stats[src];
    if (l->hellos > 0) {
        uint32_t gap = now - l->last_ms;
        if (gap > BB_GAP_MS) {
            st->gaps++;
        }
        if (gap > st->max_gap_ms) {
            st->max_gap_ms = gap;
        }
    }
    st->hellos++;
    memcpy(l->mac, r->mac, 6);
    l->last_ms = now;
    l->hellos++;
    l->rssi = l->hellos == 1 ? r->rssi : (l->rssi * 7 + r->rssi) / 8;
    ensure_peer(l);

    bool lists_me = false;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = body + LG_HELLO_LEN + 1u + i * 6u;
        if (lg_rd16(p) == s.self && lg_rd32(p + 2) == s.boot) {
            lists_me = true;
        }
    }
    if (lists_me && !l->usable) {
        l->usable = true;
        st->ups++;
        ESP_LOGI(TAG, "[BB] Link up to node %u, RSSI %d, %u clients there", src, l->rssi, h.clients);
        if (s.on_link != NULL) {
            s.on_link(src, true);
        }
    }
}

static void handle_rx(const lgbb_rx_t *r, uint32_t now)
{
    const uint8_t *o = r->data;
    if (o[0] != BB_VERSION) {
        return;
    }
    uint16_t src = lg_rd16(o + 2);
    uint32_t boot = lg_rd32(o + 4);
    uint32_t seq = lg_rd32(o + 8);
    if (src == s.self || src >= LG_MAX_NODES) {
        return;
    }
    size_t ct_len = (size_t)r->len - BB_OUTER_LEN;
    uint8_t inner[LG_FRAME_MAX];
    if (ct_len - LG_AEAD_TAG_LEN > sizeof(inner)) {
        return;
    }
    int n = lg_aead_open(s.key, o, o, BB_OUTER_LEN, o + BB_OUTER_LEN, ct_len, inner);
    if (n < 0) {
        s.rx_auth_fail++;
        return;
    }
    if (lg_dedup_mark(&s.replay, src, boot, seq) != LG_DEDUP_NEW) {
        s.rx_replay++;
        return;
    }
    s.rx_frames++;

    lg_env_t e;
    if (lg_env_decode(inner, (size_t)n, &e) != LG_OK) {
        return;
    }
    if (e.type == LG_T_NODE_HELLO) {
        handle_hello(src, r, boot, &e, lg_frame_body(inner), now);
        return;
    }
    if (!lgbb_is_neighbor(src)) {
        return;   /* data only over confirmed links */
    }
    if (s.on_frame != NULL) {
        s.on_frame(src, inner, (size_t)n);
    }
}

static void probe_and_expire_links(uint32_t now)
{
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        lgbb_link_t *l = &s.links[i];
        if (!l->in_use) {
            continue;
        }
        uint32_t hello_age = now - l->last_ms;
        bool acked = l->last_ack_ms != 0 && now - l->last_ack_ms <= BB_LINK_TIMEOUT_MS;
        if (l->usable && l->peer_added && hello_age > BB_PROBE_AFTER_MS && now - l->last_probe_ms >= BB_PROBE_MS) {
            l->last_probe_ms = now;
            if (send_hello_to(l->mac)) {
                s.stats[l->node].probes++;
            }
        }
        bool lost;
        if (l->usable) {
            lost = hello_age > BB_LINK_HARD_TIMEOUT_MS || (hello_age > BB_LINK_TIMEOUT_MS && !acked);
            if (!lost && hello_age > BB_LINK_TIMEOUT_MS && !l->hello_timeout_noted) {
                l->hello_timeout_noted = true;
                s.stats[l->node].saves++;
                ESP_LOGW(TAG, "[BB] No HELLO from node %u for %" PRIu32 " ms, but it ACKs keepalives; link kept",
                         l->node, hello_age);
            }
        } else {
            lost = hello_age > BB_LINK_TIMEOUT_MS;
        }
        if (hello_age <= BB_LINK_TIMEOUT_MS) {
            l->hello_timeout_noted = false;
        }
        if (!lost) {
            continue;
        }
        bool was_usable = l->usable;
        uint16_t node = l->node;
        if (l->peer_added) {
            (void)esp_now_del_peer(l->mac);
        }
        memset(l, 0, sizeof(*l));
        if (was_usable) {
            s.stats[node].losses++;
            ESP_LOGW(TAG, "[BB] Link lost to node %u (no HELLO for %" PRIu32 " ms, %s)", node, hello_age,
                     acked ? "keepalives still ACKed" : "no keepalive ACK either");
        } else {
            ESP_LOGW(TAG, "[BB] One-way link to node %u expired (no HELLO for %" PRIu32 " ms)", node, hello_age);
        }
        if (was_usable && s.on_link != NULL) {
            s.on_link(node, false);
        }
    }
}

void lgbb_poll(uint32_t now)
{
    static lgbb_rx_t r;
    for (unsigned i = 0; i < BB_RX_PER_POLL && xQueueReceive(s.rxq, &r, 0) == pdTRUE; i++) {
        handle_rx(&r, now);
    }
    uint32_t interval = s.hellos_sent < BB_HELLO_FAST_COUNT ? BB_HELLO_FAST_MS : BB_HELLO_MS;
    if (now - s.last_hello_ms >= interval) {
        s.last_hello_ms = now;
        send_hello();
    }
    probe_and_expire_links(now);
    service_tx(now);
}

esp_err_t lgbb_init(uint16_t self, uint32_t boot, const uint8_t key[32],
                  lgbb_frame_cb_t on_frame, lgbb_link_cb_t on_link, lgbb_clients_cb_t clients)
{
    memset(&s, 0, sizeof(s));
    s.self = self;
    s.boot = boot;
    memcpy(s.key, key, 32);
    s.on_frame = on_frame;
    s.on_link = on_link;
    s.clients = clients;
    lg_dedup_init(&s.replay, s.replay_slots, LG_MAX_NODES);

    s.rxq = xQueueCreate(BB_RX_QUEUE, sizeof(lgbb_rx_t));
    if (s.rxq == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_AP;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t version = 0;
    if (esp_now_get_version(&version) == ESP_OK) {
        ESP_LOGI(TAG, "[BB] ESP-NOW v%" PRIu32 " on SoftAP interface, node %u boot %" PRIu32, version, self, boot);
    }
    return ESP_OK;
}

size_t lgbb_links(lgbb_link_info_t *out, size_t max, uint32_t now_ms)
{
    size_t n = 0;
    for (size_t i = 0; i < LG_MAX_NODES && n < max; i++) {
        const lgbb_link_t *l = &s.links[i];
        if (l->in_use) {
            out[n].node = l->node;
            out[n].up = l->usable;
            out[n].rssi = l->rssi;
            out[n].age_ms = now_ms - l->last_ms;
            n++;
        }
    }
    return n;
}

void lgbb_print(void)
{
    printf("Backbone links (AP %u):\n", s.self);
    printf("  AP    STATE     RSSI  AGE_MS  HELLOS  GAPS  MAX_GAP  PROBES  ACKS  SAVES  UPS  LOSSES\n");
    /* no hardware addresses in output (decision D21); counters survive link loss */
    uint32_t now = (uint32_t)(esp_log_timestamp());
    for (uint16_t node = 0; node < LG_MAX_NODES; node++) {
        const lgbb_link_t *l = link_find(node);
        const lgbb_link_stats_t *st = &s.stats[node];
        if (l == NULL && st->hellos == 0) {
            continue;
        }
        if (l != NULL) {
            printf("  %-4u  %-8s  %4d  %6" PRIu32, node, l->usable ? "UP" : "ONE-WAY", l->rssi, now - l->last_ms);
        } else {
            printf("  %-4u  %-8s  %4s  %6s", node, "DOWN", "--", "--");
        }
        printf("  %6" PRIu32 "  %4" PRIu32 "  %7" PRIu32 "  %6" PRIu32 "  %4" PRIu32 "  %5" PRIu32 "  %3" PRIu32
               "  %6" PRIu32 "\n", st->hellos, st->gaps, st->max_gap_ms, st->probes, st->acks, st->saves, st->ups,
               st->losses);
    }
    printf("  gap = HELLO more than %u ms after the previous; probe = unicast keepalive; save = link kept by ACKs\n",
           (unsigned)BB_GAP_MS);
    printf("  tx %" PRIu32 " tx_fail %" PRIu32 " dropped %" PRIu32 " nomem %" PRIu32
           " | rx %" PRIu32 " auth_fail %" PRIu32 " replay %" PRIu32 " rx_queue_full %" PRIu32 " | queued %u\n",
           s.tx_frames, s.tx_fail, s.tx_dropped, s.tx_nomem,
           s.rx_frames, s.rx_auth_fail, s.rx_replay, s.rx_queue_full, (unsigned)s.tx_count);
}
