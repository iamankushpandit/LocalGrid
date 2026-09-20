/*
 * ble_link.c - the sealed, read-only BLE admin link (D70). Protocol: docs/ble-link.md.
 *
 * Sealing: K_link = HKDF-SHA256(salt "LG-BLE-LINK-1", ikm = K, info "admin link"), K being the
 * the same key the pairing code (D69) carries, so a paired watcher needs nothing new. Every chunk
 * is ChaCha20-Poly1305 with a full 16-byte tag, AAD = its own 4-byte clear header, nonce =
 * (direction, session, counter). The session is picked at random when the connection opens and
 * each direction counts its own chunks from 0, so a nonce is never used twice under this key: a
 * counter is spent whether or not the chunk went out, and the connection closes at the u32 wrap.
 *
 * Nothing here writes. There is no opcode that changes the grid and none that returns any message
 * body, announcement text, or audio (D70); the two large replies are the admin page's own bytes,
 * emitted by web_admin's one emitter so the two can never drift apart.
 */
#include "ble_link.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "grid_state.h"
#include "host/ble_hs.h"
#include "lg_crypto.h"
#include "lg_envelope.h"
#include "node_app.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings.h"
#include "traffic.h"
#include "web_admin.h"

static const char *TAG = "BLE";

#define LINK_VERSION        1u
#define LINK_HDR            4u
#define LINK_TAG            LG_AEAD_TAG_BYTES
#define LINK_MTU_MIN        64u      /* below this a chunk would carry no body at all */
#define LINK_BODY_MAX       224u     /* the 247-byte MTU NimBLE prefers, less ATT, header and tag */
#define LINK_CHUNK_MAX      (LINK_HDR + LINK_BODY_MAX + LINK_TAG)
#define LINK_IDLE_MS        60000u
#define LINK_LOGIN_TRIES    5u

#define LINK_NOTIFY_WINDOW  4u       /* chunks allowed in flight before waiting for the radio */
#define LINK_NOTIFY_WAIT_MS 2000u    /* the longest one chunk may wait for a buffer */
#define LINK_CHALLENGE      32u
#define LINK_PROOF          32u
#define LINK_TASK_STACK     3584u

/* Opcodes. Replies have bit 7; refusals have bits 7 and 6. */
enum {
    OP_HELLO = 0x01, OP_LOGIN = 0x02, OP_GET_STATUS = 0x03, OP_GET_HISTORY = 0x04, OP_GET_TRAFFIC = 0x05,
    OP_SESSION = 0x80, OP_HELLO_OK = 0x81, OP_LOGIN_OK = 0x82, OP_STATUS = 0x83, OP_HISTORY = 0x84,
    OP_TRAFFIC = 0x85, OP_ERROR = 0xC0, OP_LOGIN_FAIL = 0xC2,
};
enum { ERR_LOGIN = 1, ERR_BUSY = 2, ERR_BAD = 3, ERR_VERSION = 4, ERR_INTERNAL = 5 };
#define FLAG_MORE 0x01u

/* 4c470001-6c67-4772-6964-42544c453031 and its two characteristics (docs/ble-link.md). */
#define LINK_UUID(b3, b2, b1, b0) \
    BLE_UUID128_INIT(0x31, 0x30, 0x45, 0x4c, 0x54, 0x42, 0x64, 0x69, 0x72, 0x47, 0x67, 0x6c, \
                     (b0), (b1), (b2), (b3))
static const ble_uuid128_t UUID_SERVICE = LINK_UUID(0x4c, 0x47, 0x00, 0x01);
static const ble_uuid128_t UUID_REQUEST = LINK_UUID(0x4c, 0x47, 0x00, 0x02);
static const ble_uuid128_t UUID_REPLY   = LINK_UUID(0x4c, 0x47, 0x00, 0x03);

typedef struct {
    uint16_t len;
    uint8_t  data[LINK_CHUNK_MAX];
} link_chunk_t;

static uint16_t s_reply_handle;   /* filled by NimBLE when the service starts */

static struct {
    bool              ready;
    uint8_t           key[LG_AEAD_KEY_LEN];
    QueueHandle_t     inq;
    SemaphoreHandle_t mux;        /* guards the fields below, shared with the NimBLE host task */
    TaskHandle_t      task;
    /* per connection */
    uint16_t          conn;       /* BLE_HS_CONN_HANDLE_NONE when idle */
    uint16_t          session;
    uint16_t          body_max;   /* from the negotiated MTU */
    bool              notify_on;
    bool              hello_done;
    bool              logged_in;
    bool              closing;
    uint32_t          tx_counter;
    uint32_t          rx_next;
    uint32_t          last_ms;
    uint32_t          failures;
    volatile uint32_t in_flight;
    uint8_t           challenge[LINK_CHALLENGE];
} L;

/* ---- connection bookkeeping ---- */

static void lock(void)   { xSemaphoreTake(L.mux, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(L.mux); }

bool ble_link_busy(void)
{
    return L.conn != BLE_HS_CONN_HANDLE_NONE;
}

TaskHandle_t ble_link_task(void)
{
    return L.task;
}

static void reset_session(void)
{
    L.conn = BLE_HS_CONN_HANDLE_NONE;
    L.session = 0;
    L.body_max = 0;
    L.notify_on = false;
    L.hello_done = false;
    L.logged_in = false;
    L.closing = false;
    L.tx_counter = 0;
    L.rx_next = 0;
    L.failures = 0;
    L.in_flight = 0;
    lg_secure_zero(L.challenge, sizeof(L.challenge));
}

/* Ends the connection. Safe from either task; the GAP disconnect event does the tidying. */
static void close_link(const char *why)
{
    uint16_t conn = L.conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || L.closing) {
        return;
    }
    L.closing = true;
    ESP_LOGW(TAG, "[BLE] Admin link closed: %s", why);
    (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

void ble_link_on_connect(uint16_t conn_handle)
{
    lock();
    if (L.conn != BLE_HS_CONN_HANDLE_NONE || !L.ready) {
        unlock();
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);   /* one watcher at a time */
        return;
    }
    if (esp_get_free_heap_size() < LINK_MIN_HEAP) {
        unlock();
        ESP_LOGW(TAG, "[BLE] Admin link refused: only %" PRIu32 " bytes of heap free",
                 esp_get_free_heap_size());
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    reset_session();
    L.conn = conn_handle;
    L.body_max = 0;
    L.last_ms = app_now_ms();
    uint8_t r[2] = { 0, 0 };
    if (lg_crypto_random(r, sizeof(r)) != 0) {
        L.session = (uint16_t)(app_now_ms() & 0xFFFFu);   /* nonce material, never a secret */
    } else {
        L.session = (uint16_t)(r[0] | ((uint16_t)r[1] << 8));
    }
    unlock();
    ESP_LOGI(TAG, "[BLE] Admin link open (D70); not logged in");   /* no address is logged (D21) */
}

void ble_link_on_disconnect(uint16_t conn_handle)
{
    lock();
    if (L.conn == conn_handle) {
        reset_session();
        ESP_LOGI(TAG, "[BLE] Admin link ended");
    }
    unlock();
}

void ble_link_on_mtu(uint16_t conn_handle, uint16_t mtu)
{
    lock();
    if (L.conn == conn_handle) {
        uint16_t room = mtu > (3u + LINK_HDR + LINK_TAG) ? (uint16_t)(mtu - 3u - LINK_HDR - LINK_TAG) : 0u;
        L.body_max = room > LINK_BODY_MAX ? LINK_BODY_MAX : room;
    }
    unlock();
}

void ble_link_on_notify_tx(void)
{
    if (L.in_flight > 0) {
        L.in_flight--;
    }
}

/* ---- sealing ---- */

/* Nonce: dir, session (u16 LE), 0, counter (u32 LE), four zeros. */
static void make_nonce(uint8_t dir, uint16_t session, uint32_t counter, uint8_t nonce[LG_AEAD_NONCE_LEN])
{
    memset(nonce, 0, LG_AEAD_NONCE_LEN);
    nonce[0] = dir;
    lg_wr16(nonce + 1, session);
    lg_wr32(nonce + 4, counter);
}

/* Seals one chunk and notifies it. Waits for a radio buffer rather than dropping it. */
static bool send_chunk(uint8_t opcode, uint8_t flags, const uint8_t *body, size_t len)
{
    if (L.conn == BLE_HS_CONN_HANDLE_NONE || L.closing || len > LINK_BODY_MAX) {
        return false;
    }
    if (L.tx_counter == 0xFFFFFFFFu) {
        close_link("chunk counter used up; a nonce is never reused");
        return false;
    }
    uint8_t out[LINK_CHUNK_MAX];
    out[0] = opcode;
    out[1] = flags;
    lg_wr16(out + 2, (uint16_t)len);
    uint8_t nonce[LG_AEAD_NONCE_LEN];
    uint32_t counter = L.tx_counter++;   /* spent whether or not it goes out */
    make_nonce(1u, L.session, counter, nonce);
    int n = lg_aead_seal(L.key, nonce, out, LINK_HDR, body, len, out + LINK_HDR);
    if (n != (int)(len + LINK_TAG)) {
        close_link("could not seal a reply");
        return false;
    }
    size_t total = LINK_HDR + (size_t)n;

    uint32_t waited = 0;
    while (L.in_flight >= LINK_NOTIFY_WINDOW) {
        if (waited >= LINK_NOTIFY_WAIT_MS || L.closing) {
            close_link("the watcher stopped taking the reply");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    for (;;) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(out, total);
        if (om != NULL) {
            L.in_flight++;
            int rc = ble_gatts_notify_custom(L.conn, s_reply_handle, om);
            if (rc == 0) {
                return true;
            }
            if (L.in_flight > 0) {
                L.in_flight--;
            }
            close_link("the reply could not be sent");
            return false;
        }
        if (waited >= LINK_NOTIFY_WAIT_MS || L.closing) {
            close_link("no radio buffer for the reply");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

/* ---- replies ---- */

/* A reply gathers here piece by piece and goes out whenever one chunk's worth is ready. */
typedef struct {
    uint8_t opcode;
    size_t  n;
    bool    failed;
    uint8_t piece[LINK_BODY_MAX];
} reply_t;

static void reply_begin(reply_t *r, uint8_t opcode)
{
    r->opcode = opcode;
    r->n = 0;
    r->failed = false;
}

static size_t chunk_room(void)
{
    size_t room = L.body_max;   /* set from the negotiated MTU before any reply is built */
    return room == 0 || room > LINK_BODY_MAX ? LINK_BODY_MAX : room;
}

static bool reply_write(void *ctx, const void *data, size_t len)
{
    reply_t *r = (reply_t *)ctx;
    const uint8_t *p = (const uint8_t *)data;
    size_t room = chunk_room();
    while (len > 0 && !r->failed) {
        size_t take = room - r->n;
        if (take > len) {
            take = len;
        }
        memcpy(r->piece + r->n, p, take);
        r->n += take;
        p += take;
        len -= take;
        if (r->n == room) {
            r->failed = !send_chunk(r->opcode, FLAG_MORE, r->piece, r->n);
            r->n = 0;
        }
    }
    return !r->failed;
}

static bool reply_end(reply_t *r)
{
    if (r->failed) {
        return false;
    }
    return send_chunk(r->opcode, 0, r->piece, r->n);   /* the last chunk, "more" clear */
}

/* One short reply that always fits a chunk. */
static bool send_short(uint8_t opcode, const uint8_t *body, size_t len)
{
    return send_chunk(opcode, 0, body, len);
}

static bool send_error_code(uint8_t code, const char *reason)
{
    uint8_t body[64];
    size_t n = strlen(reason);
    if (n > sizeof(body) - 1u) {
        n = sizeof(body) - 1u;
    }
    body[0] = code;
    memcpy(body + 1, reason, n);
    return send_short(OP_ERROR, body, 1u + n);
}

/* ---- opcodes ---- */

static void do_hello(const uint8_t *body, size_t len)
{
    if (len < 1u || body[0] != LINK_VERSION) {
        (void)send_error_code(ERR_VERSION, "unsupported version");
        close_link("the watcher speaks another version");
        return;
    }
    node_settings_t cfg;
    grid_state_settings(&cfg);
    if (lg_crypto_random(L.challenge, sizeof(L.challenge)) != 0) {
        (void)send_error_code(ERR_INTERNAL, "no randomness");
        close_link("could not make a login challenge");
        return;
    }
    uint8_t out[1 + 1 + 4 + 1 + SETTINGS_SALT_LEN + 4 + LINK_CHALLENGE];
    out[0] = LINK_VERSION;
    out[1] = (uint8_t)g_app.index;
    lg_wr32(out + 2, g_app.boot);
    out[6] = cfg.configured ? 1u : 0u;
    memcpy(out + 7, cfg.salt, SETTINGS_SALT_LEN);
    lg_wr32(out + 7 + SETTINGS_SALT_LEN, cfg.iterations);
    memcpy(out + 11 + SETTINGS_SALT_LEN, L.challenge, LINK_CHALLENGE);
    L.hello_done = true;
    (void)send_short(OP_HELLO_OK, out, sizeof(out));
    lg_secure_zero(&cfg, sizeof(cfg));
}

static void do_login(const uint8_t *body, size_t len)
{
    static const char CONTEXT[] = "lg-ble-admin";
    if (!L.hello_done || len != LINK_PROOF) {
        (void)send_error_code(ERR_BAD, "say hello first");
        return;
    }
    node_settings_t cfg;
    grid_state_settings(&cfg);
    if (!cfg.configured) {
        lg_secure_zero(&cfg, sizeof(cfg));
        (void)send_error_code(ERR_BAD, "not set up yet");
        return;
    }
    uint8_t msg[LINK_CHALLENGE + sizeof(CONTEXT) - 1u];
    memcpy(msg, L.challenge, LINK_CHALLENGE);
    memcpy(msg + LINK_CHALLENGE, CONTEXT, sizeof(CONTEXT) - 1u);
    uint8_t want[32];
    bool ok = lg_hmac_sha256(cfg.hash, SETTINGS_HASH_LEN, msg, sizeof(msg), want) == 0 &&
              lg_ct_equal(want, body, LINK_PROOF);
    lg_secure_zero(want, sizeof(want));
    lg_secure_zero(msg, sizeof(msg));
    lg_secure_zero(&cfg, sizeof(cfg));
    if (!ok) {
        L.failures++;
        /* The admin page's own shape: free tries, then a wait that doubles. Nothing of the hash,
         * the salt, the challenge or the proof is ever logged. */
        uint32_t wait_s = L.failures * 2u;
        uint8_t out[4];
        lg_wr32(out, wait_s);
        (void)send_short(OP_LOGIN_FAIL, out, sizeof(out));
        ESP_LOGW(TAG, "[BLE] Admin link login refused (%" PRIu32 " of %u)", L.failures,
                 (unsigned)LINK_LOGIN_TRIES);
        if (L.failures >= LINK_LOGIN_TRIES) {
            close_link("too many failed logins");
        } else {
            vTaskDelay(pdMS_TO_TICKS(wait_s * 100u));   /* the link is one client, so a pause is the limit */
        }
        return;
    }
    L.logged_in = true;
    L.failures = 0;
    lg_secure_zero(L.challenge, sizeof(L.challenge));
    ESP_LOGI(TAG, "[BLE] Admin link logged in");
    (void)send_short(OP_LOGIN_OK, NULL, 0);
}

static void do_status(bool history)
{
    reply_t r;
    reply_begin(&r, history ? OP_HISTORY : OP_STATUS);
    web_sink_t sink = { .ctx = &r, .write = reply_write };
    /* A short wait: the admin page owns the emitter while it is answering, and a watcher waits
     * its turn rather than holding up the page. */
    bool ok = history ? web_admin_emit_history(&sink, 400u) : web_admin_emit_status(&sink, 400u);
    if (!ok && !r.failed) {
        (void)send_error_code(ERR_BUSY, "busy, ask again");
        return;
    }
    (void)reply_end(&r);
}

static void do_traffic(void)
{
    static uint8_t rec[TRAFFIC_RECORD_MAX];   /* link task only; not on its stack */
    size_t n = traffic_record(rec, sizeof(rec), 200u);
    if (n == 0) {
        (void)send_error_code(ERR_BUSY, "busy, ask again");
        return;
    }
    reply_t r;
    reply_begin(&r, OP_TRAFFIC);
    if (reply_write(&r, rec, n)) {
        (void)reply_end(&r);
    }
}

static void handle_request(const link_chunk_t *c)
{
    if (c->len < LINK_HDR + LINK_TAG) {
        close_link("a request chunk was too short");
        return;
    }
    uint16_t mtu = ble_att_mtu(L.conn);
    uint16_t room = mtu > (3u + LINK_HDR + LINK_TAG) ? (uint16_t)(mtu - 3u - LINK_HDR - LINK_TAG) : 0u;
    L.body_max = room > LINK_BODY_MAX ? LINK_BODY_MAX : room;
    if (L.body_max == 0 || mtu < LINK_MTU_MIN) {
        close_link("the watcher's MTU is too small to carry a reply");
        return;
    }
    uint8_t body[LINK_BODY_MAX];
    size_t ct_len = c->len - LINK_HDR;
    if (ct_len - LINK_TAG > sizeof(body)) {
        close_link("a request chunk was too long");
        return;
    }
    /*
     * The header is clear (it is the AAD, and its "more follows" flag must be readable), so the
     * counter comes from the sealed reading of it: the tag covers the header, and a counter that
     * repeats or goes backwards closes the connection.
     */
    uint16_t declared = lg_rd16(c->data + 2);
    uint32_t counter = L.rx_next;
    uint8_t nonce[LG_AEAD_NONCE_LEN];
    make_nonce(0u, L.session, counter, nonce);
    int n = lg_aead_open(L.key, nonce, c->data, LINK_HDR, c->data + LINK_HDR, ct_len, body);
    if (n < 0 || (size_t)n != declared) {
        close_link("a request did not open, or was out of step");
        return;
    }
    L.rx_next = counter + 1u;
    L.last_ms = app_now_ms();

    uint8_t op = c->data[0];
    if (op != OP_HELLO && !L.hello_done) {
        (void)send_error_code(ERR_BAD, "say hello first");
        return;
    }
    if ((op == OP_GET_STATUS || op == OP_GET_HISTORY || op == OP_GET_TRAFFIC) && !L.logged_in) {
        (void)send_error_code(ERR_LOGIN, "log in first");
        return;
    }
    switch (op) {
    case OP_HELLO:       do_hello(body, (size_t)n); break;
    case OP_LOGIN:       do_login(body, (size_t)n); break;
    case OP_GET_STATUS:  do_status(false); break;
    case OP_GET_HISTORY: do_status(true); break;
    case OP_GET_TRAFFIC: do_traffic(); break;
    default:
        (void)send_error_code(ERR_BAD, "unknown request");
        break;
    }
    lg_secure_zero(body, sizeof(body));
}

/* ---- the link task ---- */

static void link_task(void *arg)
{
    (void)arg;
    static link_chunk_t c;
    for (;;) {
        if (xQueueReceive(L.inq, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (L.conn != BLE_HS_CONN_HANDLE_NONE && !L.closing) {
                handle_request(&c);
            }
        } else if (L.conn != BLE_HS_CONN_HANDLE_NONE && !L.closing &&
                   app_now_ms() - L.last_ms > LINK_IDLE_MS) {
            close_link("idle for a minute");
        }
    }
}

/* ---- GATT ---- */

/* The watcher wrote one sealed request chunk. Host task: copy it and return (AGENTS.md). */
static int on_request_write(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                            void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (conn_handle != L.conn || !L.ready) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    static link_chunk_t c;   /* the host task delivers one write at a time */
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(c.data)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, c.data, sizeof(c.data), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    c.len = len;
    if (xQueueSend(L.inq, &c, 0) != pdTRUE) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;   /* one request at a time */
    }
    return 0;
}

static int on_reply_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;   /* notify only: everything is sealed and asked for */
}

static const struct ble_gatt_chr_def LINK_CHRS[] = {
    {
        .uuid = &UUID_REQUEST.u,
        .access_cb = on_request_write,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &UUID_REPLY.u,
        .access_cb = on_reply_access,
        .val_handle = &s_reply_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def LINK_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SERVICE.u,
        .characteristics = LINK_CHRS,
    },
    { 0 },
};

esp_err_t ble_link_gatt_register(void)
{
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(LINK_SVCS);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(LINK_SVCS);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] Admin link service not registered, rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ble_link_on_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify_on)
{
    lock();
    if (conn_handle == L.conn && attr_handle == s_reply_handle) {
        L.notify_on = notify_on;
        L.last_ms = app_now_ms();
        if (notify_on) {
            /*
             * Subscribing starts a conversation, and may start a second one on a connection that
             * never went away: Windows keeps a BLE connection alive after a watcher believes it
             * has disconnected, so the next watcher subscribes again on the same connection and
             * counts its messages from zero. Before 2026-09-20 the AP kept the old session and
             * counters, so that first message was "out of step", the AP hung up, and it then
             * refused every later watcher: the dashboard's map and traffic worked once after a
             * reboot and never again. So each subscribe gets a fresh session, fresh counters and
             * a fresh login. Failed logins are deliberately NOT forgiven here, or resubscribing
             * would wipe the rate limit.
             */
            uint32_t keep_failures = L.failures;
            uint16_t keep_conn = L.conn;
            reset_session();
            L.conn = keep_conn;
            L.failures = keep_failures;
            L.notify_on = true;
            L.last_ms = app_now_ms();
            uint8_t r[2] = { 0, 0 };
            if (lg_crypto_random(r, sizeof(r)) != 0) {
                L.session = (uint16_t)(app_now_ms() & 0xFFFFu);
            } else {
                L.session = (uint16_t)(r[0] | ((uint16_t)r[1] << 8));
            }
            /*
             * The one message that is not sealed: it carries the session number both directions'
             * nonces are built from. A nonce is public by design and this one is random, so
             * nothing is given away; everything after it is sealed.
             */
            uint8_t hdr[LINK_HDR + 2];
            hdr[0] = OP_SESSION;
            hdr[1] = 0;
            lg_wr16(hdr + 2, 2u);
            lg_wr16(hdr + LINK_HDR, L.session);
            struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, sizeof(hdr));
            if (om != NULL) {
                L.in_flight++;
                if (ble_gatts_notify_custom(L.conn, s_reply_handle, om) != 0 && L.in_flight > 0) {
                    L.in_flight--;
                }
            }
        }
    }
    unlock();
}

esp_err_t ble_link_init(const uint8_t backbone_key[32])
{
    /*
     * K_link comes from K, the status key, not from the backbone key (docs/ble-link.md: "the link
     * key comes from the same key the pairing code carries"). This matters: a paired phone is
     * given K alone and never holds the backbone key, so deriving from the backbone key would mean
     * no phone could ever open the link. It did exactly that until 2026-09-20 - every request
     * failed to unseal, the AP hung up, and the watcher's map, positions and traffic never worked.
     */
    static const char k_salt[] = "LG-BLE-STATUS-1";
    static const char k_info[] = "ble status";
    static const char salt[] = "LG-BLE-LINK-1";
    static const char info[] = "admin link";
    uint8_t k[32];
    L.conn = BLE_HS_CONN_HANDLE_NONE;
    if (lg_hkdf_sha256((const uint8_t *)k_salt, sizeof(k_salt) - 1u, backbone_key, 32,
                       (const uint8_t *)k_info, sizeof(k_info) - 1u, k, sizeof(k)) != 0 ||
        lg_hkdf_sha256((const uint8_t *)salt, sizeof(salt) - 1u, k, sizeof(k), (const uint8_t *)info,
                       sizeof(info) - 1u, L.key, sizeof(L.key)) != 0) {
        lg_secure_zero(k, sizeof(k));
        ESP_LOGE(TAG, "[BLE] Admin link off: key derivation failed");
        return ESP_FAIL;
    }
    lg_secure_zero(k, sizeof(k));
    L.mux = xSemaphoreCreateMutex();
    L.inq = xQueueCreate(1, sizeof(link_chunk_t));
    if (L.mux == NULL || L.inq == NULL) {
        ESP_LOGE(TAG, "[BLE] Admin link off: out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_task, "lg_blelink", LINK_TASK_STACK, NULL, 4, &L.task) != pdPASS) {
        ESP_LOGE(TAG, "[BLE] Admin link off: no task");
        return ESP_ERR_NO_MEM;
    }
    L.ready = true;
    ESP_LOGI(TAG, "[BLE] Admin link ready: one watcher at a time, read-only, sealed (D70)");
    return ESP_OK;
}
