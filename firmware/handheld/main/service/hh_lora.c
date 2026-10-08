/* hh_lora.c - a LoRa module on a handheld (D77). See hh_lora.h. */
#include "hh_lora.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_envelope.h"
#include "lg_lora_hh.h"

static const char *TAG = "LORA";
static int s_power_dbm = LG_LORA_POWER_FULL;   /* D98: what AT+CRFOP is set to */

/*
 * Which serial port. UART0 is the console on the boards that have one there, and a board with a GPS
 * has taken UART0 or UART1 already (hh_gps.c), so the last high-power UART is the one left. The
 * ESP32-S3's GPIO matrix puts any UART on any free pin, and the F4B is the only board with a LoRa
 * connector today (D77).
 *
 * The count differs by chip and must be read from the chip's own capabilities rather than assumed:
 * the ESP32 and S3 have three high-power UARTs, but the **C6 has two**, so UART_NUM_2 does not exist
 * there at all and naming it is a compile error. No C6 board has a LoRa connector, so this port is
 * never opened on one -- but every handheld shares one firmware, so it still has to compile.
 */
#include "soc/soc_caps.h"
#if SOC_UART_HP_NUM > 2
#define LORA_UART         UART_NUM_2
#else
#define LORA_UART         UART_NUM_1
#endif
#define UART_RX_BUF       1024
#define UART_TX_BUF       512
#define LORA_LINE_MAX     320u     /* "+RCV=" plus a part's characters plus its numbers, with room over */
#define TASK_STACK        4096
#define TASK_PRIORITY     2        /* below the service task (5): airtime can wait, a message cannot */
#define AT_TIMEOUT_MS     1000u
#define SEND_TIMEOUT_MS   8000u    /* one full part is about a second of airtime, plus the module */
#define DETECT_TRIES      3u
#define PART_TRIES        3u
#define BACKOFF_MAX_MS    300u     /* random wait before every transmission: no carrier sense */
/* How long another sender's remaining parts keep this radio quiet, and the quiet the module needs
 * between packets as a receiver (as on the APs, measured 2026-10-08). */
#define PART_GAP_MIN_MS   1500u
#define AIR_PART_GAP_MS   1600u
#define AIR_HOLD_MARGIN_MS 300u
#define AIR_HOLD_CAP_MS   8000u
#define RETRY_MIN_MS      800u
#define RETRY_SPAN_MS     2200u
#define RX_RING           2u
#define WEDGE_BACKOFF_MS  2000u
#define WEDGE_BACKOFF_MAX 60000u
#define QUIET_WAIT_MS     1000u    /* with no module: purely passive, and costs nothing */

/*
 * Memory. Taken ONCE, when a module has answered, and never again; nothing here allocates per
 * message. A handheld with no module, or a board with no connector, holds no frame buffers at all.
 *
 * Only small slots exist here. The 6 KB long-payload slots the APs can refuse (LORA_LONG_PAYLOAD)
 * are for D72's voice notes, which D78 caps at about 10 s over LoRa and which are not built; when
 * they are, they need their own slot sizing measured on the handheld as well as on the AP.
 */
#define ASM_SLOTS_USED   2u
#define TXQ_SLOTS_USED   4u

_Static_assert(ASM_SLOTS_USED <= LORA_ASM_SLOTS, "reassembly slots fit the wire layer's table");
_Static_assert(TXQ_SLOTS_USED <= LORA_TXQ_SLOTS, "queue slots fit the wire layer's table");

/* What crosses from the LoRa task to the service task: the slot holding the payload, not a copy. */
typedef struct {
    lora_asm_slot_t *slot;
    int8_t           rssi;
    int8_t           snr;
} rx_entry_t;

static struct {
    bool     started;
    int8_t   rx_pin, tx_pin, rst_pin;
    uint32_t device;
    uint16_t address;
    uint8_t  networkid;

    bool     fitted;         /* a module answered */
    bool     said_missing;   /* the one line about no module has been logged for these pins */
    bool     configured;
    bool     off;            /* console chaos hook */
    uint32_t off_until_ms;
    bool     reset_wanted;
    bool     at_pending;
    char     at_text[64];
    char     version[24];

    lglh_t   seal;           /* D77: the handheld LoRa key, its sequence and its replay window */
    lora_asm_t asmb;
    lora_txq_t txq;
    uint8_t  asm_buf[ASM_SLOTS_USED][LORA_SMALL_MAX];
    uint8_t  txq_buf[TXQ_SLOTS_USED][LORA_SMALL_MAX];
    uint8_t  next_id;

    rx_entry_t rx[RX_RING];
    uint8_t    rx_head, rx_count;

    SemaphoreHandle_t lock;
    hh_lora_frame_cb_t on_frame;

    /* Which APs have been heard, and when. Bit n: AP n. */
    uint8_t  peer_bits;
    uint32_t peer_ms[LG_MAX_NODES];
    uint32_t heard_ms;       /* last frame in, 0 never */

    int8_t   rssi, snr;
    uint32_t frames_out, frames_in, parts_out, parts_in, parts_dropped;
    uint32_t refused_type, retries, airtime_ms, restarts;
    uint32_t acks_out;   /* D109: acknowledgements sent to an AP for a frame it is waiting to hear about */
    uint32_t air_busy_until_ms;   /* somebody's multi-part frame is mid-flight: hold non-alert sends */
    uint32_t held_for_air;
    uint32_t last_tx_end_ms;      /* when this handheld's own last transmission finished */
} L;

static void lock(void)   { if (L.lock != NULL) { xSemaphoreTake(L.lock, portMAX_DELAY); } }
static void unlock(void) { if (L.lock != NULL) { xSemaphoreGive(L.lock); } }

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t rand_ms(uint32_t span)
{
    return span == 0 ? 0 : esp_random() % span;
}

/* ---- reading lines from the module ---- */

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

/*
 * Waits for one kind of reply. A frame arriving while we wait is never lost: +RCV is handled
 * wherever it turns up, including between the parts of our own transmission.
 */
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
    if (L.rst_pin >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (unsigned)L.rst_pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        (void)gpio_config(&io);
        (void)gpio_set_level((gpio_num_t)L.rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        (void)gpio_set_level((gpio_num_t)L.rst_pin, 1);
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
 * The same settings the APs use, from the one shared header (lg_lora_wire.h), because a handheld on
 * different settings is on a different channel and nothing it says is ever heard.
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
    snprintf(cmd, sizeof(cmd), "AT+BAND=%" PRIu32, (uint32_t)LG_LORA_BAND_HZ);
    if (!at_ok(cmd, AT_TIMEOUT_MS)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+PARAMETER=%d,%d,%d,%d", LG_LORA_SF, LG_LORA_BW_CODE, LG_LORA_CR_CODE,
             LG_LORA_PREAMBLE);
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
    char addr[16] = "?", net[16] = "?", par[32] = "?";
    (void)at_value("AT+ADDRESS?", addr, sizeof(addr));
    (void)at_value("AT+NETWORKID?", net, sizeof(net));
    (void)at_value("AT+PARAMETER?", par, sizeof(par));
    ESP_LOGI(TAG, "[LORA] Configured %s: address %s, network %s, parameters %s, %" PRIu32 " Hz, %d dBm",
             L.version[0] != '\0' ? L.version : "module", addr, net, par, (uint32_t)LG_LORA_BAND_HZ,
             s_power_dbm);
    return true;
}

void hh_lora_set_power(int dbm)
{
    s_power_dbm = lg_lora_power_clamp(dbm);
    if (L.started && L.fitted) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", s_power_dbm);
        hh_lora_request_at(cmd);   /* the module takes it at once; a restart sets it again from here */
        ESP_LOGI(TAG, "[LORA] Transmit power: %s (%d dBm)", lg_lora_power_name(s_power_dbm), s_power_dbm);
    }
}

int hh_lora_power(void)
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
        L.parts_dropped++;   /* the chaos hook: the radio hears, we pretend not to */
        return;
    }
    size_t n = lora_b64_decode(at->data, at->len, raw, sizeof(raw));
    if (n == 0) {
        L.parts_dropped++;
        return;
    }
    /* Carrier sense at the protocol layer, as on the APs (2026-10-08): a part with more to come
     * means the air is taken for that much longer, and transmitting into it loses a part for
     * everyone. Only an alert may interrupt. */
    lora_part_t ph;
    if (lora_part_parse(raw, n, &ph) && ph.part + 1u < ph.of) {
        uint32_t more = (uint32_t)(ph.of - ph.part - 1u);
        uint32_t wait = more * (lora_airtime_ms(LORA_PART_B64_MAX) + AIR_PART_GAP_MS) + AIR_HOLD_MARGIN_MS;
        wait = wait > AIR_HOLD_CAP_MS ? AIR_HOLD_CAP_MS : wait;
        uint32_t until = now_ms() + wait;
        if ((int32_t)(until - L.air_busy_until_ms) > 0) {
            L.air_busy_until_ms = until;
        }
    }
    lora_asm_slot_t *slot = NULL;
    if (lora_asm_feed(&L.asmb, now_ms(), raw, n, &slot) != LORA_ASM_COMPLETE) {
        return;   /* lora_asm_feed counts everything it refused, oversize included */
    }
    lock();
    if (L.rx_count >= RX_RING) {
        unlock();
        lora_asm_done(&L.asmb, slot);
        L.parts_dropped++;   /* the service task has not drained in a whole pass */
        return;
    }
    rx_entry_t *e = &L.rx[(L.rx_head + L.rx_count) % RX_RING];
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
    /*
     * The sender byte says handheld, with this device's index in the low nibble. D71 settled the
     * whole byte before handhelds had modules, so the wire format needs no change here; a device
     * above 15 would, which is a change to make before it happens rather than after.
     */
    size_t plen = lora_part_build((uint8_t)(LORA_PEER_HANDHELD | (L.device & 0x0Fu)), id, (uint8_t)part,
                                  (uint8_t)of, payload + off, slice, raw, sizeof(raw));
    size_t blen = plen != 0 ? lora_b64_encode(raw, plen, b64, sizeof(b64) - 1u) : 0u;
    if (blen == 0) {
        return false;
    }
    b64[blen] = '\0';
    for (unsigned attempt = 0; attempt < PART_TRIES; attempt++) {
        /* No carrier sense in this module, and every AP shares the channel: wait a short random
         * time before each transmission, and longer and randomly again after a failure. */
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

/* ---- the task ---- */

static void run_pending_at(void)
{
    char cmd[sizeof(L.at_text)];
    lock();
    snprintf(cmd, sizeof(cmd), "%s", L.at_text);
    L.at_pending = false;
    unlock();
    if (cmd[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "[LORA] > %s", cmd);
    at_write(cmd);
    for (;;) {
        char *line = NULL;
        if (!read_line(AT_TIMEOUT_MS, &line)) {
            return;
        }
        lora_at_t at;
        if (lora_at_parse(line, &at) && at.kind == LORA_AT_RCV) {
            handle_rcv(&at);
            continue;
        }
        ESP_LOGI(TAG, "[LORA] < %s", line);
        return;
    }
}

static void drop_queue(void)
{
    lock();
    for (size_t i = 0; i < LORA_TXQ_SLOTS; i++) {
        lora_txq_release(&L.txq, &L.txq.slots[i]);
    }
    unlock();
}

/* One part of whatever is due. False when nothing was due, so the caller can listen instead. */
static bool transmit_a_part(unsigned *fails)
{
    lock();
    lora_txq_slot_t *s = lora_txq_peek(&L.txq, now_ms());
    if (s != NULL && !s->alert && !s->is_ack && s->part == 0u &&
        (int32_t)(now_ms() - L.air_busy_until_ms) < 0) {
        L.held_for_air++;   /* somebody's frame is mid-flight: wait for the air, keep listening */
        unlock();
        return false;
    }
    /* Receivers need PART_GAP_MIN_MS of quiet after each packet, ours included (as on the APs).
     * An acknowledgement is never held: the sender is listening for it right now. */
    if (s != NULL && !s->alert && !s->is_ack && (int32_t)(now_ms() - (L.last_tx_end_ms + PART_GAP_MIN_MS)) < 0) {
        unlock();
        return false;
    }
    unlock();
    if (s == NULL) {
        return false;
    }
    s->sending = true;
    bool ok = send_one(s->dest, s->id, s->data, s->len, s->part, s->of);
    lock();
    L.last_tx_end_ms = now_ms();
    if (!ok) {
        (*fails)++;
        lora_txq_release(&L.txq, s);
        unlock();
        return true;
    }
    *fails = 0;
    s->part++;
    if (s->part >= s->of) {
        L.frames_out++;
        lora_txq_release(&L.txq, s);
    }
    unlock();
    return true;
}

static bool take_buffers(void)
{
    lora_asm_init(&L.asmb);
    lora_txq_init(&L.txq);
    for (size_t i = 0; i < ASM_SLOTS_USED; i++) {
        lora_asm_set_slot(&L.asmb, i, L.asm_buf[i], LORA_SMALL_MAX);
    }
    for (size_t i = 0; i < TXQ_SLOTS_USED; i++) {
        lora_txq_set_slot(&L.txq, i, L.txq_buf[i], LORA_SMALL_MAX);
    }
    return true;   /* the buffers are in L, taken with the rest of it: nothing can fail here */
}

static void lora_task(void *arg)
{
    (void)arg;
    uint32_t backoff = WEDGE_BACKOFF_MS;
    uint32_t next_retry = 0;
    unsigned fails = 0;
    bool first_look = true;

    for (;;) {
        /*
         * No module. The task only listens, on a port nothing is driving, and never transmits,
         * never logs and never times anything: a handheld with no module is exactly a handheld
         * built before D77. A module plugged in later prints +READY as it powers up, and is picked
         * up without a restart.
         */
        if (!L.fitted) {
            char *line = NULL;
            if (!first_look && !read_line(QUIET_WAIT_MS, &line)) {
                continue;
            }
            first_look = false;
            if (!detect()) {
                if (!L.said_missing) {
                    L.said_missing = true;
                    ESP_LOGI(TAG, "[LORA] No module on GPIO%d/%d; this handheld uses Wi-Fi alone, "
                                  "exactly as before", L.rx_pin, L.tx_pin);
                }
                continue;
            }
            (void)take_buffers();
            ESP_LOGI(TAG, "[LORA] Module on UART%d (rx GPIO%d, tx GPIO%d), address %u, network %u",
                     (int)LORA_UART, L.rx_pin, L.tx_pin, (unsigned)L.address, L.networkid);
            L.said_missing = false;
            L.fitted = true;
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
        if (transmit_a_part(&fails)) {
            if (fails >= 5u) {
                fails = 0;
                L.configured = false;   /* wedged: reconfigure, and back off if it keeps failing */
                next_retry = now_ms() + backoff;
                backoff = backoff * 2u > WEDGE_BACKOFF_MAX ? WEDGE_BACKOFF_MAX : backoff * 2u;
            }
            continue;
        }
        /* Nothing to send: listen. This is where a handheld spends nearly all of its time. */
        char *line = NULL;
        if (read_line(50u, &line)) {
            lora_at_t at;
            if (lora_at_parse(line, &at) && at.kind == LORA_AT_RCV) {
                handle_rcv(&at);
            }
        }
    }
}

/* ---- starting ---- */

esp_err_t hh_lora_start(int rx_gpio, int tx_gpio, int rst_gpio, uint32_t device, uint32_t boot,
                        const uint8_t key[32], const uint8_t discriminator[4],
                        hh_lora_frame_cb_t on_frame)
{
    if (rx_gpio < 0 || tx_gpio < 0 || key == NULL || discriminator == NULL) {
        return ESP_ERR_INVALID_ARG;   /* a board with no connector: not a failure, and not logged */
    }
    if (L.started) {
        return ESP_OK;
    }
    memset(&L, 0, sizeof(L));
    L.rx_pin = (int8_t)rx_gpio;
    L.tx_pin = (int8_t)tx_gpio;
    L.rst_pin = (int8_t)rst_gpio;
    L.device = device;
    L.address = HH_LORA_ADDR_SELF(device);
    L.on_frame = on_frame;
    L.rssi = 0;
    L.snr = 0;

    /*
     * The network ID must match the APs' exactly, and theirs is 3 + (first discriminator byte mod
     * 13) (firmware/node/main/lora.c). A handheld holds the discriminator and never the backbone
     * key, so it computes the same number without holding anything it should not.
     */
    L.networkid = (uint8_t)(3u + discriminator[0] % 13u);

    /* D77: the handheld LoRa key, and the boot counter already committed to NVS by the service. */
    lglh_init(&L.seal, key, LGLH_SENDER_HH((uint16_t)device), boot);

    L.lock = xSemaphoreCreateMutex();
    if (L.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t cfg = {
        .baud_rate = LG_LORA_UART_BAUD,
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
        err = uart_set_pin(LORA_UART, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LORA] Serial port would not open on GPIO%d/%d: %s", rx_gpio, tx_gpio,
                 esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(lora_task, "hh_lora", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    L.started = true;
    return ESP_OK;
}

/* ---- the service task's side ---- */

esp_err_t hh_lora_set_pins(int rx_gpio, int tx_gpio)
{
    if (!L.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rx_gpio < 0 || tx_gpio < 0 || rx_gpio == tx_gpio) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    /* GPIO33-37 are this board's octal PSRAM and 19-20 are USB: driving either is not a bad reading,
     * it is a crash or a dead console. Refused here rather than discovered by trying. */
    if ((rx_gpio >= 33 && rx_gpio <= 37) || (tx_gpio >= 33 && tx_gpio <= 37) ||
        rx_gpio == 19 || rx_gpio == 20 || tx_gpio == 19 || tx_gpio == 20) {
        return ESP_ERR_INVALID_ARG;
    }
#endif
    esp_err_t err = uart_set_pin(LORA_UART, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(L.lock, portMAX_DELAY);
    L.rx_pin = rx_gpio;
    L.tx_pin = tx_gpio;
    L.fitted = false;        /* look again: these are different pins */
    L.configured = false;
    L.said_missing = false;  /* and say so again, for these pins */
    xSemaphoreGive(L.lock);
    ESP_LOGW(TAG, "[LORA] Looking for a module on GPIO%d/%d instead", rx_gpio, tx_gpio);
    return ESP_OK;
}

bool hh_lora_fitted(void)
{
    return L.fitted;
}

bool hh_lora_has_peer(void)
{
    return L.peer_bits != 0u;
}

static void note_peer(uint16_t ap, uint32_t now)
{
    if (ap < LG_MAX_NODES) {
        bool was = (L.peer_bits & (uint8_t)(1u << ap)) != 0u;
        L.peer_bits |= (uint8_t)(1u << ap);
        L.peer_ms[ap] = now;
        if (!was) {
            ESP_LOGI(TAG, "[LORA] Link up to AP %u, RSSI %d dBm, SNR %d", (unsigned)ap, L.rssi, L.snr);
        }
    }
}

/*
 * D109: tell AP `ap` that its frame sealed as `seq` arrived here. Six bytes sealed with the handheld LoRa
 * key, sent at once and ahead of anything waiting: the AP listens for it for about a second.
 */
static void queue_ack(uint16_t ap, uint32_t seq)
{
    uint8_t body[LORA_ACK_LEN];
    if (lora_ack_build(seq, body, sizeof(body)) == 0u) {
        return;
    }
    lock();
    lora_txq_slot_t *s = lora_txq_reserve_ranked(&L.txq, LG_LORA_ADDR_AP(ap), LORA_RANK_MESSAGE, ++L.next_id,
                                                 LGLH_OUTER_LEN + sizeof(body) + LG_AEAD_TAG_LEN, now_ms() + rand_ms(80u));
    int sealed = s != NULL ? lglh_seal(&L.seal, s->data, s->cap, body, sizeof(body)) : -1;
    if (s != NULL && (sealed <= 0 || !lora_txq_commit(&L.txq, s, (size_t)sealed))) {
        lora_txq_release(&L.txq, s);
        s = NULL;
    }
    if (s != NULL) {
        s->is_ack = true;
        L.acks_out++;
    }
    unlock();
}

void hh_lora_poll(uint32_t now)
{
    if (!L.started) {
        return;
    }
    /* A chaos outage that has run its course comes back by itself (D78's `wifi off` in spirit). */
    if (L.off && L.off_until_ms != 0 && (int32_t)(now - L.off_until_ms) >= 0) {
        L.off = false;
        L.off_until_ms = 0;
        ESP_LOGW(TAG, "[LORA] Back on");
    }
    (void)lora_asm_expire(&L.asmb, now);

    /* Hand over whatever arrived. The slot is read where it lies and then given back. */
    for (;;) {
        rx_entry_t e;
        lock();
        if (L.rx_count == 0) {
            unlock();
            break;
        }
        e = L.rx[L.rx_head];
        L.rx_head = (uint8_t)((L.rx_head + 1u) % RX_RING);
        L.rx_count--;
        unlock();

        static uint8_t frame[LG_FRAME_MAX];
        size_t flen = 0;
        uint16_t sender = 0;
        lglh_open_t r = lglh_open(&L.seal, e.slot->data, e.slot->total, &sender, frame, sizeof(frame), &flen);
        if (r == LGLH_OPEN_REPLAY && e.slot->total > LGLH_OUTER_LEN + LG_AEAD_TAG_LEN) {
            /*
             * D109: the same sealed frame again means its AP never heard this handheld's answer - the frame
             * arrived, the acknowledgement did not - so the answer is given again, and the frame is not
             * handled twice. lglh_open authenticated and opened it before it found the number used.
             */
            uint16_t from = lg_rd16(e.slot->data + 2);
            uint32_t seq = lg_rd32(e.slot->data + 8);
            size_t plain = e.slot->total - LGLH_OUTER_LEN - LG_AEAD_TAG_LEN;
            if (LGLH_KIND(from) == LGLH_KIND_AP && LGLH_INDEX(from) < LG_MAX_NODES &&
                lora_frame_wants_ap_ack(frame, plain)) {
                queue_ack(LGLH_INDEX(from), seq);
            }
        }
        lora_asm_done(&L.asmb, e.slot);
        if (r != LGLH_OPEN_OK) {
            continue;   /* lglh_open counts every refusal, and NOT_MINE is not one */
        }
        /*
         * A handheld accepts LoRa frames from APs only (D77): another handheld's frame is not
         * routed by anything here, and taking it would invent a peer relationship the core has
         * never had.
         */
        if (LGLH_KIND(sender) != LGLH_KIND_AP) {
            continue;
        }
        uint16_t ap = LGLH_INDEX(sender);
        L.frames_in++;
        L.heard_ms = now != 0 ? now : 1u;
        note_peer(ap, now);
        if (ap < LG_MAX_NODES && lora_frame_wants_ap_ack(frame, flen)) {
            queue_ack(ap, lg_rd32(e.slot->data + 8));   /* D109: answered before it is handled */
        }
        if (L.on_frame != NULL) {
            L.on_frame(frame, flen, ap);
        }
    }

    /* A peer that has gone quiet is no longer worth transmitting at. */
    for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
        if ((L.peer_bits & (uint8_t)(1u << n)) != 0u &&
            now - L.peer_ms[n] > HH_LORA_PEER_TIMEOUT_MS) {
            L.peer_bits &= (uint8_t)~(1u << n);
            ESP_LOGW(TAG, "[LORA] Link down to AP %u (no word for %" PRIu32 " s)", (unsigned)n,
                     (now - L.peer_ms[n]) / 1000u);
        }
    }
}

void hh_lora_offer(const uint8_t *frame, size_t len, bool wifi_ok)
{
    if (!L.started || !L.fitted || !L.configured || L.off || frame == NULL || len == 0) {
        return;
    }
    /*
     * Two gates, and both matter. The policy is the APs' own (D74): one radio carries at a time,
     * except that urgent broadcasts, an SOS and an all clear always go on both, because finding out
     * a link had died *after* losing an alert is the failure this radio exists to prevent.
     */
    lora_policy_t p = lora_policy_for_frame(frame, len);
    if (p == LORA_SEND_NEVER) {
        return;
    }
    if (p != LORA_SEND_ALWAYS && wifi_ok) {
        return;
    }
    /*
     * The second gate is D77's allow-list, checked here as well as on the AP. An AP refuses what a
     * handheld must not be trusted about; a handheld should not spend a second of airtime finding
     * that out.
     */
    if (!lglh_type_allowed(frame, len)) {
        L.refused_type++;
        return;
    }
    bool alert = p == LORA_SEND_ALWAYS;
    /* The handheld's own notes still go twice: its frames are broadcast, so no one AP owes it an
     * answer; an AP's note to a handheld is acknowledged instead (D109). */
    int copies = lora_frame_is_compact_voice(frame, len) ? 2 : 1;

    lock();
    for (int copy = 0; copy < copies; copy++) {
        /* Sealed straight into its queue slot: nothing is ever staged in a second buffer. */
        uint8_t id = ++L.next_id;
        /* D102: its own message waits behind nothing routine - an announcement or a position gives way. */
        lora_txq_slot_t *s = lora_txq_reserve_ranked(&L.txq, LG_LORA_ADDR_BROADCAST,
                                                     alert ? LORA_RANK_ALERT : lora_frame_rank(frame, len), id,
                                                     LGLH_OUTER_LEN + len + LG_AEAD_TAG_LEN,
                                                     now_ms() + rand_ms(BACKOFF_MAX_MS));
        if (s == NULL) {
            break;   /* lora_txq counts what it could not hold */
        }
        int sealed = lglh_seal(&L.seal, s->data, s->cap, frame, len);
        if (sealed <= 0 || !lora_txq_commit(&L.txq, s, (size_t)sealed)) {
            lora_txq_release(&L.txq, s);
            break;
        }
    }
    unlock();   /* lora_txq_commit worked out how many parts that is */
}

/* ---- console and reporting ---- */

void hh_lora_disable(uint32_t seconds)
{
    uint32_t s = seconds == 0 ? 1u : seconds;
    L.off_until_ms = now_ms() + s * 1000u;
    L.off = true;
    ESP_LOGW(TAG, "[LORA] Off for %" PRIu32 " s (chaos hook); it comes back on its own", s);
}

void hh_lora_enable(void)
{
    L.off = false;
    L.off_until_ms = 0;
    ESP_LOGI(TAG, "[LORA] Back on");
}

void hh_lora_request_reset(void)
{
    L.reset_wanted = true;
}

void hh_lora_request_at(const char *command)
{
    if (command == NULL) {
        return;
    }
    lock();
    snprintf(L.at_text, sizeof(L.at_text), "%s", command);
    L.at_pending = true;
    unlock();
}

void hh_lora_info(hh_lora_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->connector = L.rx_pin >= 0 && L.tx_pin >= 0;
    out->fitted = L.fitted;
    out->configured = L.configured;
    out->off = L.off;
    snprintf(out->version, sizeof(out->version), "%s", L.version);
    out->address = L.address;
    out->networkid = L.networkid;
    out->rssi = L.rssi;
    out->snr = L.snr;
    out->frames_out = L.frames_out;
    out->frames_in = L.frames_in;
    out->parts_out = L.parts_out;
    out->parts_in = L.parts_in;
    out->parts_dropped = L.parts_dropped + L.asmb.dropped;
    out->reasm_timeouts = L.asmb.timeouts;
    out->seal_fail = L.seal.auth_fail + L.seal.malformed + L.seal.replayed;
    out->refused_type = L.refused_type;
    out->queue_dropped = L.txq.dropped;
    out->retries = L.retries;
    out->airtime_ms = L.airtime_ms;
    out->restarts = L.restarts;
    out->heard_age_ms = L.heard_ms == 0 ? UINT32_MAX : now_ms() - L.heard_ms;
    out->peers = L.peer_bits;
}
