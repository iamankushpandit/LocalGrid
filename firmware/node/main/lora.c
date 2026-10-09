/* lora.c - the RYLR998 second backbone (D71). See lora.h and docs/lora.md. */
#include "lora.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_body.h"
#include "lg_envelope.h"
#include "lg_lora_wire.h"
#include "lg_lora_hh.h"
#include "lg_gridkeys.h"
#include "node_app.h"
#include "nvs.h"

static const char *TAG = "LORA";

/* D77: the key that seals LoRa frames between a handheld and an AP. Never the backbone key. */
#define s_lora_hh_key (lg_gridkeys()->lora_hh)   /* from this device's own storage, or the build (D96) */
static int8_t s_power_dbm = LG_LORA_POWER_FULL;   /* D98: what AT+CRFOP is set to */

#define LORA_UART         UART_NUM_1
#define UART_RX_BUF       1024
#define UART_TX_BUF       512
#define LORA_LINE_MAX     320u      /* "+RCV=" plus a part's characters plus its numbers, with room over */
#define TASK_STACK        4096
#define TASK_PRIORITY     3         /* below the core task (5): airtime can wait */
#define AT_TIMEOUT_MS     1000u
#define SEND_TIMEOUT_MS   8000u     /* one full part is about a second of airtime, plus the module */
#define DETECT_TRIES      3u
#define PART_TRIES        3u
#define BACKOFF_MAX_MS    300u      /* random wait before every transmission: no carrier sense */
#define RETRY_MIN_MS      800u      /* a failed part waits longer, and randomly, before trying again */
#define RETRY_SPAN_MS     2200u
#define RX_RING           2u
#define WEDGE_FAILS       5u        /* consecutive failed parts before the module is reset */
#define WEDGE_BACKOFF_MS  2000u
/*
 * D104. Every AP that owes an acknowledgement sends it in a turn of its own, because the frame may have
 * gone to all of them at once and two answers at the same moment are two answers lost. A turn is the
 * acknowledgement's time on the air and the module's own delay. See ack_turn() for how turns are counted.
 */
/*
 * The module needs quiet time between packets as a RECEIVER: back-to-back 120-character frames
 * 0.8 s apart arrived 14 of 16 on the bench, 1.2 s apart 13-16 of 16, and 1.5 s or more apart
 * 15-16 of 16 (2026-10-08). So every sender leaves PART_GAP_MIN_MS of idle air after each of its
 * own transmissions, except before an alert, and a holder's estimate of a mid-flight frame counts
 * the same gap.
 */
#define PART_GAP_MIN_MS   1500u
#define AIR_PART_GAP_MS   1600u
#define AIR_HOLD_MARGIN_MS 300u
#define AIR_HOLD_CAP_MS   8000u
#define ACK_TURN_MS       500u
#define ACK_JITTER_MS     80u
/*
 * On top of the last turn, before the sender stops listening. It has to cover a part the answering AP
 * is already putting on the air when its answer comes due, since a transfer in progress is never
 * interrupted for one, and that part can be a full one, about 940 ms. At 400 ms, with the Wi-Fi backbone
 * off on the bench, Denali heard 5 of Everest's 11 answers in time and sent 10 frames again that had
 * arrived the first time (2026-10-07): airtime spent twice, on a channel that had none to spare.
 */
#define ACK_MARGIN_MS     1500u
#define ACK_RESEND_SPAN_MS 600u     /* and a random wait more before it sends again */
#define WEDGE_BACKOFF_MAX 60000u
#define IDLE_WAIT_MS      50u       /* how long the task listens for a line when it has work to do */
#define QUIET_WAIT_MS     1000u     /* ... and with no module: purely passive, and costs nothing */

/*
 * Memory. Every byte of it is taken ONCE, when a module has answered, and never again; nothing
 * here allocates per message. An AP with no module holds no frame buffers at all, which is the
 * first thing this feature has to protect.
 *
 *   small slots   2 reassembly + 4 queue, LORA_SMALL_MAX each          3.0 KB
 *   long payload  1 reassembly slot, LORA_PAYLOAD_MAX                  6.1 KB
 *   long payload  1 queue slot, LORA_PAYLOAD_MAX, only if the heap      6.1 KB
 *                 still has HEAP_FLOOR to spare afterwards
 *
 * The long-payload slots exist for the 6 KB voice note of docs/lora.md; nothing produces one yet.
 * Receiving one matters first, because a peer decides what it sends, so the reassembly slot is
 * taken whenever a module is fitted and the send slot only where there is room. MAIN, which runs
 * at about 24 KB free, will usually skip the send slot and say so; it can still receive.
 */
#define SMALL_ASM_SLOTS  2u
#define SMALL_TXQ_SLOTS  4u
#define HEAP_FLOOR       24576u   /* free heap that must remain after taking the send slot */

_Static_assert(SMALL_ASM_SLOTS + 1u == LORA_ASM_SLOTS, "two small reassembly slots and one large");
_Static_assert(SMALL_TXQ_SLOTS + 1u == LORA_TXQ_SLOTS, "four small queue slots and one large");

/*
 * What crosses from the LoRa task to the core task: not the payload, but which reassembly slot is
 * holding it. The core task opens it where it lies and hands the slot back, so a 6 KB payload
 * needs no second 6 KB buffer anywhere in the AP.
 */
typedef struct {
    lora_asm_slot_t *slot;
    int8_t           rssi;
    int8_t           snr;
} lora_rx_t;

typedef struct {
    bool     up;
    bool     heard;
    uint32_t boot;
    uint32_t last_ms;
    int8_t   rssi;
    int8_t   snr;
    uint8_t  hears;      /* bit i: this AP's last HELLO listed AP i as a neighbour (LG_MAX_NODES is 8) */
} lora_peer_t;

static struct {
    uint8_t     index;
    uint16_t    address;
    uint8_t     networkid;
    lora_pins_t pins;
    /*
     * D77: the handheld side of the radio. Its own key, its own sequence counter and its own replay
     * window, entirely separate from the backbone's, so neither domain can retire the other's frames.
     */
    lglh_t      hh;
    uint32_t    hh_frames_in;      /* frames opened from a handheld */
    uint32_t    hh_refused_type;   /* refused by the allow-list, however well sealed */
    bool        started;
    bool        buffers;          /* the frame buffers were taken: this AP can carry LoRa traffic */
    bool        big;              /* ... including a long payload both ways */
    char        version[28];

    volatile bool fitted;         /* a module is answering */
    volatile bool configured;
    volatile bool ever_fitted;
    volatile bool broadcast_ok;
    volatile bool reset_wanted;
    volatile bool at_pending;
    char          at_text[64];

    /* Chaos hook (console only; never the BLE link, never the admin page). Restores itself. */
    volatile bool     off;
    volatile uint32_t off_until_ms;

    SemaphoreHandle_t lock;       /* guards txq, the rx ring and msg_id */
    lora_txq_t        txq;
    lora_rx_t         rx[RX_RING];
    size_t            rx_head, rx_count;
    uint8_t           msg_id;

    lora_asm_t  asmb;             /* LoRa task only */

    /* core task only */
    lora_peer_t     peers[LG_MAX_NODES];
    uint32_t        next_hb_ms;
    bool            force_once;
    uint8_t         hb_round;     /* which AP the heartbeat goes to when broadcast is off */
    lgbb_frame_cb_t on_frame;
    lgbb_link_cb_t  on_link;

    /* counters: 32-bit adds, read without a lock for the traffic record (D70) */
    uint32_t frames_out, frames_in, frames_first, parts_out, parts_in, parts_dropped;
    uint32_t seal_fail, airtime_ms, retries, refused_big, heard_ms;
    uint32_t gave_way;   /* housekeeping frames that waited rather than crowd the air */
    bool     relaying;   /* a frame that arrived over LoRa is being handled right now ... */
    uint16_t relay_src;  /* ... from this AP */
    uint32_t relays_spared;   /* floods not repeated over LoRa because their sender reaches everyone here */
    uint32_t hb_waited;       /* times a heartbeat found the queue full of messages and came back later */
    uint32_t took_over;       /* messages ESP-NOW could not deliver, put on the air here instead (D105) */
    uint32_t acks_out;        /* acknowledgements sent (D104) */
    uint32_t acks_in;         /* acknowledgements that answered a frame waiting here */
    uint32_t acks_late;       /* acknowledgements for a frame no longer waiting: answered twice, or given up */
    uint32_t listen_until_ms; /* only alerts and acknowledgements are transmitted before this */
    uint32_t air_busy_until_ms; /* somebody's multi-part frame is mid-flight: hold non-alert sends */
    uint32_t held_for_air;    /* times a send waited for it */
    bool     holding_for_air;
    uint32_t last_tx_end_ms;  /* when this AP's own last transmission finished */
    uint32_t configured_at_ms; /* when the module last came up: discovery hellos run fast from here */
    bool     heard_any;
    uint8_t  restarts;
    int8_t   rssi, snr;
} L;

static void lock(void)   { if (L.lock != NULL) { xSemaphoreTake(L.lock, portMAX_DELAY); } }
static void unlock(void) { if (L.lock != NULL) { xSemaphoreGive(L.lock); } }

static uint32_t rand_ms(uint32_t span)
{
    return span == 0 ? 0u : esp_random() % span;
}

/* ---- the serial line ---- */

static char   s_line[LORA_LINE_MAX];
static size_t s_line_len;

/* One line the module printed, without its ending. False when nothing arrived in time. */
static bool read_line(uint32_t timeout_ms, char **out)
{
    uint32_t start = app_now_ms();
    for (;;) {
        uint32_t spent = app_now_ms() - start;
        if (spent >= timeout_ms) {
            return false;
        }
        uint8_t c = 0;
        if (uart_read_bytes(LORA_UART, &c, 1, pdMS_TO_TICKS(timeout_ms - spent)) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (s_line_len == 0) {
                continue;
            }
            s_line[s_line_len] = '\0';
            s_line_len = 0;
            *out = s_line;
            return true;
        }
        if (s_line_len + 1u < LORA_LINE_MAX) {
            s_line[s_line_len++] = (char)c;
        } else {
            s_line_len = 0;   /* longer than anything this module sends: noise */
        }
    }
}

static void handle_rcv(const lora_at_t *at);
static bool take_buffers(void);
static bool lora_air_is_quiet(void);

/*
 * Diagnosis counter (2026-10-08): on the bench, a quarter of full-size parts never reached
 * parts_in with a clean channel and SNR 10, and nothing counted them. A line the module prints
 * that parses as nothing is the one way a part can vanish inside this firmware, so it is counted
 * where it is ignored.
 */
static uint32_t s_at_noise;

static bool parse_or_count(char *line, lora_at_t *at)
{
    if (lora_at_parse(line, at)) {
        return true;
    }
    if (line != NULL && line[0] != '\0') {
        s_at_noise++;
    }
    return false;
}

/*
 * Waits for one kind of reply. Frames arriving while we wait are never lost: +RCV is handled
 * wherever it turns up, including in the middle of a transmission.
 */
static bool at_wait(lora_at_kind_t want, uint32_t timeout_ms, char *value, size_t vcap, int *err)
{
    uint32_t start = app_now_ms();
    for (;;) {
        uint32_t spent = app_now_ms() - start;
        if (spent >= timeout_ms) {
            return false;
        }
        char *line = NULL;
        if (!read_line(timeout_ms - spent, &line)) {
            return false;
        }
        lora_at_t at;
        if (!parse_or_count(line, &at)) {
            continue;
        }
        if (at.kind == LORA_AT_RCV) {
            handle_rcv(&at);
            continue;
        }
        if (at.kind == LORA_AT_ERR && want != LORA_AT_ERR) {
            if (err != NULL) {
                *err = at.err;
            }
            return false;
        }
        if (at.kind == want) {
            if (value != NULL && vcap > 0) {
                snprintf(value, vcap, "%s", at.kind == LORA_AT_VALUE && at.value != NULL ? at.value : "");
            }
            return true;
        }
    }
}

static void at_write(const char *cmd)
{
    (void)uart_write_bytes(LORA_UART, cmd, strlen(cmd));
    (void)uart_write_bytes(LORA_UART, "\r\n", 2);
}

/* One command that must answer +OK. */
static bool at_ok(const char *cmd, uint32_t timeout_ms)
{
    int err = 0;
    at_write(cmd);
    if (at_wait(LORA_AT_OK, timeout_ms, NULL, 0, &err)) {
        return true;
    }
    if (err != 0) {
        ESP_LOGW(TAG, "[LORA] \"%s\" refused: +ERR=%d", cmd, err);
    }
    return false;
}

/* One query whose answer is "+NAME=value". */
static bool at_value(const char *cmd, char *value, size_t cap)
{
    at_write(cmd);
    return at_wait(LORA_AT_VALUE, AT_TIMEOUT_MS, value, cap, NULL);
}

/* ---- the module ---- */

static void module_reset(void)
{
    if (L.pins.rst >= 0) {
        /* Driving a pin that was never wired to anything is harmless, which is what lets an AP
         * run this firmware with the module half fitted, or not fitted at all. */
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (unsigned)L.pins.rst,
            .mode = GPIO_MODE_OUTPUT,
        };
        (void)gpio_config(&io);
        (void)gpio_set_level((gpio_num_t)L.pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        (void)gpio_set_level((gpio_num_t)L.pins.rst, 1);
    } else {
        at_write("AT+RESET");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    (void)uart_flush_input(LORA_UART);
    s_line_len = 0;
}

static bool detect(void)
{
    for (unsigned i = 0; i < DETECT_TRIES; i++) {
        if (at_ok("AT", AT_TIMEOUT_MS)) {
            return true;
        }
    }
    return false;
}

/*
 * The whole configuration, in order. Every command is checked for its +OK; the first refusal
 * gives up, and the caller resets the module and tries again after a backoff.
 */
static bool configure(void)
{
    char cmd[48];
    char value[sizeof(L.version)];

    if (at_value("AT+VER?", value, sizeof(value))) {
        snprintf(L.version, sizeof(L.version), "%s", value);
    }
    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%u", (unsigned)L.address);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%u", (unsigned)L.networkid);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+BAND=%" PRIu32, (uint32_t)LORA_BAND_HZ);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+PARAMETER=%d,%d,%d,%d", LORA_SF, LORA_BW_CODE, LORA_CR_CODE, LORA_PREAMBLE);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", s_power_dbm);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    if (!at_ok("AT+MODE=0", AT_TIMEOUT_MS)) {
        return false;
    }
    /* Read back what the module thinks, so the log says what is set rather than what we asked. */
    char addr[16] = "?", net[16] = "?", par[32] = "?";
    (void)at_value("AT+ADDRESS?", addr, sizeof(addr));
    (void)at_value("AT+NETWORKID?", net, sizeof(net));
    (void)at_value("AT+PARAMETER?", par, sizeof(par));
    ESP_LOGI(TAG, "[LORA] Configured %s: address %s, network %s, parameters %s, %" PRIu32 " Hz, %d dBm",
             L.version[0] != '\0' ? L.version : "module", addr, net, par, (uint32_t)LORA_BAND_HZ, s_power_dbm);
    return true;
}

/* D98: transmit power, kept in this AP's own NVS. Not grid state: which devices sit close together
 * is a fact about each one's place, not about the grid. */
#define LORA_NVS_NAMESPACE "lglora"
#define LORA_NVS_DBM       "dbm"

static void power_load(void)
{
    nvs_handle_t h;
    int8_t dbm = LG_LORA_POWER_FULL;
    if (nvs_open(LORA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i8(h, LORA_NVS_DBM, &dbm);
        nvs_close(h);
    }
    s_power_dbm = (int8_t)lg_lora_power_clamp(dbm);
}

void lora_set_power(int dbm)
{
    s_power_dbm = (int8_t)lg_lora_power_clamp(dbm);
    nvs_handle_t h;
    if (nvs_open(LORA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_i8(h, LORA_NVS_DBM, s_power_dbm) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", s_power_dbm);
    lora_request_at(cmd);
    ESP_LOGI(TAG, "[LORA] Transmit power: %s (%d dBm)", lg_lora_power_name(s_power_dbm), s_power_dbm);
}

int lora_power(void)
{
    return s_power_dbm;
}

/* ---- receive ---- */

static void handle_rcv(const lora_at_t *at)
{
    static uint8_t raw[LORA_PART_RAW_MAX];

    L.parts_in++;
    L.rssi = (int8_t)(at->rssi < -128 ? -128 : at->rssi > 127 ? 127 : at->rssi);
    L.snr = (int8_t)(at->snr < -128 ? -128 : at->snr > 127 ? 127 : at->snr);
    if (L.off) {
        L.parts_dropped++;   /* the chaos hook: the radio hears, but this AP is pretending not to */
        return;
    }

    size_t n = lora_b64_decode(at->data, at->len, raw, sizeof(raw));
    if (n == 0) {
        L.parts_dropped++;
        ESP_LOGW(TAG, "[LORA] A part arrived garbled: %u characters that were not base64", (unsigned)at->len);
        return;
    }
    /* Every piece of a multi-part frame is named as it arrives: the one trace that says where a
     * frame died when only some of it ever came (2026-10-08). */
    lora_part_t ph;
    if (lora_part_parse(raw, n, &ph)) {
        ESP_LOGI(TAG, "[LORA] Part %u/%u of %u from peer 0x%02X, %u chars, RSSI %d dBm, SNR %d",
                 (unsigned)(ph.part + 1u), (unsigned)ph.of, (unsigned)ph.id, (unsigned)ph.peer,
                 (unsigned)at->len, (int)L.rssi, (int)L.snr);
        /*
         * Carrier sense at the protocol layer (2026-10-08). The radios have none of their own, and a
         * receiver that transmits while somebody's multi-part frame is mid-flight is deaf for a part
         * of it: with three-part frames on the bench, one witness kept 8 of 8 while the other, busier,
         * kept 3 of 8. The header says how many parts are still coming, so everything that hears one
         * waits that much airtime before its own non-alert transmissions.
         */
        if (ph.part + 1u < ph.of) {
            uint32_t more = (uint32_t)(ph.of - ph.part - 1u);
            uint32_t wait = more * (lora_airtime_ms(LORA_PART_B64_MAX) + AIR_PART_GAP_MS) + AIR_HOLD_MARGIN_MS;
            wait = wait > AIR_HOLD_CAP_MS ? AIR_HOLD_CAP_MS : wait;
            uint32_t until = app_now_ms() + wait;
            if ((int32_t)(until - L.air_busy_until_ms) > 0) {
                L.air_busy_until_ms = until;
            }
        }
    }
    lora_asm_slot_t *slot = NULL;
    lora_asm_result_t r = lora_asm_feed(&L.asmb, app_now_ms(), raw, n, &slot);
    if (r == LORA_ASM_OVERSIZE) {
        static uint32_t said;
        L.refused_big++;
        if (said != L.refused_big && L.refused_big <= 3u) {
            said = L.refused_big;
            ESP_LOGW(TAG, "[LORA] A payload larger than %u bytes was offered; refused, not truncated",
                     (unsigned)lora_asm_capacity(&L.asmb));
        }
        return;
    }
    if (r != LORA_ASM_COMPLETE) {
        return;   /* lora_asm_feed counts everything it refused */
    }
    lock();
    if (L.rx_count >= RX_RING) {
        unlock();
        lora_asm_done(&L.asmb, slot);
        L.parts_dropped++;   /* the core task has not drained in a whole pass: exceedingly unlikely */
        return;
    }
    lora_rx_t *e = &L.rx[(L.rx_head + L.rx_count) % RX_RING];
    e->slot = slot;
    e->rssi = L.rssi;
    e->snr = L.snr;
    L.rx_count++;
    unlock();
}

/*
 * D104: the turn in which AP `answerer` acknowledges a frame from AP `src`, from 1. The APs that could
 * answer go in order of index after the sender's, counting only those present: this AP itself and its
 * LoRa peers that are up. The sender and the answerer each count from their own list of peers, and the
 * two lists can differ by an AP that one of them has just heard or lost; that moves an answer by one
 * turn, inside the sender's ACK_MARGIN_MS.
 *
 * Counted over all LG_MAX_NODES indices, as it was, Everest (0) answering Denali (2) waited six turns,
 * 2.5 s, on a grid of three. Denali, holding back everything but answers while it listened, could not
 * keep up with a talk's compact copy arriving every 2.8 s: its queue dropped 8 frames and one packet of
 * four crossed (2026-10-07). Now the one AP addressed answers at once.
 */
static uint32_t ack_turn(uint16_t src, uint16_t answerer)
{
    uint32_t dist = (uint32_t)((answerer + LG_MAX_NODES - src) % LG_MAX_NODES);
    uint32_t turn = 1;
    for (uint16_t p = 0; p < LG_MAX_NODES; p++) {
        if (p == src || p == answerer || !(p == L.index || L.peers[p].up)) {
            continue;
        }
        if ((uint32_t)((p + LG_MAX_NODES - src) % LG_MAX_NODES) < dist) {
            turn++;
        }
    }
    return turn;
}

/* ---- transmit ---- */

static bool send_part(uint16_t dest, const char *b64, size_t b64_len)
{
    static char cmd[40 + LORA_PART_B64_MAX];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%u,%u,%.*s", (unsigned)dest, (unsigned)b64_len, (int)b64_len, b64);
    at_write(cmd);
    L.airtime_ms += lora_airtime_ms(b64_len);
    int err = 0;
    if (at_wait(LORA_AT_OK, SEND_TIMEOUT_MS, NULL, 0, &err)) {
        L.parts_out++;
        return true;
    }
    if (err != 0) {
        ESP_LOGW(TAG, "[LORA] Transmission refused: +ERR=%d", err);
    }
    return false;
}

/* One part of a payload, with its random backoff and its retries. */
static bool send_one(uint16_t dest, uint8_t id, const uint8_t *payload, size_t len, size_t part, size_t of)
{
    static uint8_t raw[LORA_PART_RAW_MAX];
    static char    b64[LORA_PART_B64_MAX + 1];

    size_t off = part * LORA_SLICE_MAX;
    size_t slice = len - off > LORA_SLICE_MAX ? LORA_SLICE_MAX : len - off;
    size_t plen = lora_part_build((uint8_t)(LORA_PEER_AP | L.index), id, (uint8_t)part, (uint8_t)of,
                                  payload + off, slice, raw, sizeof(raw));
    size_t blen = plen != 0 ? lora_b64_encode(raw, plen, b64, sizeof(b64) - 1u) : 0u;
    if (blen == 0) {
        L.seal_fail++;
        return false;
    }
    b64[blen] = '\0';
    for (unsigned attempt = 0; attempt < PART_TRIES; attempt++) {
        /* Three APs share one channel and this module has no carrier sense, so every transmission
         * waits a short random time, and one that failed waits longer and randomly again. */
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? rand_ms(BACKOFF_MAX_MS) : RETRY_MIN_MS + rand_ms(RETRY_SPAN_MS)));
        if (attempt > 0) {
            L.retries++;
        }
        if (send_part(dest, b64, blen)) {
            return true;
        }
    }
    return false;
}

/* ---- the LoRa task ---- */

static void run_pending_at(void)
{
    char cmd[sizeof(L.at_text)];
    lock();
    snprintf(cmd, sizeof(cmd), "%s", L.at_text);
    L.at_pending = false;
    unlock();
    ESP_LOGI(TAG, "[LORA] > %s", cmd);
    at_write(cmd);
    char *line = NULL;
    uint32_t started = app_now_ms();
    while (app_now_ms() - started < AT_TIMEOUT_MS * 2u && read_line(AT_TIMEOUT_MS, &line)) {
        lora_at_t at;
        if (lora_at_parse(line, &at) && at.kind == LORA_AT_RCV) {
            handle_rcv(&at);
            continue;
        }
        ESP_LOGI(TAG, "[LORA] < %s", line);
    }
}

/*
 * One part of one payload, chosen fresh each time round the loop. The payload stays in its queue
 * slot with its progress, so an alert that arrives during a long transfer goes out after at most
 * one part rather than after the whole thing, and the long transfer then carries on where it was:
 * each part carries its own number, and the receiver keeps a set per (sender, message number).
 */
static bool transmit_a_part(unsigned *fails)
{
    uint16_t dest;
    uint8_t  id;
    size_t   len, part, of;

    lock();
    uint32_t now = app_now_ms();
    bool listening = (int32_t)(now - L.listen_until_ms) < 0 && lora_txq_awaiting(&L.txq);
    lora_txq_slot_t *slot = lora_txq_peek_for(&L.txq, now, listening);
    if (slot == NULL) {
        unlock();
        return false;
    }
    /* Somebody else's frame is mid-flight: everything but an alert and an acknowledgement, and but
     * a frame of our own already under way (its receivers are holding for us in turn), waits for
     * the air. Acknowledgements are never held: held behind a sender's resend stream, the ack that
     * would stop the stream never went, the sender resent, and the hold renewed - Everest put 31
     * frames on the air while Denali, hearing every one, answered 4 (2026-10-08). */
    if (!slot->alert && !slot->is_ack && slot->part == 0u && (int32_t)(now - L.air_busy_until_ms) < 0) {
        if (!L.holding_for_air) {
            L.holding_for_air = true;
            L.held_for_air++;
        }
        unlock();
        return false;
    }
    /* And the receivers need PART_GAP_MIN_MS of quiet after each packet, ours included, or they
     * miss the next one; the task keeps listening meanwhile. An alert or an ack goes at once. */
    if (!slot->alert && !slot->is_ack && (int32_t)(now - (L.last_tx_end_ms + PART_GAP_MIN_MS)) < 0) {
        unlock();
        return false;
    }
    L.holding_for_air = false;
    slot->sending = true;
    dest = slot->dest;
    id = slot->id;
    len = slot->len;
    part = slot->part;
    of = slot->of;
    const uint8_t *data = slot->data;
    unlock();

    /*
     * The slot is marked `sending`, which keeps the queue from evicting or reusing it, so reading
     * its bytes outside the lock is safe. Only this task ever transmits.
     */
    bool ok = send_one(dest, id, data, len, part, of);

    lock();
    L.last_tx_end_ms = app_now_ms();   /* receivers need quiet from here, sent or not */
    if (!slot->in_use || slot->id != id) {
        unlock();   /* it was released under us (the module was reset): nothing to finish */
        return true;
    }
    if (!ok) {
        lora_txq_release(&L.txq, slot);   /* abandoned cleanly; the receiver's set times out */
        unlock();
        if (++*fails >= WEDGE_FAILS) {
            *fails = 0;
            L.restarts++;
            ESP_LOGW(TAG, "[LORA] %u parts in a row failed; resetting the module", WEDGE_FAILS);
            L.configured = false;
        }
        return true;
    }
    *fails = 0;
    slot->part++;
    if (slot->part >= slot->of) {
        /*
         * All of it is out. If APs are to acknowledge it (D104), work out when the last of them will
         * have had its turn, listen until then, and send again a random moment after if any has not.
         */
        uint32_t turns = 0;
        for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
            if ((slot->await & (1u << n)) != 0u) {
                uint32_t turn = ack_turn(L.index, n);
                turns = turn > turns ? turn : turns;
            }
        }
        if ((slot->await & LORA_AWAIT_HANDHELD) != 0u && turns == 0u) {
            turns = 1;   /* D109: the one handheld it is for answers at once */
        }
        uint32_t done = app_now_ms();
        uint32_t listen = turns * ACK_TURN_MS + ACK_MARGIN_MS;
        if (lora_txq_sent(&L.txq, slot, done + listen + rand_ms(ACK_RESEND_SPAN_MS))) {
            L.listen_until_ms = done + listen;
        }
        L.frames_out++;
    }
    unlock();
    return true;
}

static void drop_queue(void)
{
    lock();
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        L.txq.slots[i].in_use = false;
        L.txq.slots[i].sending = false;
    }
    L.txq.depth = 0;
    unlock();
}

static void lora_task(void *arg)
{
    (void)arg;
    uint32_t backoff = WEDGE_BACKOFF_MS;
    uint32_t next_retry = 0;
    unsigned fails = 0;
    bool first_look = true;
    bool said_missing = false;

    for (;;) {
        /*
         * No module. The task only listens, on a port nothing is driving, and never transmits,
         * never logs and never times anything: an AP with no module is exactly an AP built before
         * D71. If a module is plugged in later it prints +READY as it powers up, or answers the
         * one AT we then send, and this AP picks it up without a restart.
         */
        if (!L.fitted) {
            /*
             * One look at start-up, and after that only when something on the line speaks: a
             * module powering up prints +READY, and a reply to nothing costs nothing. No timer,
             * no retry storm, no second log line.
             */
            char *line = NULL;
            if (!first_look && !read_line(QUIET_WAIT_MS, &line)) {
                continue;
            }
            first_look = false;
            if (!detect()) {
                if (!said_missing) {
                    said_missing = true;
                    ESP_LOGI(TAG, "[LORA] No module on GPIO%d/%d; this AP has one backbone, exactly as "
                                  "before", L.pins.rx, L.pins.tx);
                }
                continue;
            }
            if (!take_buffers()) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            ESP_LOGI(TAG, "[LORA] Module on UART1 (rx GPIO%d, tx GPIO%d), address %u, network %u",
                     L.pins.rx, L.pins.tx, (unsigned)L.address, L.networkid);
            said_missing = false;
            L.fitted = true;
            L.ever_fitted = true;
            L.configured = false;
            next_retry = 0;
            fails = 0;
            backoff = WEDGE_BACKOFF_MS;
            continue;
        }

        if (!L.configured) {
            if ((int32_t)(app_now_ms() - next_retry) < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            drop_queue();
            if (configure()) {
                L.configured = true;
                L.configured_at_ms = app_now_ms();
                fails = 0;
                backoff = WEDGE_BACKOFF_MS;
                continue;
            }
            module_reset();
            if (!detect()) {
                L.fitted = false;
                ESP_LOGW(TAG, "[LORA] The module stopped answering; this AP carries on with one backbone");
                continue;
            }
            next_retry = app_now_ms() + backoff;
            backoff = backoff * 2u > WEDGE_BACKOFF_MAX ? WEDGE_BACKOFF_MAX : backoff * 2u;
            continue;
        }

        if (L.reset_wanted) {
            L.reset_wanted = false;
            L.configured = false;
            L.restarts++;
            next_retry = 0;
            ESP_LOGW(TAG, "[LORA] Resetting the module");
            module_reset();
            continue;
        }
        if (L.at_pending) {
            run_pending_at();
            continue;
        }

        /* Anything the module says on its own, chiefly +RCV, and the wait that paces this loop. */
        char *line = NULL;
        if (read_line(IDLE_WAIT_MS, &line)) {
            lora_at_t at;
            if (parse_or_count(line, &at)) {
                if (at.kind == LORA_AT_RCV) {
                    handle_rcv(&at);
                } else if (at.kind == LORA_AT_READY) {
                    ESP_LOGW(TAG, "[LORA] The module restarted by itself; configuring it again");
                    L.configured = false;
                    next_retry = 0;
                    continue;
                }
            }
        }
        (void)lora_asm_expire(&L.asmb, app_now_ms());

        if (L.off) {
            drop_queue();   /* the chaos hook: abandon cleanly, the receiver's sets time out */
            continue;
        }
        (void)transmit_a_part(&fails);
    }
}

/* ---- the core task's side ---- */

/*
 * Seals one frame straight into the queue slot that will carry it: nothing is staged in a second
 * buffer, so the largest payload this AP can send is simply the largest slot it has.
 */
/*
 * D77: the same, sealed with the handheld LoRa key instead of the backbone key, for a frame going to
 * one handheld. A handheld cannot open a backbone-sealed frame and must never be sent one.
 */
static bool queue_sealed_hh(uint16_t dest, bool alert, const uint8_t *frame, size_t len)
{
    size_t need = len + LGLH_OUTER_LEN + LG_AEAD_TAG_LEN;
    uint8_t rank = alert ? LORA_RANK_ALERT : lora_frame_rank(frame, len);   /* D102 */
    lock();
    lora_txq_slot_t *slot = lora_txq_reserve_ranked(&L.txq, dest, rank, L.msg_id, need,
                                                    app_now_ms() + rand_ms(BACKOFF_MAX_MS));
    int n = slot != NULL ? lglh_seal(&L.hh, slot->data, slot->cap, frame, len) : -1;
    bool ok = n > 0 && lora_txq_commit(&L.txq, slot, (size_t)n);
    if (slot != NULL && !ok) {
        lora_txq_release(&L.txq, slot);
    }
    if (ok) {
        L.msg_id++;
        /* D109: a message for a handheld on LoRa alone is acknowledged by it, and sent again until it is,
         * as one between APs is (D104); since 2026-10-08 a talk's compact copy too, instead of being
         * said twice (D95). A broadcast address has nobody in particular to answer. */
        if (dest != LORA_ADDR_BROADCAST && lora_frame_wants_ap_ack(frame, len)) {
            lora_txq_want_ack(slot, lg_rd32(slot->data + 8), LORA_AWAIT_HANDHELD);
        }
    }
    unlock();
    if (!ok) {
        L.seal_fail++;
    }
    return ok;
}

/*
 * `await` is the APs that are to acknowledge it, one bit each, and is ignored for a frame that wants no
 * acknowledgement (D104). `ack_after_ms` is not zero for a frame that is itself an acknowledgement:
 * how long to wait for this AP's turn before sending it.
 */
static bool queue_sealed_for(uint16_t dest, bool alert, const uint8_t *frame, size_t len, uint16_t await,
                             uint32_t ack_after_ms)
{
    size_t need = len + LGBB_OUTER_LEN + LG_AEAD_TAG_LEN;
    bool is_ack = ack_after_ms != 0u;
    uint8_t rank = alert ? LORA_RANK_ALERT : is_ack ? LORA_RANK_MESSAGE : lora_frame_rank(frame, len);   /* D102 */
    lock();
    lora_txq_slot_t *slot = lora_txq_reserve_ranked(&L.txq, dest, rank, L.msg_id, need,
                                                    app_now_ms() + (is_ack ? ack_after_ms : rand_ms(BACKOFF_MAX_MS)));
    int n = slot != NULL ? lgbb_seal(slot->data, slot->cap, frame, len) : -1;
    bool ok = n > 0 && lora_txq_commit(&L.txq, slot, (size_t)n);
    if (slot != NULL && !ok) {
        lora_txq_release(&L.txq, slot);
    }
    if (ok) {
        L.msg_id++;
        slot->is_ack = is_ack;
        if (!is_ack && await != 0u && lora_frame_wants_ap_ack(frame, len)) {
            lora_txq_want_ack(slot, lg_rd32(slot->data + 8), await);   /* the outer header's sequence number */
        }
    }
    unlock();
    if (slot == NULL) {
        /*
         * No slot big enough, or none free. A payload larger than this AP can carry is refused
         * here with a reason rather than half sent (docs/lora.md, "Push-to-talk over LoRa").
         */
        static uint32_t said;
        L.refused_big++;
        if (said != L.refused_big && L.refused_big <= 3u) {
            said = L.refused_big;
            ESP_LOGW(TAG, "[LORA] A %u-byte frame did not fit the send queue; not put on the air",
                     (unsigned)len);
        }
    } else if (!ok) {
        L.seal_fail++;
    }
    return ok;
}

static bool queue_sealed(uint16_t dest, bool alert, const uint8_t *frame, size_t len)
{
    return queue_sealed_for(dest, alert, frame, len, 0, 0);
}

/* D104: tell AP `src` that its frame sealed as `seq` arrived here, when this AP's turn comes. */
static void queue_ack(uint16_t src, uint32_t seq)
{
    uint8_t body[LORA_ACK_LEN];
    if (src >= LG_MAX_NODES || src == L.index || lora_ack_build(seq, body, sizeof(body)) == 0u) {
        return;
    }
    uint32_t wait = (ack_turn(src, L.index) - 1u) * ACK_TURN_MS + 1u + rand_ms(ACK_JITTER_MS);
    if (queue_sealed_for((uint16_t)LORA_ADDR_AP(src), false, body, sizeof(body), 0, wait)) {
        L.acks_out++;
    }
}

/*
 * D105: ESP-NOW could not hand this message to AP `node`, and until now that was the end of it for the
 * six seconds it takes to notice a dead link. It is sealed for that AP already, in the very format this
 * backbone carries, so the same bytes go out here - and are acknowledged and repeated like any message
 * (D104). If the ESP-NOW copy did arrive after all and only its radio acknowledgement was lost, the
 * receiver sees a sequence number it has had, answers, and handles nothing twice.
 */
static void take_over(uint16_t node, const uint8_t *sealed, size_t len)
{
    if (!L.started || !L.fitted || L.off || sealed == NULL || node >= LG_MAX_NODES || !L.peers[node].up ||
        len <= LGBB_OUTER_LEN + LG_AEAD_TAG_LEN) {
        return;
    }
    lock();
    lora_txq_slot_t *slot = lora_txq_reserve_ranked(&L.txq, (uint16_t)LORA_ADDR_AP(node), LORA_RANK_MESSAGE,
                                                    L.msg_id, len, app_now_ms() + rand_ms(BACKOFF_MAX_MS));
    bool ok = slot != NULL;
    if (ok) {
        memcpy(slot->data, sealed, len);
        ok = lora_txq_commit(&L.txq, slot, len);
    }
    if (ok) {
        L.msg_id++;
        L.took_over++;
        lora_txq_want_ack(slot, lg_rd32(sealed + 8), (uint16_t)(1u << node));
    }
    unlock();
}

bool lora_is_peer(uint16_t node)
{
    return node < LG_MAX_NODES && L.peers[node].up && !L.off;
}

bool lora_fitted(void)
{
    return L.fitted;
}

void lora_force_next(void)
{
    L.force_once = true;
}

void lora_set_broadcast(bool on)
{
    L.broadcast_ok = on;
}

void lora_request_reset(void)
{
    L.reset_wanted = true;
}

void lora_request_at(const char *command)
{
    if (!L.started || command == NULL) {
        return;
    }
    lock();
    snprintf(L.at_text, sizeof(L.at_text), "%s", command);
    L.at_pending = true;
    unlock();
}

void lora_disable(uint32_t seconds)
{
    uint32_t s = seconds == 0 ? 1u : seconds;
    L.off_until_ms = app_now_ms() + s * 1000u;
    L.off = true;
    ESP_LOGW(TAG, "[LORA] Off for %" PRIu32 " s (chaos hook); it comes back on its own", s);
}

void lora_enable(void)
{
    if (L.off) {
        L.off = false;
        ESP_LOGW(TAG, "[LORA] Back on");
    }
}

bool lora_is_off(void)
{
    return L.off;
}

/*
 * D77/D80: offer one frame to a handheld this AP can reach over LoRa. Core task only.
 *
 * `wifi_ok` is whether that handheld has a working session here; the policy is the APs' own (D74),
 * so this costs no airtime while the session is up, except for the alerts that always go on both.
 * The allow-list is applied here too: an AP should not spend a second of airtime sending a handheld
 * something the handheld's own side would refuse to open.
 */
void lora_offer_handheld(uint32_t device, const uint8_t *frame, size_t len, bool wifi_ok)
{
    if (!L.fitted || !L.configured || L.off || frame == NULL || len == 0 || device == 0) {
        return;
    }
    lora_policy_t p = lora_policy_for_frame(frame, len);
    if (p == LORA_SEND_NEVER || (p != LORA_SEND_ALWAYS && wifi_ok)) {
        return;
    }
    if (!lglh_type_allowed(frame, len)) {
        L.hh_refused_type++;
        return;
    }
    /* A compact voice packet to a handheld goes once and is acknowledged like text (D109), not said
     * twice: with the radio's 1.5 s quiet time a duplicate is expensive air, and unacknowledged it
     * still lost a packet in three on the bench (2026-10-08). */
    (void)queue_sealed_hh(LG_LORA_ADDR_HANDHELD(device), p == LORA_SEND_ALWAYS, frame, len);
}

void lora_offer(uint16_t node, const uint8_t *frame, size_t len, bool wifi_ok)
{
    if (!L.started || !L.fitted || L.off) {
        return;
    }
    bool force = L.force_once;
    lora_policy_t policy = lora_policy_for_frame(frame, len);
    if (policy == LORA_SEND_NEVER) {
        return;   /* live voice never crosses LoRa, not even when the console asks (D61, D71) */
    }
    if ((policy == LORA_SEND_IF_WIFI_DOWN || policy == LORA_SEND_SPARE_ROOM) && wifi_ok && !force) {
        return;   /* ESP-NOW is carrying it: keep the air clear */
    }
    if (policy == LORA_SEND_SPARE_ROOM && !force && !lora_air_is_quiet()) {
        L.gave_way++;
        return;   /* housekeeping waits behind what people are waiting for */
    }
    if (node >= LG_MAX_NODES || (!L.peers[node].up && !force)) {
        return;   /* no LoRa link to that AP: transmitting at it would be wasted airtime */
    }
    L.force_once = false;
    /* A compact voice packet goes once and is acknowledged, like text (D95 as amended, D104). */
    (void)queue_sealed_for((uint16_t)LORA_ADDR_AP(node), policy == LORA_SEND_ALWAYS, frame, len,
                           (uint16_t)(1u << node), 0);
}

/*
 * Room to spare: fewer than half the small slots hold anything. The queue is only four slots deep,
 * so "half full" is the point where a message someone is waiting for would start queueing behind
 * housekeeping that can heal later or over Wi-Fi.
 */
static bool lora_air_is_quiet(void)
{
    return lora_txq_waiting(&L.txq) * 2u < SMALL_TXQ_SLOTS;
}

static uint8_t peer_count(uint16_t except_node)
{
    uint8_t n = 0;
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (L.peers[i].up && i != except_node) {
            n++;
        }
    }
    return n;
}

void lora_offer_flood(uint16_t except_node, const uint8_t *frame, size_t len)
{
    if (!L.started || !L.fitted || L.off) {
        return;
    }
    bool force = L.force_once;
    lora_policy_t policy = lora_policy_for_frame(frame, len);
    if (policy == LORA_SEND_NEVER) {
        return;
    }
    uint8_t reachable = peer_count(except_node);
    if (reachable == 0) {
        return;
    }
    /*
     * Rule 2 applies per AP: a flood goes on LoRa for the peers ESP-NOW is not reaching. If every
     * peer LoRa can reach is also reachable over Wi-Fi, and the frame is not an alert, the air
     * stays clear.
     */
    if (policy == LORA_SEND_SPARE_ROOM && !force && !lora_air_is_quiet()) {
        L.gave_way++;
        return;
    }
    bool needed = policy == LORA_SEND_ALWAYS || force;
    for (uint16_t i = 0; i < LG_MAX_NODES && !needed; i++) {
        if (L.peers[i].up && i != except_node && !lgbb_link_acked(i, app_now_ms())) {
            needed = true;
        }
    }
    if (!needed) {
        return;
    }
#if CONFIG_LG_NODE_LORA_SPARE_RELAY
    /*
     * An experiment (off by default). A frame that came in over LoRa was put on the air by an AP that
     * lists its own neighbours in every HELLO; if every LoRa peer here is one of them, that AP has
     * already sent it to them itself, and repeating it only puts a second and third copy on a channel
     * that carries one frame every few seconds. Measured on 2026-10-04 with ESP-NOW off on two of three
     * APs: each sent about 25 parts and received about 10, and the four-slot queue dropped 2 to 8 a
     * round. Alerts are never spared: they go everywhere, twice if need be.
     */
    if (policy != LORA_SEND_ALWAYS && !force && L.relaying && L.relay_src == except_node &&
        except_node < LG_MAX_NODES) {
        bool somebody_new = false;
        for (uint16_t i = 0; i < LG_MAX_NODES && i < 8u; i++) {
            if (L.peers[i].up && i != except_node && !(L.peers[except_node].hears & (1u << i))) {
                somebody_new = true;
            }
        }
        if (!somebody_new) {
            L.relays_spared++;
            return;
        }
    }
#endif
    L.force_once = false;
    bool alert = policy == LORA_SEND_ALWAYS;
    if (L.broadcast_ok && reachable > 1u) {
        uint16_t all = 0;   /* one transmission, and every AP it is for answers in its own turn (D104) */
        for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
            if (L.peers[i].up && i != except_node) {
                all = (uint16_t)(all | (1u << i));
            }
        }
        (void)queue_sealed_for(LORA_ADDR_BROADCAST, alert, frame, len, all, 0);
        return;
    }
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (L.peers[i].up && i != except_node) {
            (void)queue_sealed_for((uint16_t)LORA_ADDR_AP(i), alert, frame, len, (uint16_t)(1u << i), 0);
        }
    }
}

/* The peers backbone.c lists in its HELLO, so a LoRa-only link can be confirmed both ways. */
static size_t extra_peers(uint16_t *nodes, uint32_t *boots, size_t max)
{
    size_t n = 0;
    for (uint16_t i = 0; i < LG_MAX_NODES && n < max; i++) {
        if (L.peers[i].up) {
            nodes[n] = i;
            boots[n] = L.peers[i].boot;
            n++;
        }
    }
    return n;
}

static void peer_up(uint16_t src, int8_t rssi, int8_t snr)
{
    lora_peer_t *p = &L.peers[src];
    if (p->up) {
        return;
    }
    p->up = true;
    ESP_LOGI(TAG, "[LORA] Link up to AP %u, RSSI %d dBm, SNR %d", src, rssi, snr);
    if (L.on_link != NULL && !lgbb_is_neighbor(src)) {
        L.on_link(src, true);   /* the same catch-up an ESP-NOW link brings (D48, D53) */
    }
}

static void peer_down(uint16_t src, const char *why)
{
    lora_peer_t *p = &L.peers[src];
    if (!p->up) {
        return;
    }
    p->up = false;
    ESP_LOGW(TAG, "[LORA] Link down to AP %u (%s)", src, why);
    if (L.on_link != NULL && !lgbb_is_neighbor(src)) {
        L.on_link(src, false);
    }
}

/*
 * A HELLO that crossed on LoRa. Confirmation is the same as on ESP-NOW: the peer must list this
 * AP with this AP's current boot counter, which a replayed HELLO cannot contain.
 */
static void handle_hello(uint16_t src, const lg_env_t *e, const uint8_t *body, int8_t rssi, int8_t snr,
                         uint32_t boot, uint32_t now)
{
    lg_hello_t h;
    if (e->body_len < LG_HELLO_LEN + 1u || !lg_hello_dec(body, LG_HELLO_LEN, &h) || h.node != src) {
        return;
    }
    uint8_t count = body[LG_HELLO_LEN];
    if (count > LG_MAX_NODES || e->body_len != LG_HELLO_LEN + 1u + count * 6u) {
        return;
    }
    lora_peer_t *p = &L.peers[src];
    if (p->heard && p->boot != boot) {
        peer_down(src, "it restarted; down until confirmed again");
    }
    p->heard = true;
    p->boot = boot;
    p->last_ms = now;
    p->rssi = rssi;
    p->snr = snr;
    uint8_t hears = 0;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *q = body + LG_HELLO_LEN + 1u + i * 6u;
        if (lg_rd16(q) < 8u) {
            hears |= (uint8_t)(1u << lg_rd16(q));
        }
        if (lg_rd16(q) == L.index && lg_rd32(q + 2) == g_app.boot) {
            peer_up(src, rssi, snr);
        }
    }
    p->hears = hears;
}

/*
 * D77/D80: a frame a handheld sealed with the handheld LoRa key. Core task only.
 *
 * The handheld has no Wi-Fi session, so there is no REGISTER to set the core's session device. The
 * sender's index comes from the outer header, which is the AEAD's associated data and therefore
 * authenticated -- nobody without the key can claim to be device 4. It is presented to the core as
 * an already-registered session by pre-setting session_device, which is what lets a handheld out of
 * Wi-Fi range be heard at all (lg_node.c only demands a REGISTER while session_device is 0).
 *
 * The honest limit, recorded in D77: every handheld holds this one key, so the index is only as
 * strong as a shared secret and one handheld could claim to be another. The allow-list below is what
 * bounds that -- nothing an AP would have to *trust* a handheld about is accepted here -- and 1:1
 * message content stays sealed end to end with per-device keys this key cannot forge.
 */
static void deliver_from_handheld(const lora_rx_t *r, uint32_t now)
{
    static uint8_t inner[LG_FRAME_MAX];      /* core task only */
    /* One session per handheld, kept so the core sees a continuing session rather than a new one. */
    static uint32_t hh_session[LG_MAX_DEVICES];

    uint16_t sender = 0;
    size_t n = 0;
    lglh_open_t rc = lglh_open(&L.hh, r->slot->data, r->slot->total, &sender, inner, sizeof(inner), &n);
    if (rc != LGLH_OPEN_OK) {
        if (rc != LGLH_OPEN_NOT_MINE && rc != LGLH_OPEN_REPLAY) {
            L.seal_fail++;   /* a replay is the two radios agreeing, not a fault */
        }
        return;
    }
    if (LGLH_KIND(sender) != LGLH_KIND_HH) {
        return;   /* another AP using this key domain: not a thing that happens, and not ours to route */
    }
    uint32_t device = LGLH_INDEX(sender);
    if (device == 0 || device > LG_MAX_DEVICES) {
        return;
    }
    uint32_t acked_seq = 0;
    if (lora_ack_parse(inner, n, &acked_seq)) {
        /* D109: the handheld has a frame of ours. Not a grid frame: nothing for the core, nothing to route. */
        lock();
        bool waited = lora_txq_acked(&L.txq, acked_seq, 15u);
        unlock();
        if (waited) {
            L.acks_in++;
        } else {
            L.acks_late++;
        }
        L.hh_frames_in++;
        return;
    }
    /* D77: the closed list. Checked here as well as on the handheld, because this is the side that
     * must not be talked into rewriting the grid. */
    if (!lglh_type_allowed(inner, n)) {
        L.hh_refused_type++;
        return;
    }
    L.hh_frames_in++;
    L.frames_in++;
    L.heard_ms = now;
    L.heard_any = true;

    /*
     * D80: mark it reachable over LoRa before the frame is handled, so anything the core sends back
     * while handling it -- an acknowledgement above all -- already has somewhere to go.
     */
    lg_node_mark_lora_reachable(&g_app.core, device);

    hh_session[device - 1u] = device;
    lg_node_on_session_frame(&g_app.core, &hh_session[device - 1u], inner, n);
}

static void deliver(const lora_rx_t *r, uint32_t now)
{
    /*
     * Which key sealed this? The outer header's key-epoch byte says so before a single AEAD
     * operation is attempted: the backbone writes 0 there and a handheld writes LGLH_EPOCH_HH.
     * Asking first matters for more than speed -- handing a handheld's frame to lgbb_open would fail
     * authentication and be counted as a seal failure, so the admin page would show a climbing fault
     * count for perfectly ordinary traffic.
     */
    if (r->slot->total > 1u && r->slot->data[1] == LGLH_EPOCH_HH) {
        deliver_from_handheld(r, now);
        return;
    }
    static uint8_t inner[LG_FRAME_MAX];   /* core task only */
    uint16_t src = 0;
    uint32_t boot = 0;
    size_t n = 0;
    lgbb_open_t rc = lgbb_open(r->slot->data, r->slot->total, &src, &boot, inner, sizeof(inner), &n);
    if (rc == LGBB_OPEN_REPLAY) {
        /*
         * The same sealed frame a second time. Over LoRa that is its sender sending it again because it
         * never heard this AP's acknowledgement (D104) - the frame arrived, the answer did not - so the
         * answer is given again and the frame is not handled twice. lgbb_open authenticated it and
         * opened it into `inner` before it found the sequence number already used, which is why the
         * header and the plaintext can be read here.
         */
        uint16_t again = lg_rd16(r->slot->data + 2);
        size_t plain = r->slot->total - LGBB_OUTER_LEN - LG_AEAD_TAG_LEN;
        if (again < LG_MAX_NODES && L.peers[again].up && lora_frame_wants_ap_ack(inner, plain)) {
            L.peers[again].last_ms = now;
            queue_ack(again, lg_rd32(r->slot->data + 8));
        }
        return;   /* a replay is not a fault */
    }
    if (rc != LGBB_OPEN_OK) {
        L.seal_fail++;
        return;
    }
    L.frames_in++;
    L.heard_ms = now;
    L.heard_any = true;

    uint32_t acked_seq = 0;
    if (lora_ack_parse(inner, n, &acked_seq)) {
        /* D104: that AP has a frame of ours. Authenticated like any frame, and word from that AP. */
        if (L.peers[src].up) {
            L.peers[src].last_ms = now;
        }
        lock();
        bool waited = lora_txq_acked(&L.txq, acked_seq, (uint8_t)src);
        unlock();
        if (waited) {
            L.acks_in++;
        } else {
            L.acks_late++;
        }
        return;
    }

    lg_env_t e;
    if (lg_env_decode(inner, n, &e) != LG_OK) {
        L.seal_fail++;
        return;
    }
    if (e.type == LG_T_NODE_HELLO) {
        handle_hello(src, &e, lg_frame_body(inner), r->rssi, r->snr, boot, now);
        return;
    }
    if (!L.peers[src].up) {
        /*
         * The same rule backbone.c follows: an authenticated frame from a boot we have already
         * heard proves the link works both ways, so take it as confirmation rather than losing
         * the presence flood a returning AP sends the moment it comes up.
         */
        if (!L.peers[src].heard || L.peers[src].boot != boot) {
            return;
        }
        peer_up(src, r->rssi, r->snr);
    }
    /*
     * Any authenticated frame is word from that AP, not only its heartbeat. Without this a link that
     * had timed out came up on a data frame with the old `last_ms` still in place, and the sweep in
     * lora_poll() took it down again in the same pass: "Link up to AP 0" and "Link down to AP 0 (no
     * heartbeat)" at the same millisecond, three times in three seconds on Fuji on 2026-10-04, each
     * pair running the link-up catch-up and leaving the peer unusable for the frames behind it.
     */
    L.peers[src].last_ms = now;
    if (lora_frame_wants_ap_ack(inner, n)) {
        queue_ack(src, lg_rd32(r->slot->data + 8));   /* D104: before it is handled, so the answer is first in line */
    }
    if (L.on_frame != NULL) {
        /*
         * The number that says whether the radio is earning its keep: a frame the core had not
         * already seen. A copy Wi-Fi delivered first lands in lg_core's duplicate window, so the
         * duplicate counter standing still is exactly "this one arrived here over LoRa first".
         */
        uint32_t dup_before = g_app.core.stats.duplicates;
        L.relay_src = src;
        L.relaying = true;
        L.on_frame(src, inner, n);   /* exactly the path an ESP-NOW frame takes */
        L.relaying = false;
        if (g_app.core.stats.duplicates == dup_before) {
            L.frames_first++;
        }
    }
}

static void heartbeat(uint32_t now)
{
    if ((int32_t)(now - L.next_hb_ms) < 0) {
        return;
    }
    static uint8_t frame[LG_FRAME_MAX];   /* core task only */
    size_t len = lgbb_build_hello(frame, sizeof(frame));
    if (len == 0) {
        /* The backbone was not ready to say hello yet: ask again in half a second, not in a minute.
         * Waiting the whole period cost every boot its first hello (2026-10-08). */
        L.next_hb_ms = now + LORA_HEARTBEAT_RETRY_MS;
        return;
    }
    /*
     * A heartbeat is routine traffic, and since D102 a queue full of people's messages refuses routine
     * traffic. Refused, this used to wait a whole minute for its next turn: an AP carrying a busy
     * LoRa-only handheld missed three in a row and both its neighbours called the link dead while it
     * was working hardest (Fuji, 2026-10-05, for the three minutes it carried Channi). It asks first
     * now, and with no room comes back in half a second: the queue sends something every second or
     * two, so a place comes up, and the heartbeat is in it before the next message arrives.
     */
    lock();
    bool room = lora_txq_would_take(&L.txq, LORA_RANK_ROUTINE, len + LGBB_OUTER_LEN + LG_AEAD_TAG_LEN);
    unlock();
    if (!room) {
        L.next_hb_ms = now + LORA_HEARTBEAT_RETRY_MS;
        L.hb_waited++;
        return;
    }
    /*
     * Discovery runs fast: for two minutes after the module is configured, hellos go every ten
     * seconds instead of every minute. Links need a round trip of boot-confirmed hellos to come up,
     * so after all three APs restart together the mesh took up to six minutes at the slow cadence,
     * and anything LoRa was asked to carry before then was refused for having no peer (2026-10-08).
     */
    L.next_hb_ms = now + (now - L.configured_at_ms < LORA_DISCOVERY_MS ? LORA_HELLO_FAST_MS
                                                                       : LORA_HEARTBEAT_MS);
    if (L.broadcast_ok) {
        (void)queue_sealed(LORA_ADDR_BROADCAST, false, frame, len);
        return;
    }
    /* No broadcast address: one AP per heartbeat, in turn, so discovery still happens without
     * three APs transmitting the same thing every 30 s. */
    for (unsigned i = 0; i < LG_MAX_NODES; i++) {
        L.hb_round = (uint8_t)((L.hb_round + 1u) % LG_MAX_NODES);
        if (L.hb_round != L.index) {
            (void)queue_sealed((uint16_t)LORA_ADDR_AP(L.hb_round), false, frame, len);
            return;
        }
    }
}

void lora_poll(uint32_t now_ms)
{
    if (!L.started) {
        return;
    }
    if (L.off && (int32_t)(now_ms - L.off_until_ms) >= 0) {
        lora_enable();   /* restores itself even if whatever turned it off never came back */
    }
    for (;;) {
        lora_rx_t r;
        lock();
        bool have = L.rx_count > 0;
        if (have) {
            r = L.rx[L.rx_head];
            L.rx_head = (L.rx_head + 1u) % RX_RING;
            L.rx_count--;
        }
        unlock();
        if (!have) {
            break;
        }
        deliver(&r, now_ms);
        lora_asm_done(&L.asmb, r.slot);   /* the reassembly slot is free again */
    }
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (L.peers[i].up && now_ms - L.peers[i].last_ms > LORA_LINK_TIMEOUT_MS) {
            peer_down(i, "no heartbeat");
        }
    }
    if (L.fitted && !L.off) {
        heartbeat(now_ms);
    }
}

/* ---- reporting ---- */

void lora_traffic(lora_traffic_t *out)
{
    memset(out, 0, sizeof(*out));
    out->flags = (uint8_t)((L.fitted ? LORA_TF_FITTED : 0u) | (L.configured ? LORA_TF_CONFIGURED : 0u) |
                           (L.broadcast_ok ? LORA_TF_BROADCAST : 0u) | (L.off ? LORA_TF_OFF : 0u) |
                           (L.big ? LORA_TF_BIG : 0u));
    out->address = (uint8_t)L.address;
    out->networkid = L.networkid;
    out->rssi = L.rssi;
    out->snr = L.snr;
    out->frames_out = L.frames_out;
    out->frames_in = L.frames_in;
    out->frames_first = L.frames_first;
    out->parts_out = L.parts_out;
    out->parts_in = L.parts_in;
    out->parts_dropped = L.asmb.dropped + L.parts_dropped;
    out->reasm_timeouts = L.asmb.timeouts;
    out->seal_fail = L.seal_fail;
    out->refused_big = L.refused_big;
    lock();
    out->queue_depth = L.txq.depth;
    out->queue_high = L.txq.high;
    out->queue_dropped = L.txq.dropped;
    unlock();
    out->airtime_ms = L.airtime_ms;
    out->retries = L.retries;
    out->heard_age_ms = L.heard_any ? app_now_ms() - L.heard_ms : 0xFFFFFFFFu;
    for (uint16_t i = 0; i < LG_MAX_NODES && i < 8u; i++) {
        out->peers = (uint8_t)(out->peers | (L.peers[i].up ? 1u << i : 0u));
    }
    out->restarts = L.restarts;
}

size_t lora_peer_info(lora_peer_info_t *out, size_t max, uint32_t now_ms)
{
    size_t n = 0;
    for (uint16_t i = 0; i < LG_MAX_NODES && n < max; i++) {
        if (!L.peers[i].heard) {
            continue;
        }
        out[n].node = (uint8_t)i;
        out[n].up = L.peers[i].up;
        out[n].rssi = L.peers[i].rssi;
        out[n].snr = L.peers[i].snr;
        out[n].age_ms = now_ms - L.peers[i].last_ms;
        n++;
    }
    return n;
}

const char *lora_version(void)
{
    return L.version;
}

void lora_print(void)
{
    if (!L.started) {
        printf("LoRa: not started\n");
        return;
    }
    lora_traffic_t t;
    lora_traffic(&t);
    printf("LoRa (D71): %s%s, UART1 rx GPIO%d tx GPIO%d reset %s\n",
           !L.fitted ? (L.ever_fitted ? "module stopped answering" : "no module fitted")
                     : L.configured ? "fitted and configured" : "fitted, configuring",
           L.off ? " [OFF: chaos hook]" : "", L.pins.rx, L.pins.tx, L.pins.rst >= 0 ? "wired" : "not used");
    printf("  address %u, network %u, %lu Hz, SF%d BW%s CR4/%d, %d dBm, broadcast %s%s%s\n",
           (unsigned)L.address, L.networkid, (unsigned long)LORA_BAND_HZ, LORA_SF,
           LORA_BW_CODE == 7 ? "125" : LORA_BW_CODE == 8 ? "250" : "500", LORA_CR_CODE + 4, s_power_dbm,
           L.broadcast_ok ? "on" : "off", L.version[0] != '\0' ? ", " : "", L.version);
    if (t.heard_age_ms == 0xFFFFFFFFu) {
        printf("  nothing received yet\n");
    } else {
        printf("  last frame %" PRIu32 " ms ago, RSSI %d dBm, SNR %d\n", t.heard_age_ms, t.rssi, t.snr);
    }
    printf("  AP    STATE  RSSI  SNR  AGE_MS\n");
    bool any = false;
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (!L.peers[i].heard) {
            continue;
        }
        any = true;
        printf("  %-4u  %-5s  %4d  %3d  %6" PRIu32 "\n", i, L.peers[i].up ? "UP" : "DOWN", L.peers[i].rssi,
               L.peers[i].snr, app_now_ms() - L.peers[i].last_ms);
    }
    if (!any) {
        printf("  no peer heard yet\n");
    }
    printf("  frames out %" PRIu32 " in %" PRIu32 " (%" PRIu32 " of them arrived here first)\n",
           t.frames_out, t.frames_in, t.frames_first);
    printf("  parts out %" PRIu32 " in %" PRIu32 " dropped %" PRIu32 " | reassembly timeouts %" PRIu32
           " | too large %" PRIu32 "\n", t.parts_out, t.parts_in, t.parts_dropped, t.reasm_timeouts,
           t.refused_big);
    printf("  seal/open failures %" PRIu32 " | queue %u (high %u, dropped %" PRIu32 ") | retries %" PRIu32
           " | airtime %" PRIu32 " ms | module restarts %u\n", t.seal_fail, t.queue_depth, t.queue_high,
           t.queue_dropped, t.retries, t.airtime_ms, t.restarts);
    printf("  shared state and names that gave way to busier traffic: %" PRIu32 "\n", L.gave_way);
    printf("  floods not repeated because their sender reaches every peer here: %" PRIu32 "\n", L.relays_spared);
    printf("  heartbeats that waited for room in the queue: %" PRIu32 "\n", L.hb_waited);
    printf("  module lines that parsed as nothing: %" PRIu32 "\n", s_at_noise);
    printf("  sends held while another's frame was mid-air: %" PRIu32 "\n", L.held_for_air);
    printf("  messages taken over from ESP-NOW when it could not deliver them: %" PRIu32 "\n", L.took_over);
    printf("  messages the queue had no room for (the rest of its drops were routine): %" PRIu32 "\n",
           L.txq.dropped_messages);
    printf("  acknowledged by every AP they were for: %" PRIu32 " | sent again: %" PRIu32 " | given up: %" PRIu32
           " | acknowledgements sent %" PRIu32 ", received %" PRIu32 " (%" PRIu32 " more for nothing waiting)\n",
           L.txq.acked, L.txq.resent, L.txq.unacked, L.acks_out, L.acks_in, L.acks_late);
    printf("  one full part is about %" PRIu32 " ms on the air; it can take a %u-byte payload%s;"
           " live voice never comes this way\n", lora_airtime_ms(LORA_PART_B64_MAX),
           (unsigned)lora_txq_capacity(&L.txq),
           L.big ? "" : " (no room for a long payload to be sent from here; it can still receive one)");
}

/* ---- start-up ---- */

/*
 * Every frame buffer this AP will ever use, taken once, the first time a module answers. Nothing
 * is freed and nothing is ever allocated again, so the heap cannot fragment under traffic.
 */
static bool take_buffers(void)
{
    if (L.buffers) {
        return true;
    }
    size_t small = (SMALL_ASM_SLOTS + SMALL_TXQ_SLOTS) * LORA_SMALL_MAX;
    uint8_t *pool = heap_caps_malloc(small, MALLOC_CAP_8BIT);
    if (pool == NULL) {
        ESP_LOGE(TAG, "[LORA] No room for its buffers (%u bytes); the radio stays off and this AP "
                      "carries on with one backbone", (unsigned)small);
        return false;
    }
    /* The long-payload slots are for D72's voice notes, which nothing sends yet. Measured on the
     * bench 2026-09-20: taking both left NORTH with 9.7 KB free and a 7.8 KB low-water mark, when
     * the admin link alone wants HEAP_FLOOR. So a long slot is taken only where the AP can still
     * spare HEAP_FLOOR afterwards, receive slot included; without it a long payload is refused at
     * its first part, logged and counted, and everything the grid sends today still fits. */
    uint8_t *rx_big = NULL;
    if (LORA_LONG_PAYLOAD && esp_get_free_heap_size() > HEAP_FLOOR + LORA_PAYLOAD_MAX) {
        rx_big = heap_caps_malloc(LORA_PAYLOAD_MAX, MALLOC_CAP_8BIT);
    }
    for (size_t i = 0; i < SMALL_ASM_SLOTS; i++) {
        lora_asm_set_slot(&L.asmb, i, pool + i * LORA_SMALL_MAX, LORA_SMALL_MAX);
    }
    for (size_t i = 0; i < SMALL_TXQ_SLOTS; i++) {
        lora_txq_set_slot(&L.txq, i, pool + (SMALL_ASM_SLOTS + i) * LORA_SMALL_MAX, LORA_SMALL_MAX);
    }
    if (rx_big != NULL) {
        lora_asm_set_slot(&L.asmb, SMALL_ASM_SLOTS, rx_big, LORA_PAYLOAD_MAX);
    }
    /* The send slot for a long payload only where the heap can still spare it afterwards. Nothing
     * produces one yet, and receiving one is what an AP cannot refuse on a peer's behalf. */
    uint8_t *tx_big = NULL;
    if (LORA_LONG_PAYLOAD && esp_get_free_heap_size() > HEAP_FLOOR + LORA_PAYLOAD_MAX) {
        tx_big = heap_caps_malloc(LORA_PAYLOAD_MAX, MALLOC_CAP_8BIT);
    }
    if (tx_big != NULL) {
        lora_txq_set_slot(&L.txq, SMALL_TXQ_SLOTS, tx_big, LORA_PAYLOAD_MAX);
        L.big = true;
    }
    L.buffers = true;
    ESP_LOGI(TAG, "[LORA] Buffers taken: %u bytes; it can receive a %u-byte payload and send up to %u",
             (unsigned)(small + (rx_big != NULL ? LORA_PAYLOAD_MAX : 0u)
                        + (tx_big != NULL ? LORA_PAYLOAD_MAX : 0u)),
             (unsigned)(rx_big != NULL ? LORA_PAYLOAD_MAX : LORA_SMALL_MAX),
             (unsigned)(tx_big != NULL ? LORA_PAYLOAD_MAX : LORA_SMALL_MAX));
    return true;
}

esp_err_t lora_start(uint16_t ap_index, const uint8_t discriminator[4], lgbb_frame_cb_t on_frame,
                     lgbb_link_cb_t on_link)
{
    /*
     * D77: the handheld side of the radio, keyed separately from the backbone. Its sender identity is
     * this AP, so a handheld can tell which AP answered it, and its boot counter is the AP's own,
     * already committed to NVS before any transmit, so a nonce never repeats.
     */
    lglh_init(&L.hh, s_lora_hh_key, LGLH_SENDER_AP(ap_index), g_app.boot);
    if (L.started) {
        return ESP_ERR_INVALID_STATE;
    }
    power_load();   /* D98: before the module is configured */
    if (ap_index >= LG_MAX_NODES) {
        ESP_LOGW(TAG, "[LORA] AP index %u has no LoRa address; not started", ap_index);
        return ESP_ERR_INVALID_ARG;
    }
    const lora_pins_t main_pins = LORA_PINS_MAIN;
    const lora_pins_t other_pins = LORA_PINS_OTHER;
    L.pins = ap_index == 0 ? main_pins : other_pins;
    L.index = (uint8_t)ap_index;
    L.address = (uint16_t)LORA_ADDR_AP(ap_index);
    /* AT+NETWORKID takes 3..15 (18 is its default). One byte of the grid's discriminator picks
     * one, so another LocalGrid built from other secrets is ignored by the module itself. */
    L.networkid = (uint8_t)(3u + (discriminator[0] % 13u));
    L.broadcast_ok = true;
    L.on_frame = on_frame;
    L.on_link = on_link;
    lora_txq_init(&L.txq);   /* no slot has memory until a module answers */
    lora_asm_init(&L.asmb);
    L.next_hb_ms = app_now_ms() + (uint32_t)ap_index * LORA_HB_STAGGER_MS;

    L.lock = xSemaphoreCreateMutex();
    if (L.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uart_config_t cfg = {
        .baud_rate = LORA_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LORA_UART, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(LORA_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(LORA_UART, L.pins.tx, L.pins.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = gpio_set_pull_mode((gpio_num_t)L.pins.rx, GPIO_PULLUP_ONLY);   /* nothing fitted reads as silence */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LORA] Serial port not opened: %s", esp_err_to_name(err));
        return err;
    }
    /*
     * The task looks for a module on its first pass, not here: start-up must not wait on a radio
     * that may not exist, so an AP with no module boots in exactly the time it boots in today.
     */
    if (xTaskCreate(lora_task, "lora", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    L.started = true;
    lgbb_set_extra_peers(extra_peers);
    lgbb_set_fallback(take_over);
    return ESP_OK;
}
