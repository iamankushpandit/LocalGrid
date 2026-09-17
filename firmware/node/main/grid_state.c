#include "grid_state.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backbone.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lg_node.h"
#include "node_app.h"
#include "nvs.h"
#include "power_trace.h"

static const char *TAG = "GRID";

/*
 * GRID_STATE bodies. The first byte says which kind:
 *
 * GS_LAYOUT (4): settings and this AP's health, exactly GS_LEN bytes, little-endian:
 *     0  u8    layout
 *     1  u32   settings seq            5  u16  settings author       7  u8  configured
 *     8  33 B  grid name              41  48 B time zone             89 16 B PBKDF2 salt
 *   105  u32   PBKDF2 iterations     109  32 B PBKDF2 hash
 *   141  u32   time generation       145  u16  time author          147 u32 time set at (Unix s)
 *   151  16 B  AP name               167  u32  uptime s             171 u8  time quality
 *   172  u16   last sync from AP     174  u32  seconds since that sync (0xFFFFFFFF never)
 *   178  i16   correction at that sync, ms
 *   180  u32   boot counter          184  u8   reset reason          185 u32 previous run s
 *   189  u8    time stratum
 * AV_KIND (0x80): u32 grid minute of the newest entry, u8 count, then count x (u8 ap, 30 B packed)
 * INC_KIND (0x81): u8 count, then count x grid_incident_t (16 B each, as stored)
 * Every frame travels sealed under the backbone key, like every backbone frame.
 */
#define GS_LAYOUT          4u
#define GS_LEN             190u
#define AV_KIND            0x80u
#define INC_KIND           0x81u
#define CAUSE_WAIT_MS      60000u    /* after an AP is back, how long to wait for its reason */
#define AV_ANNOUNCE_MS     300000u
#define AV_GAP_GIVE_UP_MS  180000u   /* no grid time this long after boot: keep history, skip the gap */
#define STICKY_SAVE_MS     600000u
#define GS_ITERATIONS_MIN  1000u
#define GS_ITERATIONS_MAX  200000u

_Static_assert(GS_LEN <= LG_GRID_STATE_MAX, "grid state body exceeds the core's limit");
_Static_assert(SETTINGS_GRID_NAME_MAX + 1 == 33 && SETTINGS_TZ_MAX + 1 == 48, "grid state layout needs updating");

/* ---- sticky records (D48), packed (D49) ---- */

#define STICKY_MARK  0x4C475335u   /* "LGS5": layout of sticky_t below */
typedef struct {
    uint32_t        mark;
    uint32_t        time_gen;
    uint32_t        time_set_unix;
    uint16_t        time_author;
    uint8_t         incident_next;
    uint8_t         incident_count;
    uint32_t        av_newest_min;   /* grid minute of the newest availability entry, 0 unanchored */
    uint8_t         avail[LG_MAX_NODES][GRID_AVAIL_BYTES];
    grid_incident_t incidents[GRID_INCIDENTS];
} sticky_t;

/* Survives a crash, watchdog, software restart, or brownout; a power-on reset clears it. */
static RTC_NOINIT_ATTR sticky_t k;

/* ---- working state, RAM only ---- */

static struct {
    SemaphoreHandle_t lock;
    uint16_t          self;
    node_settings_t   settings;
    bool              heard_other;
    uint16_t          sync_from;
    uint32_t          sync_ms;
    int16_t           sync_drift_ms;
    grid_ap_info_t    aps[LG_MAX_NODES];
    uint32_t          ap_heard_ms[LG_MAX_NODES];
    uint16_t          avail_up_s[LG_MAX_NODES];
    uint16_t          avail_seen_s[LG_MAX_NODES];
    uint32_t          avail_minute;
    bool              avail_gap_pending;
    uint32_t          last_avail_announce_ms;
    uint32_t          last_save_ms;
    bool              save_due;
    bool              was_up[LG_MAX_NODES];
    uint32_t          down_since_ms[LG_MAX_NODES];
    int16_t           open_incident[LG_MAX_NODES];
    uint32_t          back_ms[LG_MAX_NODES];
} s = { .sync_from = GRID_NO_AP };

/* ---- helpers ---- */

static inline void avail_set(uint8_t *packed, size_t m, uint8_t v)
{
    uint8_t shift = (uint8_t)((m % 4u) * 2u);
    packed[m / 4u] = (uint8_t)((packed[m / 4u] & ~(3u << shift)) | ((v & 3u) << shift));
}

/* Drops the oldest minute and appends v as the newest. */
static void avail_push(uint16_t ap, uint8_t v)
{
    uint8_t *p = k.avail[ap];
    for (size_t m = 0; m + 1u < GRID_AVAIL_MINUTES; m++) {
        avail_set(p, m, grid_avail_get(p, m + 1u));
    }
    avail_set(p, GRID_AVAIL_MINUTES - 1u, v);
}

static void incident_set_kind(grid_incident_t *in, uint8_t kind, uint8_t reset)
{
    in->kind_reset = (uint8_t)((kind & 3u) | ((reset & 0x3Fu) << 2));
}

static uint16_t sat16(uint32_t v)
{
    return (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
}

static uint16_t run_minutes(uint32_t prev_run_s)
{
    return prev_run_s == UINT32_MAX ? 0xFFFFu : sat16(prev_run_s / 60u);
}

const char *grid_state_reset_text(uint8_t reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:  return "power-on or reset";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "crash (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "hang (task watchdog)";
    case ESP_RST_WDT:      return "crash (other watchdog)";
    case ESP_RST_BROWNOUT: return "low supply voltage (brownout)";
    default:               return "unknown reset";
    }
}

static const char *ap_name(uint16_t ap)
{
    if (ap == s.self) {
        return g_app.name;
    }
    return ap < LG_MAX_NODES && s.aps[ap].valid && s.aps[ap].name[0] ? s.aps[ap].name : "an AP";
}

static void incident_log(const grid_incident_t *in)
{
    switch (grid_incident_kind(in)) {
    case GRID_INCIDENT_RESTARTED:
        ESP_LOGW(TAG, "[GRID] AP %u %s was unreachable for %u s: it restarted after %s", in->ap, ap_name(in->ap),
                 in->duration_s, grid_state_reset_text(grid_incident_reset(in)));
        break;
    case GRID_INCIDENT_LINK:
        ESP_LOGW(TAG, "[GRID] AP %u %s was unreachable for %u s but kept running: the backbone link dropped", in->ap,
                 ap_name(in->ap), in->duration_s);
        break;
    case GRID_INCIDENT_UNEXPLAINED:
        ESP_LOGW(TAG, "[GRID] AP %u %s was unreachable for %u s and never said why", in->ap, ap_name(in->ap),
                 in->duration_s);
        break;
    default:
        ESP_LOGW(TAG, "[GRID] AP %u %s is unreachable", in->ap, ap_name(in->ap));
        break;
    }
}

static grid_incident_t *incident_append(void)
{
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        if (s.open_incident[i] == (int16_t)k.incident_next) {
            s.open_incident[i] = -1;   /* the ring is about to overwrite it */
        }
    }
    grid_incident_t *in = &k.incidents[k.incident_next];
    memset(in, 0, sizeof(*in));
    k.incident_next = (uint8_t)((k.incident_next + 1u) % GRID_INCIDENTS);
    if (k.incident_count < GRID_INCIDENTS) {
        k.incident_count++;
    }
    return in;
}

static grid_incident_t *incident_open(uint16_t ap)
{
    int16_t index = (int16_t)k.incident_next;
    grid_incident_t *in = incident_append();
    in->down_grid_time = app_grid_time();
    in->ap = (uint8_t)ap;
    in->seen_by = (uint8_t)s.self;
    in->prev_run_min = 0xFFFFu;
    in->boot = s.aps[ap].valid ? (uint16_t)s.aps[ap].boot : 0u;   /* a different boot on return means it restarted */
    incident_set_kind(in, GRID_INCIDENT_DOWN, 0);
    s.open_incident[ap] = index;
    return in;
}

/* An incident another AP recorded: keep it unless one for the same AP within 90 s is known. */
static void incident_merge(const grid_incident_t *in)
{
    for (uint8_t i = 0; i < k.incident_count; i++) {
        const grid_incident_t *e = &k.incidents[i];
        int64_t d = (int64_t)e->down_grid_time - (int64_t)in->down_grid_time;
        if (e->ap == in->ap && e->down_grid_time != 0 && d > -90 && d < 90) {
            return;
        }
    }
    *incident_append() = *in;
    s.save_due = true;
}

/* ---- flash copy of the sticky record ---- */

static void sticky_save(void)
{
    nvs_handle_t h;
    if (nvs_open("lgst", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    ptrace_event(PTRACE_NVS_WRITE);
    if (nvs_set_blob(h, "k", &k, sizeof(k)) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
    s.save_due = false;
    s.last_save_ms = app_now_ms();
}

static bool sticky_valid(const sticky_t *r)
{
    return r->mark == STICKY_MARK && r->incident_next < GRID_INCIDENTS && r->incident_count <= GRID_INCIDENTS;
}

static void sticky_boot(void)
{
    const char *from = "RTC memory";
    if (!sticky_valid(&k)) {
        static sticky_t flash;   /* 0.5 KB: kept off the stack */
        size_t len = sizeof(flash);
        nvs_handle_t h;
        bool loaded = false;
        if (nvs_open("lgst", NVS_READONLY, &h) == ESP_OK) {
            loaded = nvs_get_blob(h, "k", &flash, &len) == ESP_OK && len == sizeof(flash) && sticky_valid(&flash);
            nvs_close(h);
        }
        if (loaded) {
            k = flash;
            from = "flash (RTC memory was cleared by a power-on reset)";
        } else {
            memset(&k, 0, sizeof(k));
            k.mark = STICKY_MARK;
            ESP_LOGI(TAG, "[GRID] No history kept from before this start");
            return;
        }
    }
    for (uint8_t i = 0; i < k.incident_count; i++) {
        if (grid_incident_kind(&k.incidents[i]) == GRID_INCIDENT_DOWN) {
            incident_set_kind(&k.incidents[i], GRID_INCIDENT_UNEXPLAINED, 0);   /* this run cannot explain it */
        }
    }
    s.avail_gap_pending = k.av_newest_min != 0;
    ESP_LOGI(TAG, "[GRID] History kept from %s: %u incident(s), time setting generation %" PRIu32, from,
             k.incident_count, k.time_gen);
}

/* Once grid time is known after a restart: the minutes this AP was down go into the history. */
static void avail_fill_gap(void)
{
    uint32_t now_min = app_grid_time() / 60u;
    if (k.av_newest_min != 0 && now_min > k.av_newest_min + 1u) {
        uint32_t gap = now_min - k.av_newest_min - 1u;
        gap = gap > GRID_AVAIL_MINUTES ? GRID_AVAIL_MINUTES : gap;
        for (uint32_t m = 0; m < gap; m++) {
            for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
                avail_push(ap, ap == s.self ? GRID_AVAIL_DOWN : GRID_AVAIL_UNKNOWN);
            }
        }
        k.av_newest_min = now_min - 1u;
        ESP_LOGI(TAG, "[GRID] Availability: %" PRIu32 " minute(s) down added for this AP", gap);
    }
    s.avail_gap_pending = false;
}

/* ---- sharing the history ---- */

static void incidents_announce(void)
{
    uint8_t body[LG_GRID_STATE_MAX];
    size_t n = 2;
    uint8_t count = 0;
    body[0] = INC_KIND;
    for (uint8_t j = 0; j < k.incident_count && n + sizeof(grid_incident_t) <= sizeof(body); j++) {
        const grid_incident_t *in = &k.incidents[(k.incident_next + GRID_INCIDENTS - 1u - j) % GRID_INCIDENTS];
        if (grid_incident_kind(in) == GRID_INCIDENT_DOWN || in->down_grid_time == 0) {
            continue;   /* still open, or no grid time to line it up by */
        }
        memcpy(body + n, in, sizeof(*in));   /* little-endian on every ESP32, stored as sent */
        n += sizeof(*in);
        count++;
    }
    body[1] = count;
    if (count > 0) {
        (void)lg_node_announce_grid_state(&g_app.core, body, n);
    }
}

void grid_state_avail_announce(void)
{
    incidents_announce();
    if (k.av_newest_min == 0 || s.avail_gap_pending) {
        return;   /* not anchored to grid time, so nobody could line it up */
    }
    uint8_t body[LG_GRID_STATE_MAX];
    size_t n = 6;
    uint8_t count = 0;
    body[0] = AV_KIND;
    lg_wr32(body + 1, k.av_newest_min);
    for (uint16_t ap = 0; ap < LG_MAX_NODES && n + 1u + GRID_AVAIL_BYTES <= sizeof(body); ap++) {
        bool any = false;
        for (size_t b = 0; b < GRID_AVAIL_BYTES && !any; b++) {
            any = k.avail[ap][b] != 0;
        }
        if (!any) {
            continue;
        }
        body[n] = (uint8_t)ap;
        memcpy(body + n + 1u, k.avail[ap], GRID_AVAIL_BYTES);
        n += 1u + GRID_AVAIL_BYTES;
        count++;
    }
    body[5] = count;
    if (count > 0) {
        (void)lg_node_announce_grid_state(&g_app.core, body, n);
    }
}

static void incidents_on_frame(const uint8_t *body, size_t len)
{
    if (len < 2u || len != 2u + (size_t)body[1] * sizeof(grid_incident_t)) {
        return;
    }
    for (uint8_t j = 0; j < body[1]; j++) {
        grid_incident_t in;
        memcpy(&in, body + 2u + (size_t)j * sizeof(in), sizeof(in));
        uint8_t kind = grid_incident_kind(&in);
        if (in.ap < LG_MAX_NODES && kind != GRID_INCIDENT_DOWN && in.down_grid_time != 0) {
            incident_merge(&in);
        }
    }
}

/* Another AP's history: fill only minutes this AP has no record of. */
static void avail_on_frame(uint16_t origin_node, const uint8_t *body, size_t len)
{
    if (len < 6u || k.av_newest_min == 0 || s.avail_gap_pending) {
        return;
    }
    uint32_t their_newest = lg_rd32(body + 1);
    uint8_t count = body[5];
    if (their_newest == 0 || len != 6u + (size_t)count * (1u + GRID_AVAIL_BYTES)) {
        return;
    }
    int64_t shift = (int64_t)their_newest - (int64_t)k.av_newest_min;   /* their index = mine + shift */
    uint32_t filled = 0;
    for (uint8_t j = 0; j < count; j++) {
        const uint8_t *e = body + 6u + (size_t)j * (1u + GRID_AVAIL_BYTES);
        uint8_t ap = e[0];
        if (ap >= LG_MAX_NODES) {
            continue;
        }
        for (size_t m = 0; m < GRID_AVAIL_MINUTES; m++) {
            int64_t their = (int64_t)m + shift;
            if (their < 0 || their >= (int64_t)GRID_AVAIL_MINUTES || grid_avail_get(k.avail[ap], m) != 0) {
                continue;
            }
            uint8_t v = grid_avail_get(e + 1u, (size_t)their);
            if (v != GRID_AVAIL_UNKNOWN) {
                avail_set(k.avail[ap], m, v);
                filled++;
            }
        }
    }
    if (filled > 0) {
        s.save_due = true;
        ESP_LOGI(TAG, "[GRID] Availability: filled %" PRIu32 " minute(s) from AP %u's record", filled, origin_node);
    }
}

/* ---- settings and time generation ---- */

static bool newer(uint32_t seq_a, uint16_t author_a, uint32_t seq_b, uint16_t author_b)
{
    return seq_a > seq_b || (seq_a == seq_b && author_a > author_b);
}

void grid_state_init(uint16_t self)
{
    s.lock = xSemaphoreCreateMutex();
    s.self = self;
    for (uint16_t i = 0; i < LG_MAX_NODES; i++) {
        s.open_incident[i] = -1;
    }
    sticky_boot();
    esp_err_t err = settings_load(&s.settings);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[GRID] Settings unreadable (%s); waiting for another AP or setup", esp_err_to_name(err));
        memset(&s.settings, 0, sizeof(s.settings));
    }
    ESP_LOGI(TAG, "[GRID] Settings version %" PRIu32 " from AP %u (%s)", s.settings.seq, s.settings.author,
             s.settings.configured ? "set up" : "not set up");
}

void grid_state_settings(node_settings_t *out)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.settings;
    xSemaphoreGive(s.lock);
}

esp_err_t grid_state_commit(const node_settings_t *in)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    node_settings_t next = *in;
    next.seq = s.settings.seq + 1u;
    next.author = s.self;
    ptrace_event(PTRACE_NVS_WRITE);
    esp_err_t err = settings_save(&next);
    if (err == ESP_OK) {
        s.settings = next;
    }
    xSemaphoreGive(s.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[GRID] Settings version %" PRIu32 " made on this AP", next.seq);
        node_cmd_t cmd = { .type = NODE_CMD_GRID_ANNOUNCE };
        (void)xQueueSend(g_app.cmd_queue, &cmd, pdMS_TO_TICKS(500));
    }
    return err;
}

bool grid_state_setup_allowed(void)
{
    return s.heard_other || lgbb_link_count() == 0;
}

void grid_state_time_set_here(uint32_t unix_s)
{
    k.time_gen++;
    k.time_author = s.self;
    k.time_set_unix = unix_s;
    s.sync_from = s.self;
    s.sync_ms = app_now_ms();
    s.sync_drift_ms = 0;
    s.save_due = true;
}

void grid_state_note_sync(uint16_t from_ap, int32_t correction_ms)
{
    s.sync_from = from_ap;
    s.sync_ms = app_now_ms();
    s.sync_drift_ms = (int16_t)(correction_ms > INT16_MAX ? INT16_MAX : correction_ms < INT16_MIN ? INT16_MIN : correction_ms);
}

uint16_t grid_state_sync_source(void)
{
    return s.sync_from;
}

static uint8_t own_stratum(void)
{
    return g_app.time_quality == LG_TIME_UNSET ? LG_STRATUM_UNKNOWN : g_app.time_stratum;
}

static void encode(uint8_t out[GS_LEN])
{
    memset(out, 0, GS_LEN);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const node_settings_t *c = &s.settings;
    out[0] = GS_LAYOUT;
    lg_wr32(out + 1, c->seq);
    lg_wr16(out + 5, c->author);
    out[7] = c->configured ? 1u : 0u;
    memcpy(out + 8, c->grid_name, SETTINGS_GRID_NAME_MAX + 1);
    memcpy(out + 41, c->timezone, SETTINGS_TZ_MAX + 1);
    memcpy(out + 89, c->salt, SETTINGS_SALT_LEN);
    lg_wr32(out + 105, c->iterations);
    memcpy(out + 109, c->hash, SETTINGS_HASH_LEN);
    xSemaphoreGive(s.lock);
    lg_wr32(out + 141, k.time_gen);
    lg_wr16(out + 145, k.time_author);
    lg_wr32(out + 147, k.time_set_unix);
    strncpy((char *)out + 151, g_app.name != NULL ? g_app.name : "", GRID_AP_NAME_MAX);
    lg_wr32(out + 167, app_now_ms() / 1000u);
    out[171] = g_app.time_quality;
    lg_wr16(out + 172, s.sync_from);
    lg_wr32(out + 174, s.sync_from == GRID_NO_AP ? GRID_NEVER : (app_now_ms() - s.sync_ms) / 1000u);
    lg_wr16(out + 178, (uint16_t)s.sync_drift_ms);
    lg_wr32(out + 180, g_app.boot);
    out[184] = (uint8_t)esp_reset_reason();
    lg_wr32(out + 185, ptrace_prev_run_s());
    out[189] = own_stratum();
}

void grid_state_announce(void)
{
    uint8_t body[GS_LEN];
    encode(body);
    (void)lg_node_announce_grid_state(&g_app.core, body, GS_LEN);
    ptrace_event(PTRACE_GRID_ANNOUNCE);
}

/* An AP's announcement: explain an open outage, catch a restart too quick to see, help it. */
static void watch_ap(uint16_t ap, const uint8_t *body)
{
    grid_ap_info_t *a = &s.aps[ap];
    uint32_t boot = lg_rd32(body + 180);
    uint8_t reason = body[184];
    uint32_t prev_run = lg_rd32(body + 185);
    bool restarted = a->valid && boot != a->boot;
    int16_t open = s.open_incident[ap];
    uint32_t now = app_now_ms();

    /* A restarted AP announces itself as soon as its link is up, which can be before this AP's
     * once-a-second check notices the link came back. A new boot explains the outage whenever it
     * arrives; an unchanged boot explains it only once the link is back. */
    bool new_boot = open >= 0 && (uint16_t)boot != k.incidents[open].boot;
    if (open >= 0 && grid_incident_kind(&k.incidents[open]) == GRID_INCIDENT_DOWN && (new_boot || s.back_ms[ap] != 0)) {
        grid_incident_t *in = &k.incidents[open];
        if (s.back_ms[ap] == 0) {
            in->duration_s = sat16((now - s.down_since_ms[ap]) / 1000u);
            s.back_ms[ap] = now;
        }
        incident_set_kind(in, new_boot ? GRID_INCIDENT_RESTARTED : GRID_INCIDENT_LINK, reason);
        in->prev_run_min = new_boot ? run_minutes(prev_run) : 0xFFFFu;
        in->boot = (uint16_t)boot;
        s.open_incident[ap] = -1;
        incident_log(in);
        s.save_due = true;
    } else if (restarted && open < 0) {
        grid_incident_t *in = incident_open(ap);
        incident_set_kind(in, GRID_INCIDENT_RESTARTED, reason);
        in->prev_run_min = run_minutes(prev_run);
        in->boot = (uint16_t)boot;
        s.open_incident[ap] = -1;
        incident_log(in);
        s.save_due = true;
    }

    /* It has no grid time and this AP does: send it now, rather than let it wait (at most every 5 s). */
    static uint32_t last_help_ms;
    if (body[171] == LG_TIME_UNSET && g_app.time_quality != LG_TIME_UNSET &&
        (last_help_ms == 0 || now - last_help_ms > 5000u)) {
        last_help_ms = now;
        lg_node_announce_time(&g_app.core, g_app.time_quality);
        ESP_LOGI("TIME", "[TIME] AP %u has no grid time; sent ours", ap);
    }

    a->valid = true;
    a->ap = ap;
    memcpy(a->name, body + 151, GRID_AP_NAME_MAX);
    a->name[GRID_AP_NAME_MAX] = '\0';
    a->uptime_s = lg_rd32(body + 167);
    a->time_quality = body[171];
    a->sync_from = lg_rd16(body + 172);
    a->sync_age_s = lg_rd32(body + 174);
    a->sync_drift_ms = (int16_t)lg_rd16(body + 178);
    a->boot = boot;
    a->reset_reason = reason;
    a->prev_run_s = prev_run;
    a->stratum = body[189];
    s.ap_heard_ms[ap] = now;

    /* It restarted recently: send the history now so it recovers what it missed (D48). */
    static uint32_t last_resend_ms;
    if (a->uptime_s < 180u && (last_resend_ms == 0 || now - last_resend_ms > 60000u)) {
        last_resend_ms = now;
        grid_state_avail_announce();
    }
}

void grid_state_on_frame(uint16_t origin_node, const uint8_t *body, size_t len)
{
    if (len >= 1u && body[0] == AV_KIND) {
        avail_on_frame(origin_node, body, len);
        return;
    }
    if (len >= 1u && body[0] == INC_KIND) {
        incidents_on_frame(body, len);
        return;
    }
    if (len != GS_LEN || body[0] != GS_LAYOUT) {
        ESP_LOGW(TAG, "[GRID] Grid state from AP %u ignored: length %u or layout %u not understood", origin_node,
                 (unsigned)len, len > 0 ? body[0] : 0u);
        return;
    }
    s.heard_other = true;

    node_settings_t in;
    memset(&in, 0, sizeof(in));
    in.seq = lg_rd32(body + 1);
    in.author = lg_rd16(body + 5);
    in.configured = body[7] != 0;
    memcpy(in.grid_name, body + 8, SETTINGS_GRID_NAME_MAX + 1);
    memcpy(in.timezone, body + 41, SETTINGS_TZ_MAX + 1);
    memcpy(in.salt, body + 89, SETTINGS_SALT_LEN);
    in.iterations = lg_rd32(body + 105);
    memcpy(in.hash, body + 109, SETTINGS_HASH_LEN);
    uint32_t time_gen = lg_rd32(body + 141);
    uint16_t time_author = lg_rd16(body + 145);
    uint32_t time_set_unix = lg_rd32(body + 147);

    if (origin_node < LG_MAX_NODES && origin_node != s.self) {
        watch_ap(origin_node, body);
    }

    bool well_formed = body[7] <= 1u && in.grid_name[SETTINGS_GRID_NAME_MAX] == '\0' &&
                       in.timezone[SETTINGS_TZ_MAX] == '\0' &&
                       (!in.configured || (in.iterations >= GS_ITERATIONS_MIN && in.iterations <= GS_ITERATIONS_MAX));

    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool take = well_formed && newer(in.seq, in.author, s.settings.seq, s.settings.author);
    esp_err_t err = ESP_OK;
    if (take) {
        ptrace_event(PTRACE_NVS_WRITE);
        err = settings_save(&in);
        s.settings = in;   /* held in RAM even if flash refused it, so this AP still agrees with the grid */
    }
    xSemaphoreGive(s.lock);
    if (!well_formed) {
        ESP_LOGW(TAG, "[GRID] Settings from AP %u ignored: malformed", origin_node);
    } else if (take) {
        ESP_LOGI(TAG, "[GRID] Adopted settings version %" PRIu32 " (made on AP %u) via AP %u%s", in.seq, in.author,
                 origin_node, err == ESP_OK ? "" : "; not saved to flash");
    }

    if (newer(time_gen, time_author, k.time_gen, k.time_author)) {
        k.time_gen = time_gen;
        k.time_author = time_author;
        k.time_set_unix = time_set_unix;
        s.save_due = true;
        if (time_author != s.self) {
            app_time_follow_new_generation();   /* any stratum held was measured from the old setting */
            if (g_app.time_quality == LG_TIME_AUTHORITATIVE) {
                g_app.time_quality = LG_TIME_CARRIED;
                ESP_LOGI("TIME", "[TIME] Time was set more recently on AP %u; following it", time_author);
            }
        }
    }
}

/* ---- views ---- */

bool grid_state_ap(uint16_t ap, grid_ap_info_t *out)
{
    if (ap >= LG_MAX_NODES) {
        return false;
    }
    uint32_t now = app_now_ms();
    if (ap == s.self) {
        memset(out, 0, sizeof(*out));
        out->valid = true;
        out->self = true;
        out->ap = ap;
        strncpy(out->name, g_app.name != NULL ? g_app.name : "", GRID_AP_NAME_MAX);
        out->uptime_s = now / 1000u;
        out->time_quality = g_app.time_quality;
        out->stratum = own_stratum();
        out->sync_from = s.sync_from;
        out->sync_age_s = s.sync_from == GRID_NO_AP ? GRID_NEVER : (now - s.sync_ms) / 1000u;
        out->sync_drift_ms = s.sync_drift_ms;
        out->boot = g_app.boot;
        out->reset_reason = (uint8_t)esp_reset_reason();
        out->prev_run_s = ptrace_prev_run_s();
        return true;
    }
    if (!s.aps[ap].valid) {
        return false;
    }
    *out = s.aps[ap];
    uint32_t since = (now - s.ap_heard_ms[ap]) / 1000u;
    out->heard_age_s = since;
    out->uptime_s += since;
    if (out->sync_age_s != GRID_NEVER) {
        out->sync_age_s += since;
    }
    return true;
}

void grid_state_time_info(uint32_t *set_unix, uint16_t *set_on_ap, uint32_t *generation)
{
    *set_unix = k.time_set_unix;
    *set_on_ap = k.time_gen == 0 ? GRID_NO_AP : k.time_author;
    *generation = k.time_gen;
}

void grid_state_avail(uint16_t ap, uint8_t out[GRID_AVAIL_BYTES])
{
    memcpy(out, k.avail[ap < LG_MAX_NODES ? ap : 0], GRID_AVAIL_BYTES);
}

uint32_t grid_state_avail_newest_minute(void)
{
    return s.avail_gap_pending ? 0u : k.av_newest_min;
}

size_t grid_state_incidents(grid_incident_t *out, size_t max)
{
    size_t n = 0;
    uint32_t now = app_now_ms();
    for (uint8_t j = 0; j < k.incident_count && n < max; j++) {
        const grid_incident_t *in = &k.incidents[(k.incident_next + GRID_INCIDENTS - 1u - j) % GRID_INCIDENTS];
        out[n] = *in;
        if (grid_incident_kind(in) == GRID_INCIDENT_DOWN && in->ap < LG_MAX_NODES && s.back_ms[in->ap] == 0 &&
            s.open_incident[in->ap] >= 0) {
            out[n].duration_s = sat16((now - s.down_since_ms[in->ap]) / 1000u);
        }
        n++;
    }
    return n;
}

/* ---- once a second ---- */

void grid_state_avail_second(const bool up[LG_MAX_NODES])
{
    uint32_t now = app_now_ms();
    if (s.avail_gap_pending) {
        if (app_grid_time() != 0) {
            avail_fill_gap();
        } else if (now > AV_GAP_GIVE_UP_MS) {
            s.avail_gap_pending = false;
        }
    }

    uint32_t minute = now / 60000u;
    if (minute != s.avail_minute) {
        for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
            uint8_t v = GRID_AVAIL_UNKNOWN;
            if (s.avail_seen_s[ap] > 0) {
                v = s.avail_up_s[ap] == 0                     ? GRID_AVAIL_DOWN
                    : s.avail_up_s[ap] >= s.avail_seen_s[ap] ? GRID_AVAIL_UP
                                                              : GRID_AVAIL_PARTIAL;
            }
            avail_push(ap, v);
            s.avail_up_s[ap] = 0;
            s.avail_seen_s[ap] = 0;
        }
        s.avail_minute = minute;
        if (!s.avail_gap_pending) {
            if (app_grid_time() != 0) {
                k.av_newest_min = app_grid_time() / 60u - 1u;
            } else if (k.av_newest_min != 0) {
                k.av_newest_min++;
            }
        }
    }
    if (!s.avail_gap_pending && k.av_newest_min == 0 && app_grid_time() != 0) {
        k.av_newest_min = app_grid_time() / 60u - 1u;   /* anchored now, so neighbours' records line up */
        s.save_due = true;
    }

    if (s.save_due || now - s.last_save_ms >= STICKY_SAVE_MS) {
        if (k.av_newest_min != 0 || k.incident_count > 0 || k.time_gen != 0) {
            sticky_save();
        } else {
            s.save_due = false;
            s.last_save_ms = now;
        }
    }
    if ((s.last_avail_announce_ms == 0 || now - s.last_avail_announce_ms >= AV_ANNOUNCE_MS) &&
        k.av_newest_min != 0 && !s.avail_gap_pending) {
        s.last_avail_announce_ms = now;
        grid_state_avail_announce();
    }

    for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
        if (ap == s.self) {
            continue;
        }
        if (s.was_up[ap] && !up[ap]) {
            s.down_since_ms[ap] = now;
            s.back_ms[ap] = 0;
            incident_log(incident_open(ap));
        } else if (!s.was_up[ap] && up[ap] && s.open_incident[ap] >= 0) {
            k.incidents[s.open_incident[ap]].duration_s = sat16((now - s.down_since_ms[ap]) / 1000u);
            s.back_ms[ap] = now;
        }
        int16_t open = s.open_incident[ap];
        if (open >= 0 && s.back_ms[ap] != 0 && now - s.back_ms[ap] > CAUSE_WAIT_MS) {
            incident_set_kind(&k.incidents[open], GRID_INCIDENT_UNEXPLAINED, 0);
            s.open_incident[ap] = -1;
            incident_log(&k.incidents[open]);
            s.save_due = true;
        }
        s.was_up[ap] = up[ap];
    }
    for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
        if (ap != s.self && !s.aps[ap].valid && !up[ap]) {
            continue;   /* never heard: stays unknown */
        }
        s.avail_seen_s[ap]++;
        s.avail_up_s[ap] += up[ap] ? 1u : 0u;
    }
}

/* ---- console ---- */

static const char *quality_text(uint8_t q)
{
    return q == LG_TIME_AUTHORITATIVE ? "AUTHORITATIVE" : q == LG_TIME_CARRIED ? "CARRIED" : "UNSET";
}

void grid_state_print(void)
{
    node_settings_t c;
    grid_state_settings(&c);
    printf("Settings version %" PRIu32 " (made on AP %u), %s; time generation %" PRIu32 " (set on AP %u)\n", c.seq,
           c.author, c.configured ? "set up" : "not set up", k.time_gen, k.time_author);
    for (uint16_t ap = 0; ap < LG_MAX_NODES; ap++) {
        grid_ap_info_t a;
        if (!grid_state_ap(ap, &a)) {
            continue;
        }
        if (a.sync_age_s == GRID_NEVER) {
            printf("  AP %u %-8s up %" PRIu32 " s, time %s, never synced%s\n", a.ap, a.name, a.uptime_s,
                   quality_text(a.time_quality), a.self ? " (this AP)" : "");
        } else {
            printf("  AP %u %-8s up %" PRIu32 " s, time %s, synced %" PRIu32 " s ago from AP %u (%+d ms), stratum %u%s\n",
                   a.ap, a.name, a.uptime_s, quality_text(a.time_quality), a.sync_age_s, a.sync_from, a.sync_drift_ms,
                   a.stratum, a.self ? " (this AP)" : "");
        }
    }
    printf("  history: %u incident(s), availability anchored at grid minute %" PRIu32 "\n", k.incident_count,
           k.av_newest_min);
}
