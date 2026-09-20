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
#include "lora_wire.h"
#include "node_app.h"

static const char *TAG = "LORA";

#define LORA_UART         UART_NUM_1
#define UART_RX_BUF       1024
#define UART_TX_BUF       512
#define LORA_LINE_MAX     320u      /* "+RCV=" plus 180 characters of data plus its numbers */
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
} lora_peer_t;

static struct {
    uint8_t     index;
    uint16_t    address;
    uint8_t     networkid;
    lora_pins_t pins;
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
        if (!lora_at_parse(line, &at)) {
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
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", LORA_POWER_DBM);
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
             L.version[0] != '\0' ? L.version : "module", addr, net, par, (uint32_t)LORA_BAND_HZ, LORA_POWER_DBM);
    return true;
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
        return;
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
    lora_txq_slot_t *slot = lora_txq_peek(&L.txq, app_now_ms());
    if (slot == NULL) {
        unlock();
        return false;
    }
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
        lora_txq_release(&L.txq, slot);
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
            if (lora_at_parse(line, &at)) {
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
static bool queue_sealed(uint16_t dest, bool alert, const uint8_t *frame, size_t len)
{
    size_t need = len + LGBB_OUTER_LEN + LG_AEAD_TAG_LEN;
    lock();
    lora_txq_slot_t *slot = lora_txq_reserve(&L.txq, dest, alert, L.msg_id, need,
                                             app_now_ms() + rand_ms(BACKOFF_MAX_MS));
    int n = slot != NULL ? lgbb_seal(slot->data, slot->cap, frame, len) : -1;
    bool ok = n > 0 && lora_txq_commit(&L.txq, slot, (size_t)n);
    if (slot != NULL && !ok) {
        lora_txq_release(&L.txq, slot);
    }
    if (ok) {
        L.msg_id++;
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
    if (policy == LORA_SEND_IF_WIFI_DOWN && wifi_ok && !force) {
        return;   /* ESP-NOW is carrying it: keep the air clear */
    }
    if (node >= LG_MAX_NODES || (!L.peers[node].up && !force)) {
        return;   /* no LoRa link to that AP: transmitting at it would be wasted airtime */
    }
    L.force_once = false;
    (void)queue_sealed((uint16_t)LORA_ADDR_AP(node), policy == LORA_SEND_ALWAYS, frame, len);
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
    bool needed = policy == LORA_SEND_ALWAYS || force;
    for (uint16_t i = 0; i < LG_MAX_NODES && !needed; i++) {
        if (L.peers[i].up && i != except_node && !lgbb_link_acked(i, app_now_ms())) {
            needed = true;
        }
    }
    if (!needed) {
        return;
    }
    L.force_once = false;
    bool alert = policy == LORA_SEND_ALWAYS;
    if (L.broadcast_ok && reachable > 1u) {
        (void)queue_sealed(LORA_ADDR_BROADCAST, alert, frame, len);
        return;
    }
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (L.peers[i].up && i != except_node) {
            (void)queue_sealed((uint16_t)LORA_ADDR_AP(i), alert, frame, len);
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
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *q = body + LG_HELLO_LEN + 1u + i * 6u;
        if (lg_rd16(q) == L.index && lg_rd32(q + 2) == g_app.boot) {
            peer_up(src, rssi, snr);
        }
    }
}

static void deliver(const lora_rx_t *r, uint32_t now)
{
    static uint8_t inner[LG_FRAME_MAX];   /* core task only */
    uint16_t src = 0;
    uint32_t boot = 0;
    size_t n = 0;
    lgbb_open_t rc = lgbb_open(r->slot->data, r->slot->total, &src, &boot, inner, sizeof(inner), &n);
    if (rc != LGBB_OPEN_OK) {
        if (rc != LGBB_OPEN_REPLAY) {
            L.seal_fail++;   /* a replay is the two backbones agreeing, not a fault */
        }
        return;
    }
    L.frames_in++;
    L.heard_ms = now;
    L.heard_any = true;

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
    if (L.on_frame != NULL) {
        /*
         * The number that says whether the radio is earning its keep: a frame the core had not
         * already seen. A copy Wi-Fi delivered first lands in lg_core's duplicate window, so the
         * duplicate counter standing still is exactly "this one arrived here over LoRa first".
         */
        uint32_t dup_before = g_app.core.stats.duplicates;
        L.on_frame(src, inner, n);   /* exactly the path an ESP-NOW frame takes */
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
    L.next_hb_ms = now + LORA_HEARTBEAT_MS;
    static uint8_t frame[LG_FRAME_MAX];   /* core task only */
    size_t len = lgbb_build_hello(frame, sizeof(frame));
    if (len == 0) {
        return;
    }
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
           LORA_BW_CODE == 7 ? "125" : LORA_BW_CODE == 8 ? "250" : "500", LORA_CR_CODE + 4, LORA_POWER_DBM,
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
    if (L.started) {
        return ESP_ERR_INVALID_STATE;
    }
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
    return ESP_OK;
}
