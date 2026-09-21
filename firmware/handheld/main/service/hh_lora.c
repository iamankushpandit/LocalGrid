/* hh_lora.c - the handheld's LoRa module (D71, D76). See hh_lora.h and docs/lora.md. */
#include "hh_lora.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_board.h"
#include "lg_envelope.h"
#include "lora_radio.h"
#include "lora_seal.h"
#include "lora_wire.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "LORA";

/*
 * The serial port. hh_gps.c takes UART0 where the console is USB-Serial-JTAG and UART1 where the
 * console is UART0, so this takes whichever of the remaining ports exists. Only the FNK0104B has
 * LoRa pins today and its console is USB-Serial-JTAG, so it lands on UART1 with the GPS on UART0.
 */
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART_NUM == 0
#if SOC_UART_NUM > 2
#define LORA_UART        UART_NUM_2
#define LORA_UART_SHARED 0
#else
/* Only two ports, and the console and the GPS hold both: the radio cannot have one. */
#define LORA_UART        UART_NUM_1
#define LORA_UART_SHARED 1
#endif
#else
#define LORA_UART        UART_NUM_1
#define LORA_UART_SHARED 0
#endif

#define UART_RX_BUF       1024
#define UART_TX_BUF       512
#define LORA_LINE_MAX     320u      /* "+RCV=" plus 180 characters of data plus its numbers */
#define TASK_STACK        4096
#define TASK_PRIORITY     3         /* below the service task (5): airtime can wait */
#define AT_TIMEOUT_MS     1000u
#define SEND_TIMEOUT_MS   8000u     /* one full part is about a second of airtime, plus the module */
#define DETECT_TRIES      3u
#define PART_TRIES        3u
#define BACKOFF_MAX_MS    300u      /* random wait before every transmission: no carrier sense */
#define RETRY_MIN_MS      800u
#define RETRY_SPAN_MS     2200u
#define RX_RING           2u
#define WEDGE_FAILS       5u
#define WEDGE_BACKOFF_MS  2000u
#define WEDGE_BACKOFF_MAX 60000u
#define IDLE_WAIT_MS      50u       /* how long the task listens for a line when it has work to do */
#define QUIET_WAIT_MS     1000u     /* ... and with no module: purely passive, and costs nothing */
#define OFF_MAX_S         3600u     /* the console hook can never take the radio out for a day */

/*
 * Memory, taken ONCE when a module has answered and never again. A handheld with no module holds
 * no frame buffers at all, which is the first thing this file has to protect.
 *
 *   2 reassembly slots   512 B each   what an AP sends back: an ack, a pong, a short text
 *   3 send slots         512 B each   an SOS, a position and one more waiting behind them
 *
 * 2.5 KB. There are no 6 KB slots here: a handheld neither sends nor receives a voice note over
 * LoRa (D72 voice notes travel between APs), and a payload larger than a slot is refused at its
 * first part, logged and counted, exactly as on an AP.
 */
#define HH_ASM_SLOTS  2u
#define HH_TXQ_SLOTS  3u

_Static_assert(HH_ASM_SLOTS <= LORA_ASM_SLOTS, "reassembly slots fit");
_Static_assert(HH_TXQ_SLOTS <= LORA_TXQ_SLOTS, "send slots fit");

/* What crosses from the LoRa task to the service task: which reassembly slot holds the payload. */
typedef struct {
    lora_asm_slot_t *slot;
    int8_t           rssi;
    int8_t           snr;
} rx_item_t;

/*
 * The rate limit. A frame is named by (type, origin_seq): the outbox retransmits the same identity
 * every few seconds, and one transmission is seconds of airtime on a cell. Eight entries is more
 * distinct messages than a handheld can have in flight, and the oldest is reused when they are all
 * taken, which costs at worst one extra transmission.
 */
#define SEEN_SLOTS 8u

typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint32_t at_ms;
} seen_t;

static struct {
    bool        started;
    bool        buffers;
    int8_t      rx_gpio, tx_gpio, rst_gpio;
    uint32_t    device;
    uint16_t    address;
    uint8_t     networkid;
    char        version[28];

    volatile bool fitted;
    volatile bool configured;
    volatile bool ever_fitted;
    volatile bool broadcast_ok;
    volatile bool reset_wanted;
    volatile bool at_pending;
    char          at_text[64];

    volatile bool     off;
    volatile uint32_t off_until_ms;
    bool              force_once;

    SemaphoreHandle_t lock;       /* guards txq, the rx ring and msg_id */
    lora_txq_t        txq;
    rx_item_t         rx[RX_RING];
    size_t            rx_head, rx_count;
    uint8_t           msg_id;

    lora_asm_t  asmb;             /* LoRa task only */
    lora_seal_t seal;             /* service task only: it seals, and it opens */

    /* service task only */
    hh_lora_frame_cb_t on_frame;
    seen_t             seen[SEEN_SLOTS];
    uint16_t           ap_addr;       /* the AP last heard from, by its LoRa address */
    uint32_t           heard_ms;
    bool               heard_any;
    bool               link;

    uint32_t frames_out, frames_in, parts_out, parts_in, parts_dropped;
    uint32_t seal_fail, held_back, airtime_ms, retries, refused_big;
    uint8_t  restarts;
    int8_t   rssi, snr;
} L;

static void lock(void)   { if (L.lock != NULL) { xSemaphoreTake(L.lock, portMAX_DELAY); } }
static void unlock(void) { if (L.lock != NULL) { xSemaphoreGive(L.lock); } }

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t rand_ms(uint32_t span)
{
    return span == 0 ? 0u : esp_random() % span;
}

/* ---- the serial line ---- */

static char   s_line[LORA_LINE_MAX];
static size_t s_line_len;

static bool read_line(uint32_t timeout_ms, char **out)
{
    uint32_t start = now_ms();
    for (;;) {
        uint32_t spent = now_ms() - start;
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

/* Waits for one kind of reply. A frame arriving while we wait is never lost. */
static bool at_wait(lora_at_kind_t want, uint32_t timeout_ms, char *value, size_t vcap, int *err)
{
    uint32_t start = now_ms();
    for (;;) {
        uint32_t spent = now_ms() - start;
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

static bool at_value(const char *cmd, char *value, size_t cap)
{
    at_write(cmd);
    return at_wait(LORA_AT_VALUE, AT_TIMEOUT_MS, value, cap, NULL);
}

/* ---- the module ---- */

static void module_reset(void)
{
    if (L.rst_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (unsigned)L.rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        (void)gpio_config(&io);
        (void)gpio_set_level((gpio_num_t)L.rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        (void)gpio_set_level((gpio_num_t)L.rst_gpio, 1);
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

/* Exactly the AP's configuration, with this handheld's address (docs/lora.md). */
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
    char addr[16] = "?", net[16] = "?", par[32] = "?";
    (void)at_value("AT+ADDRESS?", addr, sizeof(addr));
    (void)at_value("AT+NETWORKID?", net, sizeof(net));
    (void)at_value("AT+PARAMETER?", par, sizeof(par));
    ESP_LOGI(TAG, "[LORA] Configured %s: address %s, network %s, parameters %s, %" PRIu32 " Hz, %d dBm",
             L.version[0] != '\0' ? L.version : "module", addr, net, par, (uint32_t)LORA_BAND_HZ,
             LORA_POWER_DBM);
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
        L.parts_dropped++;
        return;
    }
    size_t n = lora_b64_decode(at->data, at->len, raw, sizeof(raw));
    if (n == 0) {
        L.parts_dropped++;
        return;
    }
    lora_part_t p;
    if (!lora_part_parse(raw, n, &p) || LORA_PEER_KIND(p.peer) != LORA_PEER_AP) {
        /*
         * Another handheld's parts are on the same air and are not ours to reassemble: a handheld
         * talks to APs and to nothing else. Counted, never buffered.
         */
        L.parts_dropped++;
        return;
    }
    lora_asm_slot_t *slot = NULL;
    lora_asm_result_t r = lora_asm_feed(&L.asmb, now_ms(), raw, n, &slot);
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
        L.parts_dropped++;
        return;
    }
    rx_item_t *e = &L.rx[(L.rx_head + L.rx_count) % RX_RING];
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

static bool send_one(uint16_t dest, uint8_t id, const uint8_t *payload, size_t len, size_t part, size_t of)
{
    static uint8_t raw[LORA_PART_RAW_MAX];
    static char    b64[LORA_PART_B64_MAX + 1];

    size_t off = part * LORA_SLICE_MAX;
    size_t slice = len - off > LORA_SLICE_MAX ? LORA_SLICE_MAX : len - off;
    size_t plen = lora_part_build((uint8_t)(LORA_PEER_HANDHELD | (L.device & 0x0Fu)), id, (uint8_t)part,
                                  (uint8_t)of, payload + off, slice, raw, sizeof(raw));
    size_t blen = plen != 0 ? lora_b64_encode(raw, plen, b64, sizeof(b64) - 1u) : 0u;
    if (blen == 0) {
        L.seal_fail++;
        return false;
    }
    b64[blen] = '\0';
    for (unsigned attempt = 0; attempt < PART_TRIES; attempt++) {
        /* No carrier sense in this module, and every AP and handheld shares one channel. */
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? rand_ms(BACKOFF_MAX_MS)
                                              : RETRY_MIN_MS + rand_ms(RETRY_SPAN_MS)));
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
    uint32_t started = now_ms();
    while (now_ms() - started < AT_TIMEOUT_MS * 2u && read_line(AT_TIMEOUT_MS, &line)) {
        lora_at_t at;
        if (lora_at_parse(line, &at) && at.kind == LORA_AT_RCV) {
            handle_rcv(&at);
            continue;
        }
        ESP_LOGI(TAG, "[LORA] < %s", line);
    }
}

/* One part of one payload, chosen fresh each time round, so an alert waits at most one part. */
static bool transmit_a_part(unsigned *fails)
{
    uint16_t dest;
    uint8_t  id;
    size_t   len, part, of;

    lock();
    lora_txq_slot_t *slot = lora_txq_peek(&L.txq, now_ms());
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

    /* `sending` keeps the queue from evicting or reusing the slot, so reading its bytes outside
     * the lock is safe. Only this task ever transmits. */
    bool ok = send_one(dest, id, data, len, part, of);

    lock();
    if (!slot->in_use || slot->id != id) {
        unlock();   /* released under us (the module was reset): nothing to finish */
        return true;
    }
    if (!ok) {
        lora_txq_release(&L.txq, slot);
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
         * never logs and never times anything: a handheld with no module is exactly a handheld
         * built before this file. If one is plugged in later it prints +READY as it powers up, or
         * answers the one AT we then send, and it is picked up without a restart.
         */
        if (!L.fitted) {
            char *line = NULL;
            if (!first_look && !read_line(QUIET_WAIT_MS, &line)) {
                continue;
            }
            first_look = false;
            if (!detect()) {
                if (!said_missing) {
                    said_missing = true;
                    ESP_LOGI(TAG, "[LORA] No module on GPIO%d/%d; this handheld uses Wi-Fi only, "
                                  "exactly as before", L.rx_gpio, L.tx_gpio);
                }
                continue;
            }
            if (!take_buffers()) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            ESP_LOGI(TAG, "[LORA] Module on rx GPIO%d, tx GPIO%d, address %u, network %u",
                     L.rx_gpio, L.tx_gpio, (unsigned)L.address, L.networkid);
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
            if ((int32_t)(now_ms() - next_retry) < 0) {
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
                ESP_LOGW(TAG, "[LORA] The module stopped answering; this handheld carries on over Wi-Fi");
                continue;
            }
            next_retry = now_ms() + backoff;
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
        (void)lora_asm_expire(&L.asmb, now_ms());

        if (L.off) {
            drop_queue();
            continue;
        }
        (void)transmit_a_part(&fails);
    }
}

/* ---- the service task's side ---- */

/*
 * The rate limit (hh_lora.h). Returns true when this frame may cost airtime now. A frame with a
 * message identity is named by (type, seq); a keepalive or a position, which the sender repeats
 * on its own timer, is limited by its type alone.
 */
static bool may_spend_airtime(uint8_t type, uint32_t seq, bool routine, uint32_t now)
{
    uint32_t wait = routine ? HH_LORA_ROUTINE_MS : HH_LORA_REOFFER_MS;
    seen_t *oldest = &L.seen[0];
    for (size_t i = 0; i < SEEN_SLOTS; i++) {
        seen_t *e = &L.seen[i];
        if (e->at_ms != 0 && e->type == type && (routine || e->seq == seq)) {
            if (now - e->at_ms < wait) {
                return false;
            }
            e->seq = seq;
            e->at_ms = now == 0 ? 1u : now;
            return true;
        }
        if (e->at_ms == 0) {
            oldest = e;
            break;
        }
        if ((int32_t)(e->at_ms - oldest->at_ms) < 0) {
            oldest = e;
        }
    }
    oldest->type = type;
    oldest->seq = seq;
    oldest->at_ms = now == 0 ? 1u : now;
    return true;
}

bool hh_lora_offer(const uint8_t *frame, size_t len, bool wifi_ok)
{
    if (!L.started || !L.fitted || L.off) {
        return false;
    }
    lora_policy_t policy = lora_policy_for_handheld_frame(frame, len);
    bool force = L.force_once;
    if (policy == LORA_SEND_NEVER) {
        return false;   /* live voice and everything else a handheld never puts on the air */
    }
    if (policy != LORA_SEND_ALWAYS && wifi_ok && !force) {
        return false;   /* D74: Wi-Fi is carrying it, so the radio stays quiet */
    }
    lg_env_t e;
    if (lg_env_decode(frame, len, &e) != LG_OK) {
        return false;
    }
    uint32_t now = now_ms();
    bool routine = e.type == LG_T_PING || e.type == LG_T_POSITION;
    if (!force && !may_spend_airtime((uint8_t)e.type, e.origin_seq, routine, now)) {
        L.held_back++;
        return false;
    }
    L.force_once = false;

    /*
     * Sealed straight into the queue slot that will carry it: nothing is staged in a second
     * buffer, so the largest frame this handheld can send is simply the largest slot it has.
     */
    size_t need = len + LORA_SEAL_OUTER_LEN + LG_AEAD_TAG_LEN;
    uint16_t dest = L.broadcast_ok || L.ap_addr == 0 ? (uint16_t)LORA_ADDR_BROADCAST : L.ap_addr;
    lock();
    lora_txq_slot_t *slot = lora_txq_reserve(&L.txq, dest, policy == LORA_SEND_ALWAYS, L.msg_id, need,
                                             now + rand_ms(BACKOFF_MAX_MS));
    int n = slot != NULL ? lora_seal_frame(&L.seal, slot->data, slot->cap, frame, len) : -1;
    bool ok = n > 0 && lora_txq_commit(&L.txq, slot, (size_t)n);
    if (slot != NULL && !ok) {
        lora_txq_release(&L.txq, slot);
    }
    if (ok) {
        L.msg_id++;
    }
    unlock();
    if (slot == NULL) {
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

static void deliver(const rx_item_t *r, uint32_t now)
{
    static uint8_t inner[LG_FRAME_MAX];   /* service task only */
    uint16_t src = 0;
    uint32_t boot = 0;
    size_t n = 0;
    lora_seal_result_t rc = lora_seal_open(&L.seal, r->slot->data, r->slot->total, &src, &boot, inner,
                                           sizeof(inner), &n);
    if (rc != LORA_SEAL_OK) {
        if (rc != LORA_SEAL_REPLAY) {
            L.seal_fail++;
        }
        return;
    }
    if (!lora_seal_is_ap(src)) {
        L.seal_fail++;   /* another handheld holds this key; it is not an AP and cannot act as one */
        return;
    }
    L.frames_in++;
    L.heard_ms = now == 0 ? 1u : now;
    L.heard_any = true;
    L.ap_addr = (uint16_t)LORA_ADDR_AP(src & 0x0Fu);
    if (!L.link) {
        L.link = true;
        ESP_LOGI(TAG, "[LORA] Link up to AP %u, RSSI %d dBm, SNR %d", (unsigned)(src & 0x0Fu), r->rssi,
                 r->snr);
    }
    /*
     * From here it is the frame an AP would have sent over Wi-Fi, and it takes exactly the path a
     * Wi-Fi frame takes: lg_client sorts out what it is. Frame contents are never logged (D21).
     */
    if (L.on_frame != NULL) {
        L.on_frame(inner, n);
    }
}

void hh_lora_poll(uint32_t now)
{
    if (!L.started) {
        return;
    }
    if (L.off && (int32_t)(now - L.off_until_ms) >= 0) {
        hh_lora_enable();   /* restores itself even if whatever turned it off never came back */
    }
    for (;;) {
        rx_item_t r;
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
        deliver(&r, now);
        lora_asm_done(&L.asmb, r.slot);
    }
    if (L.link && L.heard_any && now - L.heard_ms > HH_LORA_LINK_TIMEOUT_MS) {
        L.link = false;
        ESP_LOGW(TAG, "[LORA] Link down: no AP heard for %u s", (unsigned)(HH_LORA_LINK_TIMEOUT_MS / 1000u));
    }
}

bool hh_lora_fitted(void)
{
    return L.fitted;
}

/* ---- reporting ---- */

void hh_lora_info(hh_lora_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rx_gpio = L.started ? L.rx_gpio : (int8_t)LG_PIN_NONE;
    out->tx_gpio = L.started ? L.tx_gpio : (int8_t)LG_PIN_NONE;
    out->rst_gpio = L.started ? L.rst_gpio : (int8_t)LG_PIN_NONE;
    if (!L.started) {
        out->heard_age_ms = 0xFFFFFFFFu;
        return;
    }
    out->flags = (uint8_t)(HH_LORA_F_STARTED | (L.fitted ? HH_LORA_F_FITTED : 0u) |
                           (L.configured ? HH_LORA_F_CONFIGURED : 0u) |
                           (L.broadcast_ok ? HH_LORA_F_BROADCAST : 0u) | (L.off ? HH_LORA_F_OFF : 0u) |
                           (L.ever_fitted ? HH_LORA_F_EVER : 0u) | (L.link ? HH_LORA_F_LINK : 0u));
    out->address = (uint8_t)L.address;
    out->networkid = L.networkid;
    out->ap = (uint8_t)L.ap_addr;
    out->rssi = L.rssi;
    out->snr = L.snr;
    out->frames_out = L.frames_out;
    out->frames_in = L.frames_in;
    out->parts_out = L.parts_out;
    out->parts_in = L.parts_in;
    out->parts_dropped = L.asmb.dropped + L.parts_dropped;
    out->seal_fail = L.seal_fail + L.seal.auth_fail;
    out->held_back = L.held_back;
    lock();
    out->queue_dropped = L.txq.dropped;
    unlock();
    out->airtime_ms = L.airtime_ms;
    out->retries = L.retries;
    out->heard_age_ms = L.heard_any ? now_ms() - L.heard_ms : 0xFFFFFFFFu;
    out->restarts = L.restarts;
    snprintf(out->version, sizeof(out->version), "%s", L.version);
}

void hh_lora_print(void)
{
    hh_lora_info_t t;
    hh_lora_info(&t);
    if ((t.flags & HH_LORA_F_STARTED) == 0) {
        printf("LoRa (D71): not started; this board's profile names no LoRa pins\n");
        return;
    }
    printf("LoRa (D71): %s%s, rx GPIO%d tx GPIO%d reset %s\n",
           (t.flags & HH_LORA_F_FITTED) == 0
               ? ((t.flags & HH_LORA_F_EVER) != 0 ? "module stopped answering" : "no module fitted")
               : (t.flags & HH_LORA_F_CONFIGURED) != 0 ? "fitted and configured" : "fitted, configuring",
           (t.flags & HH_LORA_F_OFF) != 0 ? " [OFF: console hook]" : "", t.rx_gpio, t.tx_gpio,
           t.rst_gpio >= 0 ? "wired" : "not used");
    printf("  address %u, network %u, %lu Hz, SF%d BW%s CR4/%d, %d dBm, broadcast %s%s%s\n",
           (unsigned)t.address, t.networkid, (unsigned long)LORA_BAND_HZ, LORA_SF,
           LORA_BW_CODE == 7 ? "125" : LORA_BW_CODE == 8 ? "250" : "500", LORA_CR_CODE + 4,
           LORA_POWER_DBM, (t.flags & HH_LORA_F_BROADCAST) != 0 ? "on" : "off",
           t.version[0] != '\0' ? ", " : "", t.version);
    if (t.heard_age_ms == 0xFFFFFFFFu) {
        printf("  no AP heard yet\n");
    } else {
        printf("  AP %u, link %s, last part %" PRIu32 " ms ago, RSSI %d dBm, SNR %d\n", t.ap,
               (t.flags & HH_LORA_F_LINK) != 0 ? "up" : "down", t.heard_age_ms, t.rssi, t.snr);
    }
    printf("  frames out %" PRIu32 " in %" PRIu32 " | parts out %" PRIu32 " in %" PRIu32 " dropped %"
           PRIu32 "\n", t.frames_out, t.frames_in, t.parts_out, t.parts_in, t.parts_dropped);
    printf("  seal/open failures %" PRIu32 " | queue dropped %" PRIu32 " | retries %" PRIu32
           " | airtime %" PRIu32 " ms | module restarts %u\n", t.seal_fail, t.queue_dropped, t.retries,
           t.airtime_ms, t.restarts);
    printf("  offers held back to save the battery and the air: %" PRIu32 "\n", t.held_back);
    printf("  one full part is about %" PRIu32 " ms on the air; it carries position, presence, an "
           "SOS or urgent broadcast, short text and their acknowledgements, and nothing else\n",
           lora_airtime_ms(LORA_PART_B64_MAX));
}

void hh_lora_request_reset(void)
{
    L.reset_wanted = true;
}

void hh_lora_request_at(const char *command)
{
    if (!L.started || command == NULL) {
        return;
    }
    lock();
    snprintf(L.at_text, sizeof(L.at_text), "%s", command);
    L.at_pending = true;
    unlock();
}

void hh_lora_set_broadcast(bool on)
{
    L.broadcast_ok = on;
}

void hh_lora_force_next(void)
{
    L.force_once = true;
}

void hh_lora_disable(uint32_t seconds)
{
    uint32_t s = seconds == 0 ? 1u : seconds > OFF_MAX_S ? OFF_MAX_S : seconds;
    L.off_until_ms = now_ms() + s * 1000u;
    L.off = true;
    ESP_LOGW(TAG, "[LORA] Off for %" PRIu32 " s (console hook); it comes back on its own", s);
}

void hh_lora_enable(void)
{
    if (L.off) {
        L.off = false;
        ESP_LOGW(TAG, "[LORA] Back on");
    }
}

bool hh_lora_is_off(void)
{
    return L.off;
}

/* ---- start-up ---- */

/* Every frame buffer this handheld will ever use, taken once, the first time a module answers. */
static bool take_buffers(void)
{
    if (L.buffers) {
        return true;
    }
    size_t total = (HH_ASM_SLOTS + HH_TXQ_SLOTS) * LORA_SMALL_MAX;
    uint8_t *pool = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    if (pool == NULL) {
        ESP_LOGE(TAG, "[LORA] No room for its buffers (%u bytes); the radio stays off and this "
                      "handheld carries on over Wi-Fi", (unsigned)total);
        return false;
    }
    for (size_t i = 0; i < HH_ASM_SLOTS; i++) {
        lora_asm_set_slot(&L.asmb, i, pool + i * LORA_SMALL_MAX, LORA_SMALL_MAX);
    }
    for (size_t i = 0; i < HH_TXQ_SLOTS; i++) {
        lora_txq_set_slot(&L.txq, i, pool + (HH_ASM_SLOTS + i) * LORA_SMALL_MAX, LORA_SMALL_MAX);
    }
    L.buffers = true;
    ESP_LOGI(TAG, "[LORA] Buffers taken: %u bytes, for frames up to %u", (unsigned)total,
             (unsigned)LORA_SMALL_MAX);
    return true;
}

esp_err_t hh_lora_start(const hh_lora_cfg_t *cfg)
{
    if (L.started || cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->rx_gpio < 0 || cfg->tx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;   /* no connector on this board: nothing is started at all */
    }
    if (cfg->device == 0 || cfg->device > LORA_PEER_INDEX_MAX) {
        /* The peer index is a nibble (docs/lora.md). Said once, and then this handheld is simply
         * a handheld without a radio. */
        ESP_LOGW(TAG, "[LORA] Device %" PRIu32 " is above the %u the part header carries; the radio "
                      "is not started", cfg->device, (unsigned)LORA_PEER_INDEX_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (LORA_UART_SHARED) {
        ESP_LOGW(TAG, "[LORA] This chip has two serial ports and the console and the GPS hold both; "
                      "the radio is not started");
        return ESP_ERR_NOT_SUPPORTED;
    }
    L.rx_gpio = (int8_t)cfg->rx_gpio;
    L.tx_gpio = (int8_t)cfg->tx_gpio;
    L.rst_gpio = (int8_t)cfg->rst_gpio;
    L.device = cfg->device;
    L.address = LORA_ADDR_HANDHELD(cfg->device);
    L.networkid = LORA_NETWORK_ID(cfg->discriminator[0]);
    L.broadcast_ok = true;
    L.on_frame = cfg->on_frame;
    lora_txq_init(&L.txq);   /* no slot has memory until a module answers */
    lora_asm_init(&L.asmb);
    /* D76: the LoRa key, never the backbone key, which a handheld does not hold. The sender field
     * is this handheld's device number, which no AP can ever put there, so the one key is never
     * used with a repeated nonce (lora_seal.h). */
    lora_seal_init(&L.seal, cfg->key, (uint16_t)cfg->device, cfg->boot);

    L.lock = xSemaphoreCreateMutex();
    if (L.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uart_config_t uc = {
        .baud_rate = LORA_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LORA_UART, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(LORA_UART, &uc);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(LORA_UART, L.tx_gpio, L.rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = gpio_set_pull_mode((gpio_num_t)L.rx_gpio, GPIO_PULLUP_ONLY);   /* nothing fitted reads as silence */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LORA] Serial port not opened: %s", esp_err_to_name(err));
        return err;
    }
    /*
     * The task looks for a module on its first pass, not here: start-up must not wait on a radio
     * that may not exist, so a handheld with no module boots in exactly the time it boots in today.
     */
    if (xTaskCreate(lora_task, "hh_lora", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    L.started = true;
    return ESP_OK;
}
