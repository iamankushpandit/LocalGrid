/*
 * ble_status.c - the sealed BLE status beacon (D68). Layout: docs/ble-status.md.
 *
 * Runs on the core task, which owns every source it reads (the lg_core node, backbone links,
 * sessions, grid state, grid time), so nothing is copied under a lock and no other task touches
 * core state. Sealing 14 bytes costs well under a millisecond once every 500 ms. The frame is then
 * copied to ble_adv, whose host task applies it, so this task never waits on the radio.
 *
 * Nonces: (AP index, boot counter, frame counter). The boot counter is committed to NVS before any
 * radio transmit (next_boot_counter), and the counter only grows within a boot. At the u24 wrap the
 * beacon stops until the AP restarts rather than reuse a nonce: 2^24 frames at 2 a second is 97 days.
 */
#include "ble_status.h"

#include <string.h>

#include "backbone.h"
#include "ble_adv.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gps.h"
#include "grid_state.h"
#include "lg_crypto.h"
#include "node_app.h"
#include "sessions.h"

static const char *TAG = "BLE";

#define STATUS_INTERVAL_MS 500u
#define STATUS_MAGIC       0x53u        /* 'S' */
#define HDR_LEN            9u
#define PT_MAX             14u
#define TAG_CUT            4u
#define FRAME_MAX          (HDR_LEN + PT_MAX + TAG_CUT)   /* 27 */
#define COUNTER_MAX        0xFFFFFFu     /* u24 */
#define BATTERY_PAIRS      7u
#define NAME_TEXT_MAX      13u
#define STRATUM_FIELD_MAX  31u
#define DEVICE_BYTE_MAX    255u
#define NIBBLE_DEVICES     20u
#define NIBBLE_NONE        0xFu

enum { FT_HEALTH = 1, FT_HANDHELDS = 2, FT_ALERT = 3, FT_BATTERIES = 4, FT_NAME = 5 };

static struct {
    bool     ready;          /* key derived */
    bool     stopped;        /* the counter ran out this boot */
    uint8_t  key[LG_AEAD_KEY_LEN];
    uint32_t counter;        /* the next frame's counter */
    uint32_t last_ms;
    uint32_t battery_next;   /* where the next batteries frame starts, when more than 7 are here */
    uint32_t name_next;      /* which named device the next name frame carries */
} st;

static uint8_t sat8(uint32_t v, uint32_t max)
{
    return (uint8_t)(v > max ? max : v);
}

esp_err_t ble_status_init(const uint8_t backbone_key[32])
{
    static const char salt[] = "LG-BLE-STATUS-1";
    static const char info[] = "ble status";
    if (lg_hkdf_sha256((const uint8_t *)salt, sizeof(salt) - 1, backbone_key, 32, (const uint8_t *)info,
                       sizeof(info) - 1, st.key, sizeof(st.key)) != 0) {
        ESP_LOGE(TAG, "[BLE] Status beacon off: key derivation failed");
        return ESP_FAIL;
    }
    st.ready = true;
    ESP_LOGI(TAG, "[BLE] Status beacon sealed in the scan response, a frame every %u ms (D68)",
             STATUS_INTERVAL_MS);
    return ESP_OK;
}

/* ---- the five frame types; each returns its plaintext length (<= PT_MAX) ---- */

static size_t build_health(uint8_t *p)
{
    uint64_t up_min = (uint64_t)esp_timer_get_time() / 60000000u;
    lg_wr16(p, (uint16_t)(up_min > 0xFFFFu ? 0xFFFFu : up_min));

    uint8_t links_up = 0;
    lgbb_link_info_t links[LG_MAX_NODES];
    size_t n = lgbb_links(links, LG_MAX_NODES, app_now_ms());
    for (size_t i = 0; i < n; i++) {
        if (links[i].up && links[i].node < 8u) {
            links_up |= (uint8_t)(1u << links[i].node);
        }
    }
    p[2] = links_up;
    p[3] = sess_registered_count();

    uint32_t set_unix, generation;
    uint16_t set_on;
    bool by_gps = false;
    grid_state_time_info(&set_unix, &set_on, &generation, &by_gps);
    uint8_t q = g_app.time_quality & 0x03u;
    uint8_t stratum = q == LG_TIME_UNSET ? (uint8_t)STRATUM_FIELD_MAX : sat8(g_app.time_stratum, STRATUM_FIELD_MAX);
    p[4] = (uint8_t)(q | (q != LG_TIME_UNSET && by_gps ? 0x04u : 0u) | (uint8_t)(stratum << 3));

    p[5] = sat8(esp_get_minimum_free_heap_size() / 1024u, 255u);
    p[6] = (uint8_t)esp_reset_reason();
    p[7] = (uint8_t)(g_app.boot > 0 ? g_app.boot - 1u : 0u);   /* every boot but the first is a restart */
    p[8] = (uint8_t)node_brownouts();

    gps_state_t g;
    gps_state(&g);   /* never started on an AP other than MAIN: not fitted */
    p[9] = (uint8_t)((g.heard ? 0x01u : 0u) | (g.fix ? 0x02u : 0u) | (uint8_t)(sat8(g.sats, 63u) << 2));
    return 10;
}

static size_t build_handhelds(uint8_t *p)
{
    uint32_t online = 0;
    memset(p + 4, 0xFF, 10);   /* every nibble 0xF: not online or unknown */
    const lg_roster_t *r = g_app.core.roster;
    for (size_t i = 0; i < r->n_users; i++) {
        uint32_t dev = r->users[i].device;
        const lg_presence_entry_t *pr = lg_node_presence(&g_app.core, dev);
        if (dev == 0 || dev > 32u || pr == NULL || pr->state != LG_PRES_ONLINE) {
            continue;
        }
        online |= 1u << (dev - 1u);
        if (dev <= NIBBLE_DEVICES && pr->node < NIBBLE_NONE) {
            uint8_t *b = &p[4 + (dev - 1u) / 2u];
            if (((dev - 1u) & 1u) == 0) {
                *b = (uint8_t)((*b & 0xF0u) | pr->node);
            } else {
                *b = (uint8_t)((*b & 0x0Fu) | (pr->node << 4));
            }
        }
    }
    lg_wr32(p, online);
    return 14;
}

static size_t build_alert(uint8_t *p)
{
    lg_urgent_info_t u;
    memset(p, 0, 10);
    if (lg_node_urgent(&g_app.core, &u)) {
        p[0] = (uint8_t)((u.active ? 0x01u : 0u) | (u.all_clear ? 0x02u : 0u));
        p[1] = (uint8_t)(u.author <= DEVICE_BYTE_MAX ? u.author : 0u);
        uint32_t secs = u.age_ms / 1000u;
        lg_wr16(p + 2, (uint16_t)(secs > 0xFFFFu ? 0xFFFFu : secs));
        p[4] = u.reads;   /* counted by reader; this AP sees the reports that pass it */
    }
    lg_wr32(p + 6, app_grid_time());
    return 10;
}

static bool registered_here(uint32_t dev)
{
    const lg_presence_entry_t *pr = lg_node_presence(&g_app.core, dev);
    return pr != NULL && pr->state == LG_PRES_ONLINE && pr->node == g_app.index;
}

static size_t build_batteries(uint8_t *p)
{
    uint8_t here[LG_MAX_DEVICES];
    size_t n = 0;
    const lg_roster_t *r = g_app.core.roster;
    for (size_t i = 0; i < r->n_users && n < LG_MAX_DEVICES; i++) {
        uint32_t dev = r->users[i].device;
        if (dev != 0 && dev <= DEVICE_BYTE_MAX && registered_here(dev)) {
            here[n++] = (uint8_t)dev;
        }
    }
    size_t take = n < BATTERY_PAIRS ? n : BATTERY_PAIRS;
    size_t start = n > BATTERY_PAIRS ? st.battery_next % n : 0;   /* more than 7: take turns */
    st.battery_next += BATTERY_PAIRS;
    for (size_t k = 0; k < take; k++) {
        uint8_t dev = here[(start + k) % n];
        p[2 * k] = dev;
        p[2 * k + 1] = lg_node_battery(&g_app.core, dev);
    }
    return 2 * take;
}

/* Returns 0 when this AP knows no chosen name: the caller sends AP health in its turn. */
static size_t build_name(uint8_t *p)
{
    const lg_name_t *named[LG_MAX_DEVICES];
    size_t n = 0;
    const lg_roster_t *r = g_app.core.roster;
    for (size_t i = 0; i < r->n_users && n < LG_MAX_DEVICES; i++) {
        uint32_t dev = r->users[i].device;
        const lg_name_t *nm = lg_node_name(&g_app.core, dev);
        if (dev != 0 && dev <= DEVICE_BYTE_MAX && nm != NULL && nm->len > 0) {
            named[n++] = nm;
        }
    }
    if (n == 0) {
        return 0;
    }
    const lg_name_t *nm = named[st.name_next++ % n];
    size_t len = nm->len < NAME_TEXT_MAX ? nm->len : NAME_TEXT_MAX;
    if (len < nm->len) {
        while (len > 0 && ((uint8_t)nm->text[len] & 0xC0u) == 0x80u) {
            len--;   /* never cut inside a character */
        }
    }
    p[0] = (uint8_t)nm->device;
    memcpy(p + 1, nm->text, len);
    return 1 + len;
}

/* ---- sealing ---- */

static void send_next(void)
{
    uint32_t c = st.counter;
    uint8_t type = (c & 1u) == 0 ? (uint8_t)FT_HEALTH : (uint8_t)(FT_HANDHELDS + (c / 2u) % 4u);
    uint8_t pt[PT_MAX];
    size_t pt_len = 0;
    switch (type) {
    case FT_HANDHELDS: pt_len = build_handhelds(pt); break;
    case FT_ALERT:     pt_len = build_alert(pt); break;
    case FT_BATTERIES: pt_len = build_batteries(pt); break;
    case FT_NAME:
        pt_len = build_name(pt);
        if (pt_len == 0) {
            type = FT_HEALTH;
            pt_len = build_health(pt);
        }
        break;
    default:           pt_len = build_health(pt); break;
    }

    uint8_t ap = (uint8_t)(g_app.index & 0x0Fu);
    uint8_t frame[FRAME_MAX];
    frame[0] = STATUS_MAGIC;
    frame[1] = (uint8_t)(ap | (type << 4));
    lg_wr32(frame + 2, g_app.boot);
    frame[6] = (uint8_t)c;
    frame[7] = (uint8_t)(c >> 8);
    frame[8] = (uint8_t)(c >> 16);

    uint8_t nonce[LG_AEAD_NONCE_LEN] = { 0 };
    nonce[0] = ap;
    lg_wr32(nonce + 2, g_app.boot);
    nonce[6] = frame[6];
    nonce[7] = frame[7];
    nonce[8] = frame[8];

    uint8_t sealed[PT_MAX + LG_AEAD_TAG_BYTES];
    int n = lg_aead_seal(st.key, nonce, frame, HDR_LEN, pt, pt_len, sealed);
    st.counter++;   /* spent even if sealing failed: a counter is never offered twice */
    if (n != (int)(pt_len + LG_AEAD_TAG_BYTES)) {
        ESP_LOGW(TAG, "[BLE] Status frame %lu not sealed", (unsigned long)c);
        return;
    }
    memcpy(frame + HDR_LEN, sealed, pt_len + TAG_CUT);   /* ciphertext, then the first 4 tag bytes */
    ble_adv_set_status(frame, HDR_LEN + pt_len + TAG_CUT);
}

void ble_status_poll(uint32_t now_ms)
{
    if (!st.ready || st.stopped || now_ms - st.last_ms < STATUS_INTERVAL_MS) {
        return;
    }
    /* Keep the 500 ms cadence against the core loop's jitter, but never burst after a stall. */
    st.last_ms = now_ms - st.last_ms < 2u * STATUS_INTERVAL_MS ? st.last_ms + STATUS_INTERVAL_MS : now_ms;
    if (st.counter > COUNTER_MAX) {
        st.stopped = true;
        ble_adv_set_status(NULL, 0);
        ESP_LOGW(TAG, "[BLE] Status beacon stopped: frame counter used up this boot; it resumes after a restart");
        return;
    }
    send_next();
}
