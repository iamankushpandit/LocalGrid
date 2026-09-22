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
#define BB_OUTER_LEN         LGBB_OUTER_LEN
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
/*
 * Every slot here is BB_FRAME_MAX (1052) bytes, while the largest frame the grid actually sends is
 * about 288: a text body of 240 plus its tag, or a 256-byte grid-state record, inside a 32-byte
 * envelope. Sixteen transmit and twelve receive slots was therefore 30 KB of mostly empty buffer on
 * a board that measured 1 to 6 KB of free heap on 2026-09-21, one allocation from failing. The
 * depths below were cut to 6 each when a slot still cost a whole frame. A slot is now BB_SLOT_MAX
 * (512 B), so depth is cheap again, and 6 proved too tight in practice: the bench counted
 * queue_full 174 on a busy AP, every one of them a dropped frame. 12 and 10 cost about 11.3 KB,
 * which is still far less than the original depths did, and leave room for the burst that arrives
 * when several handhelds rejoin at once - exactly when an AP can least afford to drop anything.
 */
#define BB_TX_QUEUE          12u
#define BB_TX_DEADMAN_MS     1000u
#define BB_RX_QUEUE          10u
#define BB_RX_PER_POLL       8u

/*
 * A queue slot holds what the grid actually sends, not what the protocol's ceiling allows.
 * BB_FRAME_MAX is 1052 (LG_FRAME_MAX 1024 plus the outer header and tag) and is still what a
 * frame is checked against, but the largest frame anything here builds is a 100 ms voice frame:
 * 32 bytes of envelope, LG_VOICE_DATA_MAX of sound, the 12-byte outer header and a 16-byte tag,
 * which is 460. Sizing twelve slots for 1052 cost 7 KB of buffer that could never be filled, on
 * an AP measured at 1 to 6 KB of free heap (2026-09-21). Anything larger than a slot is refused
 * and counted rather than truncated; the static assert below keeps the two in step.
 */
#define BB_SLOT_MAX          512u
_Static_assert(BB_SLOT_MAX >= LG_ENV_SIZE + LG_VOICE_DATA_MAX + BB_OUTER_LEN + LG_AEAD_TAG_LEN,
               "a queue slot must hold the largest frame this AP sends: a voice frame");
_Static_assert(BB_SLOT_MAX <= BB_FRAME_MAX, "a slot cannot be larger than a frame may be");

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[BB_SLOT_MAX];
} lgbb_rx_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t len;
    uint8_t  data[BB_SLOT_MAX];
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
    /* D70: frames and bytes both ways, and send failures, per neighbour. One add on the hot path. */
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t tx_fail;
    uint32_t heard_ms;     /* app clock when the last frame from it was accepted; 0 never */
    bool     heard;
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
    lgbb_extra_peers_cb_t extra_peers;
    bool              off;            /* chaos hook: pretend ESP-NOW is not there */
    uint32_t          off_until_ms;
    uint32_t          tx_frames, tx_fail, tx_dropped, tx_nomem;
    uint32_t          rx_frames, rx_auth_fail, rx_replay, rx_queue_full;
} s;

/* ---- callbacks from the Wi-Fi task: copy and return ---- */

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    static lgbb_rx_t item;   /* the Wi-Fi task delivers callbacks one at a time */
    if (info == NULL || data == NULL || len <= (int)(BB_OUTER_LEN + LG_AEAD_TAG_LEN) || len > (int)BB_SLOT_MAX) {
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

/*
 * One sealed frame. The outer 12 bytes are both the AEAD nonce and its associated data, and they
 * carry a sequence number taken from a single counter: every sealed frame this AP produces gets
 * the next value whichever radio is about to carry it (D71), so no nonce is ever used twice.
 */
int lgbb_seal(uint8_t *out, size_t cap, const uint8_t *inner, size_t len)
{
    if (out == NULL || inner == NULL || len == 0 || len > LG_FRAME_MAX ||
        cap < BB_OUTER_LEN + len + LG_AEAD_TAG_LEN) {
        return -1;
    }
    out[0] = BB_VERSION;
    out[1] = 0;
    lg_wr16(out + 2, s.self);
    lg_wr32(out + 4, s.boot);
    lg_wr32(out + 8, ++s.frame_seq);
    int n = lg_aead_seal(s.key, out, out, BB_OUTER_LEN, inner, len, out + BB_OUTER_LEN);
    if (n < 0) {
        return -1;
    }
    return (int)(BB_OUTER_LEN + (size_t)n);
}

lgbb_open_t lgbb_open(const uint8_t *buf, size_t len, uint16_t *src, uint32_t *boot, uint8_t *out,
                      size_t cap, size_t *out_len)
{
    if (buf == NULL || len <= BB_OUTER_LEN + LG_AEAD_TAG_LEN || len > BB_FRAME_MAX || buf[0] != BB_VERSION) {
        return LGBB_OPEN_MALFORMED;
    }
    uint16_t from = lg_rd16(buf + 2);
    uint32_t from_boot = lg_rd32(buf + 4);
    uint32_t seq = lg_rd32(buf + 8);
    if (from == s.self || from >= LG_MAX_NODES) {
        return LGBB_OPEN_MALFORMED;
    }
    size_t ct_len = len - BB_OUTER_LEN;
    if (ct_len - LG_AEAD_TAG_LEN > cap) {
        return LGBB_OPEN_MALFORMED;
    }
    int n = lg_aead_open(s.key, buf, buf, BB_OUTER_LEN, buf + BB_OUTER_LEN, ct_len, out);
    if (n < 0) {
        return LGBB_OPEN_AUTH_FAIL;
    }
    if (lg_dedup_mark(&s.replay, from, from_boot, seq) != LG_DEDUP_NEW) {
        return LGBB_OPEN_REPLAY;
    }
    *src = from;
    *boot = from_boot;
    *out_len = (size_t)n;
    return LGBB_OPEN_OK;
}

void lgbb_disable(uint32_t seconds)
{
    uint32_t sec = seconds == 0 ? 1u : seconds;
    s.off_until_ms = (uint32_t)esp_log_timestamp() + sec * 1000u;
    s.off = true;
    ESP_LOGW(TAG, "[BB] ESP-NOW off for %" PRIu32 " s (chaos hook); anything that still crosses went by LoRa",
             sec);
}

void lgbb_enable(void)
{
    if (s.off) {
        s.off = false;
        ESP_LOGW(TAG, "[BB] ESP-NOW back on");
    }
}

bool lgbb_is_off(void)
{
    return s.off;
}

static void enqueue_sealed(const uint8_t *mac, const uint8_t *inner, size_t len)
{
    if (len == 0 || len > LG_FRAME_MAX || s.off) {
        return;
    }
    if (s.tx_count >= BB_TX_QUEUE) {
        s.tx_dropped++;
        return;
    }
    lgbb_tx_t *t = &s.txq[(s.tx_head + s.tx_count) % BB_TX_QUEUE];
    int n = lgbb_seal(t->data, sizeof(t->data), inner, len);
    if (n < 0) {
        ESP_LOGE(TAG, "seal failed");
        return;
    }
    memcpy(t->mac, mac, 6);
    t->len = (uint16_t)n;
    s.tx_count++;
    for (size_t i = 0; i < LG_MAX_NODES; i++) {   /* D70: whose link this frame is for */
        const lgbb_link_t *l = &s.links[i];
        if (l->in_use && memcmp(l->mac, mac, 6) == 0) {
            s.stats[l->node].tx_frames++;
            s.stats[l->node].tx_bytes += t->len;
            break;
        }
    }
}

static void note_ack(const uint8_t *mac, uint32_t now);

static void service_tx(uint32_t now)
{
    if (s.in_flight) {
        if (s.tx_done) {
            s.in_flight = false;
            if (!s.tx_ok) {
                s.tx_fail++;
                for (size_t i = 0; i < LG_MAX_NODES; i++) {
                    const lgbb_link_t *l = &s.links[i];
                    if (l->in_use && memcmp(l->mac, s.in_flight_mac, 6) == 0) {
                        s.stats[l->node].tx_fail++;
                        break;
                    }
                }
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

bool lgbb_link_acked(uint16_t node, uint32_t now_ms)
{
    const lgbb_link_t *l = link_find(node);
    if (l == NULL || !l->usable) {
        return false;
    }
    bool acked = l->last_ack_ms != 0 && now_ms - l->last_ack_ms <= BB_LINK_TIMEOUT_MS;
    bool heard = l->last_ms != 0 && now_ms - l->last_ms <= BB_LINK_TIMEOUT_MS;
    return acked || heard;
}

void lgbb_set_extra_peers(lgbb_extra_peers_cb_t cb)
{
    s.extra_peers = cb;
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

/*
 * Builds this AP's HELLO frame: who it is, how many handhelds it carries, and every neighbour it
 * has with that neighbour's boot counter, so the neighbour that finds itself listed knows the
 * link works both ways. Peers reached over another transport (D71) are listed too, from the
 * callback, which is how a LoRa-only link is ever confirmed.
 */
size_t lgbb_build_hello(uint8_t *out, size_t cap)
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
    uint16_t listed[LG_MAX_NODES];
    for (size_t i = 0; i < LG_MAX_NODES; i++) {
        const lgbb_link_t *l = &s.links[i];
        if (l->in_use) {
            lg_wr16(body + n, l->node);
            lg_wr32(body + n + 2, l->boot);
            n += 6;
            listed[count++] = l->node;
        }
    }
    if (s.extra_peers != NULL && count < LG_MAX_NODES) {
        uint16_t nodes[LG_MAX_NODES];
        uint32_t boots[LG_MAX_NODES];
        size_t extra = s.extra_peers(nodes, boots, LG_MAX_NODES);
        for (size_t i = 0; i < extra && count < LG_MAX_NODES; i++) {
            bool already = false;
            for (uint8_t k = 0; k < count; k++) {
                already = already || listed[k] == nodes[i];
            }
            if (already) {
                continue;
            }
            lg_wr16(body + n, nodes[i]);
            lg_wr32(body + n + 2, boots[i]);
            n += 6;
            listed[count++] = nodes[i];
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
    int flen = lg_frame_build(&e, body, n, out, cap);
    return flen > 0 ? (size_t)flen : 0u;
}

/* Builds and queues a HELLO to dest: the broadcast address, or one neighbour as a keepalive probe. */
static bool send_hello_to(const uint8_t *dest)
{
    uint8_t frame[LG_FRAME_MAX];
    size_t flen = lgbb_build_hello(frame, sizeof(frame));
    if (flen == 0) {
        return false;
    }
    enqueue_sealed(dest, frame, flen);
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
    if (o[0] != BB_VERSION || s.off) {
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
    s.stats[src].rx_frames++;
    s.stats[src].rx_bytes += r->len;
    s.stats[src].heard_ms = now;
    s.stats[src].heard = true;

    lg_env_t e;
    if (lg_env_decode(inner, (size_t)n, &e) != LG_OK) {
        return;
    }
    if (e.type == LG_T_NODE_HELLO) {
        handle_hello(src, r, boot, &e, lg_frame_body(inner), now);
        return;
    }
    if (!lgbb_is_neighbor(src)) {
        /*
         * A peer sends data only over a link it has confirmed, which it does once our HELLO lists its
         * current boot, so we have heard it. An authenticated data frame from the boot we already know
         * therefore proves the link works both ways: take it as our confirmation too. Dropping it lost
         * the presence flood a peer sends the moment its side comes up, which the chaos run of
         * 2026-09-18 traced every 1:1 refusal to: the restarted AP never learned where those handhelds
         * were. A frame from a boot we have not heard stays dropped.
         */
        lgbb_link_t *l = link_find(src);
        if (l == NULL || l->boot != boot || l->hellos == 0) {
            return;   /* data only over confirmed links */
        }
        l->usable = true;
        s.stats[src].ups++;
        ESP_LOGI(TAG, "[BB] Link up to node %u (confirmed by its data), RSSI %d", src, l->rssi);
        if (s.on_link != NULL) {
            s.on_link(src, true);
        }
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
    if (s.off && (int32_t)(now - s.off_until_ms) >= 0) {
        lgbb_enable();   /* restores itself, whatever happened to the tool that turned it off */
    }
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

size_t lgbb_link_traffic(lgbb_link_traffic_t *out, size_t max, uint32_t now_ms)
{
    size_t n = 0;
    for (uint16_t node = 0; node < LG_MAX_NODES && n < max; node++) {
        const lgbb_link_stats_t *st = &s.stats[node];
        const lgbb_link_t *l = link_find(node);
        if (l == NULL && !st->heard && st->tx_frames == 0) {
            continue;
        }
        out[n].node = node;
        out[n].up = l != NULL && l->usable;
        out[n].rssi = l != NULL ? (int8_t)l->rssi : 0;
        out[n].tx_frames = st->tx_frames;
        out[n].tx_bytes = st->tx_bytes;
        out[n].rx_frames = st->rx_frames;
        out[n].rx_bytes = st->rx_bytes;
        out[n].tx_fail = st->tx_fail;
        out[n].heard_age_ms = st->heard ? now_ms - st->heard_ms : 0xFFFFFFFFu;
        n++;
    }
    return n;
}

void lgbb_radio_traffic(lgbb_radio_traffic_t *out)
{
    out->tx_frames = s.tx_frames;
    out->tx_fail = s.tx_fail;
    out->tx_dropped = s.tx_dropped;
    out->tx_nomem = s.tx_nomem;
    out->rx_frames = s.rx_frames;
    out->rx_auth_fail = s.rx_auth_fail;
    out->rx_replay = s.rx_replay;
    out->rx_queue_full = s.rx_queue_full;
    out->tx_queued = (uint8_t)s.tx_count;
}

uint32_t lgbb_tx_frame_count(void)
{
    return s.tx_frames;
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
