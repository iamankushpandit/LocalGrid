#include "sessions.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "lg_envelope.h"
#include "lg_proto_config.h"
#include "lwip/sockets.h"
#include "node_app.h"
#include "power_trace.h"
#include "sdkconfig.h"

static const char *TAG = "NET";

#define SESS_MAX              LG_PROTO_MAX_STATIONS
#define SESS_IDLE_MS          30000u
#define SESS_REGISTER_MS      5000u
#define SEND_WAIT_MS          2000u   /* the longest one handheld may hold up the core task */

typedef struct {
    int      fd;
    uint32_t device;
    uint32_t opened_ms;
    uint32_t last_rx_ms;
    uint16_t fill;
    char     addr[16];
    uint8_t  buf[2 + LG_FRAME_MAX];
} sess_t;

static sess_t s_sess[SESS_MAX];
static int s_listen_fd = -1;
static uint32_t s_voice_dropped;   /* talk frames a listener could not take at once (D61) */
static sess_traffic_t s_traffic;   /* D70: counts and sizes only */

/*
 * Frames for the handheld whose own frame is being handled are gathered here and go out as one
 * write when the handling ends. ESP-IDF builds lwIP with LWIP_NETIF_TX_SINGLE_PBUF, so every
 * send() takes a whole MSS-sized buffer (1512 B of heap) however short the frame, and keeps it
 * until the handheld ACKs. Registering sends ten or so small frames (ack, groups, presence, names,
 * positions): 11 to 17 KB each on MAIN, stacking to a 17 KB minimum when three handhelds came
 * back together. Gathered, they take one or two buffers. The bytes on the wire are unchanged.
 * Core task only, like everything else here.
 */
#define GATHER_MAX CONFIG_LWIP_TCP_MSS

static struct {
    sess_t  *x;      /* the session being handled; NULL when not gathering */
    uint16_t n;
    uint8_t  buf[GATHER_MAX];
} s_gather;

static void gather_flush(bool stop);

static void close_quiet(sess_t *x)
{
    if (x->fd >= 0) {
        close(x->fd);
    }
    memset(x, 0, sizeof(*x));
    x->fd = -1;
}

static void sess_close(sess_t *x, const char *why)
{
    ESP_LOGI(TAG, "[NET] Session %s closed: device %" PRIu32 " (%s)", x->addr, x->device, why);
    s_traffic.disconnects++;
    uint32_t device = x->device;
    close_quiet(x);
    if (device != 0) {
        lg_node_on_session_closed(&g_app.core, device);
    }
}

esp_err_t sess_init(uint16_t port)
{
    for (size_t i = 0; i < SESS_MAX; i++) {
        memset(&s_sess[i], 0, sizeof(s_sess[i]));
        s_sess[i].fd = -1;
    }
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_fd < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s_listen_fd, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen port %u failed: errno %d", port, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }
    fcntl(s_listen_fd, F_SETFL, fcntl(s_listen_fd, F_GETFL, 0) | O_NONBLOCK);
    ESP_LOGI(TAG, "[NET] Listening for handhelds on TCP %u", port);
    return ESP_OK;
}

static void accept_one(uint32_t now)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        return;
    }
    sess_t *slot = NULL;
    for (size_t i = 0; i < SESS_MAX; i++) {
        if (s_sess[i].fd < 0) {
            slot = &s_sess[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "[NET] Session table full; refusing connection");
        s_traffic.refused++;
        close(fd);
        return;
    }
    int one = 1;
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    struct timeval snd = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    slot->opened_ms = now;
    slot->last_rx_ms = now;
    inet_ntoa_r(peer.sin_addr, slot->addr, sizeof(slot->addr));
    ESP_LOGI(TAG, "[NET] Session opened from %s", slot->addr);
    s_traffic.opened++;
    ptrace_event(PTRACE_SESSION);
}

static void on_registered(sess_t *x)
{
    /* A device reconnecting replaces its older session on this node without marking it offline. */
    for (size_t i = 0; i < SESS_MAX; i++) {
        sess_t *o = &s_sess[i];
        if (o != x && o->fd >= 0 && o->device == x->device) {
            ESP_LOGI(TAG, "[NET] Replacing stale session %s for device %" PRIu32, o->addr, o->device);
            close_quiet(o);
        }
    }
    s_traffic.registrations++;
    ESP_LOGI("GRID", "[GRID] Registered device %" PRIu32 " from %s", x->device, x->addr);
}

static void read_session(sess_t *x, uint32_t now)
{
    int n = recv(x->fd, x->buf + x->fill, sizeof(x->buf) - x->fill, 0);
    if (n == 0) {
        sess_close(x, "peer closed");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            sess_close(x, "receive error");
        }
        return;
    }
    x->fill = (uint16_t)(x->fill + n);
    x->last_rx_ms = now;
    s_traffic.bytes_in += (uint32_t)n;

    while (x->fd >= 0 && x->fill >= 2) {
        uint16_t flen = lg_rd16(x->buf);
        if (flen < LG_ENV_SIZE || flen > LG_FRAME_MAX) {
            sess_close(x, "bad frame length");
            return;
        }
        if (x->fill < 2u + flen) {
            break;
        }
        uint32_t before = x->device;
        s_gather.x = x;
        s_gather.n = 0;
        lg_node_on_session_frame(&g_app.core, &x->device, x->buf + 2, flen);
        gather_flush(true);
        if (x->fd < 0) {
            return;   /* closed by a send failure during handling */
        }
        if (before == 0 && x->device != 0) {
            on_registered(x);
        }
        uint16_t consumed = (uint16_t)(2u + flen);
        memmove(x->buf, x->buf + consumed, x->fill - consumed);
        x->fill = (uint16_t)(x->fill - consumed);
    }
}

void sess_poll(int timeout_ms)
{
    if (s_listen_fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_listen_fd, &rfds);
    int maxfd = s_listen_fd;
    for (size_t i = 0; i < SESS_MAX; i++) {
        if (s_sess[i].fd >= 0) {
            FD_SET(s_sess[i].fd, &rfds);
            if (s_sess[i].fd > maxfd) {
                maxfd = s_sess[i].fd;
            }
        }
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = timeout_ms * 1000 };
    int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    uint32_t now = app_now_ms();

    if (ready > 0) {
        if (FD_ISSET(s_listen_fd, &rfds)) {
            accept_one(now);
        }
        for (size_t i = 0; i < SESS_MAX; i++) {
            sess_t *x = &s_sess[i];
            if (x->fd >= 0 && FD_ISSET(x->fd, &rfds)) {
                read_session(x, now);
            }
        }
    }

    for (size_t i = 0; i < SESS_MAX; i++) {
        sess_t *x = &s_sess[i];
        if (x->fd < 0) {
            continue;
        }
        if (x->device == 0 && now - x->opened_ms > SESS_REGISTER_MS) {
            sess_close(x, "no registration");
        } else if (now - x->last_rx_ms > SESS_IDLE_MS) {
            sess_close(x, "idle timeout");
        }
    }
}

/*
 * Writes bytes that hold whole frames to x, or closes x.
 * This runs on the core task, which serves every session and the backbone, so it must not
 * wait long for one slow handheld. It used to wait up to SESS_IDLE_MS: a listener that fell
 * behind during push-to-talk froze the AP, the talker's socket filled, and every handheld on
 * the AP dropped within seconds (D61). A talk frame that cannot go out at once is dropped
 * for that listener -- a tenth of a second of their audio -- and anything else waits at most
 * SEND_WAIT_MS. Bytes already partly sent must finish or the session closes, because the
 * stream would be out of step.
 */
static void write_frames(sess_t *x, const uint8_t *out, size_t total, bool voice)
{
    size_t sent = 0;
    uint32_t started = app_now_ms();
    while (sent < total) {
        int n = send(x->fd, out + sent, total - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (voice && sent == 0) {
                s_voice_dropped++;
                return;
            }
            if (app_now_ms() - started > SEND_WAIT_MS) {
                break;
            }
            vTaskDelay(1);
            continue;
        }
        break;
    }
    uint32_t waited = app_now_ms() - started;
    if (waited > s_traffic.slowest_send_ms) {
        s_traffic.slowest_send_ms = waited;
    }
    s_traffic.bytes_out += (uint32_t)sent;
    if (sent < total) {
        if (waited > SEND_WAIT_MS) {
            s_traffic.send_timeouts++;
        }
        sess_close(x, "send failed");
    }
}

/* Sends what is gathered for s_gather.x; gathering stays on unless stop. */
static void gather_flush(bool stop)
{
    sess_t *x = s_gather.x;
    uint16_t n = s_gather.n;
    s_gather.n = 0;
    if (stop) {
        s_gather.x = NULL;
    }
    if (x != NULL && n > 0 && x->fd >= 0) {
        write_frames(x, s_gather.buf, n, false);   /* a close here only sends to other handhelds */
    }
}

void sess_send(uint32_t device, const uint8_t *frame, size_t len)
{
    if (len > LG_FRAME_MAX) {
        return;
    }
    /* A handheld that rebooted can still have its old session here while it registers again;
     * the replies (REGISTER_ACK first) belong to the newest session, not the first slot. */
    sess_t *newest = NULL;
    for (size_t i = 0; i < SESS_MAX; i++) {
        sess_t *c = &s_sess[i];
        if (c->fd >= 0 && c->device == device && (newest == NULL || c->opened_ms >= newest->opened_ms)) {
            newest = c;
        }
    }
    if (newest == NULL) {
        return;
    }
    sess_t *x = newest;
    bool voice = len > 1 && frame[1] == LG_T_VOICE;
    if (x == s_gather.x) {
        if (!voice && s_gather.n + 2u + len <= sizeof(s_gather.buf)) {
            lg_wr16(s_gather.buf + s_gather.n, (uint16_t)len);
            memcpy(s_gather.buf + s_gather.n + 2, frame, len);
            s_gather.n = (uint16_t)(s_gather.n + 2u + len);
            return;
        }
        gather_flush(false);   /* keep the order: what is gathered goes first */
        if (x->fd < 0) {
            return;
        }
        if (!voice && 2u + len <= sizeof(s_gather.buf)) {
            lg_wr16(s_gather.buf, (uint16_t)len);
            memcpy(s_gather.buf + 2, frame, len);
            s_gather.n = (uint16_t)(2u + len);
            return;
        }
    }
    uint8_t out[2 + LG_FRAME_MAX];   /* one write: a header sent apart would take its own buffer */
    lg_wr16(out, (uint16_t)len);
    memcpy(out + 2, frame, len);
    write_frames(x, out, 2 + len, voice);
}

uint8_t sess_registered_count(void)
{
    uint8_t n = 0;
    for (size_t i = 0; i < SESS_MAX; i++) {
        if (s_sess[i].fd >= 0 && s_sess[i].device != 0) {
            n++;
        }
    }
    return n;
}

void sess_print(void)
{
    uint32_t now = app_now_ms();
    printf("Handheld sessions on this node:\n");
    printf("  DEVICE  ADDRESS          IDLE_MS\n");
    for (size_t i = 0; i < SESS_MAX; i++) {
        const sess_t *x = &s_sess[i];
        if (x->fd >= 0) {
            printf("  %-6" PRIu32 "  %-15s  %" PRIu32 "\n", x->device, x->addr, now - x->last_rx_ms);
        }
    }
}

uint32_t sess_voice_dropped(void)
{
    return s_voice_dropped;
}

void sess_traffic(sess_traffic_t *out)
{
    *out = s_traffic;
    uint8_t open_now = 0, registered = 0;
    for (size_t i = 0; i < SESS_MAX; i++) {
        if (s_sess[i].fd >= 0) {
            open_now++;
            if (s_sess[i].device != 0) {
                registered++;
            }
        }
    }
    out->open_now = open_now;
    out->registered_now = registered;
}
