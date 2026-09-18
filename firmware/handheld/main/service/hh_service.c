/*
 * Handheld network service (design review answers 9-11 and 19; decisions D6, D12, D27).
 *
 * One task owns everything here. Wi-Fi events and the beacon element callback only copy
 * into its queue.
 *   SEARCHING    single-channel active scan; nodes are recognised by the beacon element's
 *                magic, version, and grid discriminator
 *   CONNECTING   joins the chosen node's BSSID with a static address, then opens TCP to .1
 *   REGISTERING  lg_client sends REGISTER and waits for REGISTER_ACK
 *   ONLINE       PING every 10 s; no PONG for 25 s, a Wi-Fi drop, or a socket error starts over
 *
 * Not yet: BLE observer (D4), proactive roaming (answer 11), messaging screens (P6).
 * BSSIDs stay in memory and are never logged (D21).
 */
#include "hh_battery.h"
#include "hh_gps.h"
#include "hh_mem.h"
#include "hh_service.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_body.h"
#include "lg_client.h"
#include "lg_crypto.h"
#include "lg_proto_config.h"
#include "lg_timekeep.h"
#include "lg_roster.h"
#include "lg_secrets.h"
#include "lwip/sockets.h"
#include "nvs.h"

#ifndef LG_SECRET_DISCRIMINATOR
#error "lg_secrets.h has no LG_SECRET_DISCRIMINATOR. Run: python tools/gen_secrets.py --update"
#endif

static const char *TAG = "NET";

#define TASK_STACK            8192
#define TASK_PRIORITY         5
#define QUEUE_LEN             24
#define SEND_QUEUE_LEN        4
#define SCAN_DWELL_MIN_MS     40
#define SCAN_DWELL_MAX_MS     120   /* beacons come every 102 ms */
#define NODE_FRESH_MS         15000
#define NODE_EXPIRE_MS        150000
#define RSSI_FLOOR_DBM        (-85)
#define STICKY_BONUS_DB       3
#define JOIN_TIMEOUT_MS       8000
#define TCP_CONNECT_WAIT_MS   3000
#define TCP_RETRY_MS          300
#define TCP_TRIES             4
#define REGISTER_TIMEOUT_MS   5000
#define PING_INTERVAL_MS      10000
#define PONG_TIMEOUT_MS       25000
#define RESCAN_ONLINE_MS      60000
/*
 * Moving for grid time. APs flag in their beacon whether they hold grid time. A handheld on an
 * AP without it, while another usable AP in range has it, moves -- but only once that has been
 * true for TIME_MOVE_AFTER_MS, rescanning every TIME_RESCAN_MS meanwhile, and not again within
 * TIME_MOVE_HOLDOFF_MS. Linked APs share time within seconds (D45), so this matters only when
 * the grid is split; the delays keep a handheld from bouncing while an AP is still catching up.
 */
#define TIME_MOVE_AFTER_MS    30000
#define TIME_RESCAN_MS        10000
#define TIME_MOVE_HOLDOFF_MS  120000
#define TIME_PREFER_DB        30

/*
 * Load balancing (D54). Every AP heard at BALANCE_RSSI_DBM or better is good enough, and among
 * those a handheld joins the one with the fewest handhelds, signal breaking ties. While online it
 * moves to another good AP only when that AP has fewer handhelds than its own would have without
 * it, so a move always evens the load and two handhelds never swap back and forth. It waits until
 * it has been online BALANCE_MIN_ONLINE_MS plus BALANCE_STAGGER_MS per device number, rescans
 * first, and moves at most once per BALANCE_HOLDOFF_MS, so handhelds that all see the same crowded
 * AP leave it one at a time, each seeing the move of the one before.
 */
#define BALANCE_RSSI_DBM      (-70)
#define BALANCE_MIN_ONLINE_MS 60000
#define BALANCE_STAGGER_MS    20000
#define BALANCE_HOLDOFF_MS    180000
#define BALANCE_SCAN_AGE_MS   10000
#define RSSI_POLL_MS          2000
#define TICK_MS               1000
#define PUBLISH_MS            1000
#define BACKOFF_FIRST_MS      1000
#define BACKOFF_MAX_MS        10000
#define STA_BEACON_TIMEOUT_S  3
#define NVS_NAMESPACE         "lghh"

#define MIN_U32(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U32(a, b) ((a) > (b) ? (a) : (b))

static const uint8_t s_discriminator[4] = LG_SECRET_DISCRIMINATOR;

typedef enum {
    EV_SCAN_DONE,
    EV_STA_CONNECTED,
    EV_STA_DISCONNECTED,
    EV_BEACON,
    EV_PREFER,
    EV_SCAN_NOW,
    EV_RECONNECT,
    EV_MARK_READ,
    EV_GROUP_EDIT,
    EV_RENAME,
    EV_GPS,          /* D65: the GPS task has a new fix, or lost one; read hh_gps_state */
} ev_type_t;

typedef struct {
    uint8_t type;
    int8_t  rssi;
    uint8_t bssid[6];
    uint8_t payload[LG_DISC_LEN];
    char    name[LG_DISC_NAME_MAX + 1];   /* EV_BEACON: AP name from the vendor IE tail, or empty */
    int32_t value;                 /* disconnect reason, or preferred node */
    uint32_t id[3];                /* EV_MARK_READ: author, boot, seq of the message read */
    lg_group_edit_t group;         /* EV_GROUP_EDIT, members as roster user bits */
    char    new_name[LG_NAME_MAX];  /* EV_RENAME: NUL-terminated */
} ev_t;

typedef struct {
    bool     valid;
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  clients;
    uint8_t  free_slots;
    uint8_t  flags;
    char     ssid[HH_SSID_MAX];   /* the AP's name as shown on screens, from its vendor IE (D46) */
    uint32_t heard_ms;
} node_cand_t;

typedef struct {
    uint8_t  scope;
    bool     urgent;
    uint32_t target;
    uint16_t len;
    char     text[HH_TEXT_MAX + 1];
} send_req_t;

/* A push-to-talk frame on its way to the AP (D61). */
typedef struct {
    uint8_t  scope;
    uint32_t target;
    uint16_t len;
    uint8_t  frame[HH_VOICE_FRAME_MAX];
} voice_req_t;

#define VOICE_QUEUE_LEN 4   /* 400 ms of talk: more than that behind means the link is too slow anyway */
/*
 * Wi-Fi power save holds frames for a dozing handheld at the AP until the next beacon, so talk
 * arrived in bursts with gaps longer than the player waits and was cut short. While talk flows
 * either way, and VOICE_AWAKE_MS after, the radio stays awake.
 */
#define VOICE_AWAKE_MS  10000

static struct {
    uint32_t          device;
    const lg_user_t  *user;
    lg_roster_t       roster;       /* users fixed; groups replaced by newer tables and saved (D52) */
    char              group_problem[HH_PROBLEM_MAX];
    uint32_t          boot;
    lg_e2e_t          e2e;
    lg_client_t       client;
    esp_netif_t      *netif;
    QueueHandle_t     queue;
    SemaphoreHandle_t lock;
    hh_status_t       status;       /* guarded by lock */
    node_cand_t       cand[HH_MAX_NODES];

    hh_link_t link;
    char      problem[HH_PROBLEM_MAX];
    int       sock;
    int       joining;              /* node being joined or registered with, -1 none */
    int       last_node;
    int       preferred;
    bool      wifi_associating;     /* esp_wifi_connect called and not yet disconnected */
    bool      expect_disconnect;    /* we asked for the disconnect; its event is not a failure */
    bool      wifi_up;
    bool      send_failed;
    bool      dirty;
    int       tcp_tries;
    int8_t    rssi;
    uint32_t  joins;
    uint32_t  state_since_ms;
    uint32_t  online_since_ms;
    uint32_t  next_attempt_ms;
    uint32_t  next_tcp_ms;
    uint32_t  backoff_ms;
    uint32_t  last_scan_ms;
    uint32_t  last_ping_ms;
    uint32_t  last_rssi_ms;
    uint32_t  last_tick_ms;
    uint32_t  last_publish_ms;
    uint32_t  no_time_since_ms;     /* 0, or when this AP was first seen without time while another had it */
    uint32_t  last_time_move_ms;
    uint32_t  last_balance_ms;      /* when this handheld last moved to even the load, 0 never */
    int64_t   time_offset_s;
    bool      time_set;
    uint16_t  rx_fill;
    uint8_t   rx[2 + LG_FRAME_MAX];
    uint8_t   tx[2 + LG_FRAME_MAX];

    QueueHandle_t send_queue;
    QueueHandle_t voice_queue;
    const hh_voice_io_t *voice_io;
    int           voice_refused;       /* the last local refusal reported, so a talk reports it once */
    uint32_t      voice_ms;            /* last talk frame sent or received, 0 never */
    bool          voice_awake;         /* power save is off for talk */
    bool          sending_voice;       /* io_send: a talk frame, which may be dropped rather than wait */
    hh_message_t  ring[HH_MESSAGES];   /* oldest at ring_head */
    uint8_t       ring_head;
    uint8_t       ring_count;
    uint32_t      msg_version;
    uint32_t      msg_counter;
} s = { .sock = -1, .joining = -1, .last_node = -1, .preferred = -1 };

static volatile bool s_scanning;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static void set_problem(const char *text)
{
    snprintf(s.problem, sizeof(s.problem), "%s", text);
    s.dirty = true;
}

/* The name a handheld chose (D50), or its roster name until it chooses one. Service task only. */
static const char *roster_name(uint32_t device)
{
    const lg_name_t *chosen = lg_client_name(&s.client, device);
    if (chosen != NULL) {
        return chosen->text;
    }
    const lg_user_t *u = lg_roster_user(&s.roster, device);
    return u != NULL ? u->name : "Unknown handheld";
}

/* One buffer for saving and loading names: both run on the service task, and a buffer each
 * cost 1 KB. */
static uint8_t s_names_blob[LG_MAX_DEVICES * LG_NAME_LEN_MAX];

/*
 * Every name this handheld knows, kept in flash as one blob of packed NAME records (D48, D49), so
 * names survive a restart even before an AP is in range. Written only when a name changes.
 */
static void save_names(void)
{
    uint8_t *blob = s_names_blob;
    size_t used = 0;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (s.client.names[i].version != 0) {
            used += lg_name_enc(&s.client.names[i], blob + used);
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = used > 0 ? nvs_set_blob(h, "names", blob, used) : nvs_erase_key(h, "names");
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW("GRID", "[GRID] Names not saved: %s", esp_err_to_name(err));
    }
}

static void load_names(void)
{
    uint8_t *blob = s_names_blob;
    size_t len = sizeof(s_names_blob);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_blob(h, "names", blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return;
    }
    unsigned loaded = 0;
    for (size_t at = 0; at + LG_NAME_LEN_MIN <= len;) {
        size_t rec = 9u + blob[at + 8];
        lg_name_t name;
        if (at + rec > len || !lg_name_dec(blob + at, rec, &name)) {
            break;   /* a damaged tail loses only what follows it */
        }
        loaded += lg_client_restore_name(&s.client, &name) ? 1u : 0u;
        at += rec;
    }
    ESP_LOGI("GRID", "[GRID] Loaded %u chosen name(s) from flash", loaded);
}

/* ---- message list (service task only; readers take the lock) ---- */

static hh_message_t *ring_add(void)
{
    uint8_t pos;
    if (s.ring_count < HH_MESSAGES) {
        pos = (uint8_t)((s.ring_head + s.ring_count) % HH_MESSAGES);
        s.ring_count++;
    } else {
        pos = s.ring_head;
        s.ring_head = (uint8_t)((s.ring_head + 1u) % HH_MESSAGES);
    }
    hh_message_t *m = &s.ring[pos];
    memset(m, 0, sizeof(*m));
    m->id = ++s.msg_counter;
    s.msg_version++;
    s.dirty = true;
    return m;
}

static void ring_set_text(hh_message_t *m, const char *text, size_t len)
{
    if (len > HH_TEXT_MAX) {
        len = HH_TEXT_MAX;
    }
    memcpy(m->text, text, len);
    m->text[len] = '\0';
    m->len = (uint16_t)len;
}

/* Our own message: its state follows the outbox entry it was queued into. */
static uint8_t state_from_outbox(const lg_out_msg_t *o)
{
    switch (o->state) {
    case LG_OUT_ACCEPTED:  return HH_MSG_ACCEPTED;
    case LG_OUT_DELIVERED: return HH_MSG_DELIVERED;
    case LG_OUT_READ:      return HH_MSG_READ;
    case LG_OUT_REJECTED:  return HH_MSG_REJECTED;
    default:               return HH_MSG_PENDING;
    }
}

static void ring_update_from_outbox(uint32_t slot)
{
    if (slot >= LG_OUTBOX_SIZE) {
        return;
    }
    const lg_out_msg_t *o = &s.client.outbox[slot];
    for (uint8_t i = 0; i < s.ring_count; i++) {
        hh_message_t *m = &s.ring[(s.ring_head + i) % HH_MESSAGES];
        if (m->mine && m->seq == o->seq && o->seq != 0) {
            m->state = state_from_outbox(o);
            m->reject = o->reject_reason;
            /* Counts, for a marker that says how many rather than which state (D42). The
             * core keeps these per roster member for every scope; only 1:1 promotes state.
             * Both fit a byte: the masks they are counted from are one bit per roster user,
             * and LG_MAX_DEVICES is 32. */
            m->delivered_count = (uint8_t)o->delivered_count;
            m->read_count = (uint8_t)o->read_count;
            m->read_mask = o->read_mask;   /* who, not just how many (D58) */
            s.msg_version++;
            s.dirty = true;
            return;
        }
    }
}

/*
 * A removed group's messages go (D52). The ring is compacted in place under the lock, because
 * the screens read it from their own task.
 */
static void ring_forget_group(uint16_t id)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    /* In place, oldest first: the write position never passes the read position, so no scratch
     * copy of the ring is needed (it was 6.7 KB of static RAM). */
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.ring_count; i++) {
        const hh_message_t *m = &s.ring[(s.ring_head + i) % HH_MESSAGES];
        if (m->scope == LG_SCOPE_GROUP && m->target == id) {
            continue;
        }
        if (n != i) {
            s.ring[(s.ring_head + n) % HH_MESSAGES] = *m;
        }
        n++;
    }
    uint8_t dropped = (uint8_t)(s.ring_count - n);
    for (uint8_t i = n; i < s.ring_count; i++) {
        memset(&s.ring[(s.ring_head + i) % HH_MESSAGES], 0, sizeof(s.ring[0]));
    }
    s.ring_count = n;
    s.msg_version++;
    s.dirty = true;
    xSemaphoreGive(s.lock);
    ESP_LOGI("MSG", "[MSG] Group %u was removed; deleted its %u message(s)", id, dropped);
}

/* ---- groups in NVS (D48, D52): kept as their GROUPS body ---- */

static void groups_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t body[LG_GROUPS_MAX_LEN];
    size_t len = sizeof(body);
    lg_groups_t in;
    if (nvs_get_blob(h, "groups", body, &len) == ESP_OK && lg_groups_dec(body, len, &in)) {
        s.roster.groups = in;
    }
    nvs_close(h);
}

static void groups_save(void)
{
    uint8_t body[LG_GROUPS_MAX_LEN];
    size_t len = lg_groups_enc(&s.roster.groups, body);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "groups", body, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW("GRID", "[GRID] Groups not saved to flash: %s", esp_err_to_name(err));
    }
}

static void io_groups_removed(void *ctx, const uint16_t *removed, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        ring_forget_group(removed[i]);
    }
}

/* ---- GPS and positions (D65) ----
 *
 * The GPS task (hh_gps.c) only posts EV_GPS; everything below runs on the service task, which owns
 * lg_client and the clock. While the GPS has a fix it is this handheld's clock, the one REGISTER's
 * client_time carries back to an AP (D53) and the one LG_POS_LIVE positions carry. Grid time rules
 * (D6) are untouched: lg_client_time_restricted still asks whether the AP holds grid time.
 *
 * Positions are never logged here, this handheld's or anyone else's; `gps` prints our own on request.
 */
#define POS_PERIOD_MS     30000    /* resend while fixed, so a restarted AP or handheld soon has it again */
#define POS_MOVE_M        25.0f    /* ... or at once after moving this far */
#define POS_LIVE_MS       2000u    /* a fix younger than this is sent as LG_POS_LIVE */
#define GPS_CLOCK_STEP_S  2u       /* the GPS re-sets the clock only when they disagree by this much */
#define GRID_WARN_MS      60000u   /* at most one "the grid's time differs from the GPS" line a minute */
#define POS_SLOTS         (LG_MAX_DEVICES + 1)   /* handhelds 1..LG_MAX_DEVICES, then MAIN */

typedef enum { CLOCK_NONE = 0, CLOCK_KEPT, CLOCK_GRID, CLOCK_GPS } clock_source_t;

static struct {
    int           rx, tx;             /* the board's GPS pins, -1 none */
    bool          owns_clock;         /* a fresh fix set the clock; the grid may not move it meanwhile */
    volatile uint8_t source;          /* clock_source_t, read by the console */
    bool          fix;                /* the last state the service saw, to tell a change */
    bool          has_pos;
    uint8_t       sats;
    int32_t       lat_u, lon_u;
    bool          pos_due;            /* send at the next chance: set at registration */
    bool          sent_any;
    uint32_t      sent_ms;            /* last attempt, successful or not */
    int32_t       sent_lat, sent_lon;
    uint32_t      grid_warn_ms;
    uint32_t      version;            /* positions_version; guarded by s.lock */
    lg_position_t pos[POS_SLOTS];     /* guarded by s.lock: the service's copy for other tasks */
} gp = { .rx = -1, .tx = -1 };

static int pos_slot(uint32_t subject)
{
    if (subject >= 1u && subject <= LG_MAX_DEVICES) {
        return (int)subject - 1;
    }
    return subject == HH_SUBJECT_MAIN ? LG_MAX_DEVICES : -1;
}

/* GPS task: copy nothing, just wake the service task. A full queue drops one second; the next comes. */
static void gps_notify(void)
{
    ev_t ev = { .type = EV_GPS };
    (void)xQueueSend(s.queue, &ev, 0);
}

static float moved_m(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b)
{
    const float m_per_microdeg = 0.111195f;   /* one microdegree of latitude, in metres */
    float dy = (float)((int64_t)lat_a - lat_b) * m_per_microdeg;
    float dx = (float)((int64_t)lon_a - lon_b) * m_per_microdeg * cosf((float)lat_a * 1.7453293e-8f);
    return sqrtf(dx * dx + dy * dy);
}

/* Sends this handheld's position if online and fixed; true when lg_client took it. */
static bool send_own_position(uint32_t now)
{
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (s.link != HH_LINK_ONLINE || !g.fix || !g.has_pos) {
        return false;
    }
    uint8_t flags = g.fix_age_ms < POS_LIVE_MS ? LG_POS_LIVE : 0u;
    int rc = lg_client_send_position(&s.client, g.lat_u, g.lon_u, g.last_unix, g.sats, flags);
    gp.sent_any = true;
    gp.sent_ms = now;
    gp.sent_lat = g.lat_u;
    gp.sent_lon = g.lon_u;
    gp.pos_due = false;
    if (rc < 0) {
        ESP_LOGW("TIME", "[GPS] Position not sent: %s", lg_err_str(rc));
        return false;
    }
    return true;
}

/* Online and fixed: at registration, every POS_PERIOD_MS, or after moving POS_MOVE_M. */
static void position_step(uint32_t now)
{
    if (s.link != HH_LINK_ONLINE || gp.rx < 0) {
        return;
    }
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (!g.fix || !g.has_pos) {
        return;
    }
    if (gp.pos_due || !gp.sent_any || now - gp.sent_ms >= POS_PERIOD_MS ||
        moved_m(g.lat_u, g.lon_u, gp.sent_lat, gp.sent_lon) > POS_MOVE_M) {
        (void)send_own_position(now);
    }
}

/* EV_GPS: the clock follows a fresh fix, and the screens hear about a change. */
static void gps_update(void)
{
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (g.fix != gp.fix || g.sats != gp.sats || g.has_pos != gp.has_pos || g.lat_u != gp.lat_u ||
        g.lon_u != gp.lon_u) {
        gp.fix = g.fix;
        gp.sats = g.sats;
        gp.has_pos = g.has_pos;
        gp.lat_u = g.lat_u;
        gp.lon_u = g.lon_u;
        xSemaphoreTake(s.lock, portMAX_DELAY);
        gp.version++;
        xSemaphoreGive(s.lock);
        s.dirty = true;
    }
    if (!g.fix) {
        gp.owns_clock = false;   /* the clock carries on from the last fix; the grid may correct it again */
        return;
    }
    /* The time the fix stood for, plus how long ago its sentence began to arrive. */
    uint64_t gps_ms = (uint64_t)g.last_unix * 1000u + g.last_millis + g.fix_age_ms;
    uint32_t gps_s = (uint32_t)(gps_ms / 1000u);
    uint32_t mine = s.time_set ? (uint32_t)(mono_s() + s.time_offset_s) : 0;
    uint32_t diff = mine > gps_s ? mine - gps_s : gps_s - mine;
    if (gp.owns_clock && mine != 0 && diff < GPS_CLOCK_STEP_S) {
        return;
    }
    s.time_offset_s = (int64_t)gps_s - mono_s();
    s.time_set = true;
    lg_timekeep_save(gps_s);   /* kept across a restart (D60), as a time from the grid is */
    if (!gp.owns_clock) {
        ESP_LOGI("TIME", "[TIME] Clock set from the GPS: %" PRIu32 " (%u satellites)%s", gps_s, g.sats,
                 mine != 0 && diff >= GPS_CLOCK_STEP_S ? "; it was off" : "");
    } else {
        ESP_LOGW("TIME", "[TIME] Clock stepped to the GPS: %" PRIu32 ", was %" PRIu32, gps_s, mine);
    }
    gp.owns_clock = true;
    gp.source = CLOCK_GPS;
}

/* LG_CEV_POSITION: copy lg_client's record where other tasks can read it. Never logged. */
static void position_mirror(uint32_t subject)
{
    int slot = pos_slot(subject);
    if (slot < 0) {
        return;
    }
    const lg_position_t *p = lg_client_position(&s.client, subject);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (p != NULL) {
        gp.pos[slot] = *p;
        gp.pos[slot].subject = subject;
    } else {
        memset(&gp.pos[slot], 0, sizeof(gp.pos[slot]));
    }
    gp.version++;
    xSemaphoreGive(s.lock);
}

/* ---- lg_client io (service task only) ---- */

static bool io_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (s.sock < 0 || len > LG_FRAME_MAX) {
        return false;
    }
    lg_wr16(s.tx, (uint16_t)len);
    memcpy(s.tx + 2, frame, len);
    size_t total = 2 + len;
    size_t sent = 0;
    uint32_t deadline = now_ms() + 500;
    while (sent < total) {
        int n = send(s.sock, s.tx + sent, total - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && sent == 0 && s.sending_voice) {
            return false;   /* a talk frame with nowhere to go is dropped; the session is fine */
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && now_ms() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            s.send_failed = true;   /* handled by the task loop, never inside a callback */
            return false;
        }
    }
    return true;
}

static uint32_t io_now_ms(void *ctx)
{
    (void)ctx;
    return now_ms();
}

static uint32_t io_local_time(void *ctx)
{
    (void)ctx;
    return s.time_set ? (uint32_t)(mono_s() + s.time_offset_s) : 0;
}

static void io_set_time(void *ctx, uint32_t unix_s)
{
    (void)ctx;
    if (gp.owns_clock) {
        /* D65: while the GPS has a fix it is the clock. The grid's rules (D6) are unchanged; only
         * this handheld's own clock stays on the GPS. Said at most once a minute. */
        uint32_t now = now_ms();
        if (gp.grid_warn_ms == 0 || now - gp.grid_warn_ms >= GRID_WARN_MS) {
            gp.grid_warn_ms = now == 0 ? 1 : now;
            uint32_t mine = (uint32_t)(mono_s() + s.time_offset_s);
            ESP_LOGW("TIME", "[TIME] Grid time %" PRIu32 " differs from this handheld's GPS clock %" PRIu32
                     "; keeping the GPS", unix_s, mine);
        }
        return;
    }
    gp.source = CLOCK_GRID;
    s.time_offset_s = (int64_t)unix_s - mono_s();
    s.time_set = true;
    lg_timekeep_save(unix_s);   /* so this handheld can carry it back after its own restart (D60) */
    ESP_LOGI("TIME", "[TIME] Clock set from the grid: %" PRIu32, unix_s);
}

static void io_event(void *ctx, const lg_client_event_t *ev)
{
    (void)ctx;
    switch (ev->type) {
    case LG_CEV_REGISTERED:
        if (s.joins == 0) {
            hh_mem_mark("first registration (Wi-Fi joined, TCP session up)");
        }
        s.link = HH_LINK_ONLINE;
        s.joins++;
        s.online_since_ms = now_ms();
        s.last_ping_ms = s.online_since_ms;
        s.last_node = (int)ev->value;
        s.backoff_ms = 0;
        gp.pos_due = true;   /* D65: our position at registration, from the service loop */
        set_problem("");
        ESP_LOGI("GRID", "[GRID] Registered with node %" PRIu32 " as device %" PRIu32 " (%s)", ev->value, s.device,
                 roster_name(s.device));
        ESP_LOGI(TAG, "[NET] Online; free heap %" PRIu32 " KB, lowest %" PRIu32 " KB",
                 esp_get_free_heap_size() / 1024, esp_get_minimum_free_heap_size() / 1024);
        break;
    case LG_CEV_PRESENCE: {
        const lg_peer_t *p = lg_client_peer(&s.client, ev->value);
        if (p != NULL && ev->value != s.device) {
            ESP_LOGI("GRID", "[GRID] Device %" PRIu32 " (%s) %s on node %u", ev->value, roster_name(ev->value),
                     p->state == LG_PRES_ONLINE ? "online" : "offline", p->node);
        }
        break;
    }
    case LG_CEV_NAME:
        ESP_LOGI("GRID", "[GRID] Device %" PRIu32 " is now called \"%s\"%s", ev->value, roster_name(ev->value),
                 ev->value == s.device ? " (this handheld)" : "");
        save_names();
        s.dirty = true;
        break;
    case LG_CEV_TIME:
        if (ev->value == 0) {
            ESP_LOGW("TIME", "[TIME] Grid time is not set: only receiving and urgent broadcasts (D6)");
        } else {
            ESP_LOGI("TIME", "[TIME] Grid time %" PRIu32, ev->value);
        }
        break;
    case LG_CEV_MESSAGE: {
        const lg_in_msg_t *in = lg_client_inbox(&s.client, 0);
        if (in != NULL) {
            hh_message_t *m = ring_add();
            m->author = in->author;
            m->target = in->target;
            m->scope = in->scope;
            m->state = HH_MSG_IN;
            m->seq = in->seq;
            m->origin_boot = in->boot;
            m->read_sent = false;
            m->urgent = (in->flags & LG_FLAG_URGENT) != 0;
            m->grid_time = in->grid_time;
            ring_set_text(m, (const char *)in->text, in->len);
            if (in->scope == LG_SCOPE_DIRECT) {
                /* Never the text of a 1:1 message (AGENTS.md): author, boot, and seq identify it. */
                ESP_LOGI("MSG", "[MSG] From %" PRIu32 " (%s)%s: 1:1 boot %" PRIu32 " seq %" PRIu32 ", %u bytes",
                         in->author, roster_name(in->author), m->urgent ? " URGENT" : "", in->boot, in->seq,
                         (unsigned)in->len);
            } else {
                ESP_LOGI("MSG", "[MSG] From %" PRIu32 " (%s)%s: %.*s", in->author, roster_name(in->author),
                         m->urgent ? " URGENT" : "", (int)in->len, (const char *)in->text);
            }
        }
        break;
    }
    case LG_CEV_OUTBOX: {
        /* Logged for the same reason as the read report: the only way to tell delivered from
         * read on the bench was to ask the console, and by then the reason was gone. */
        uint8_t was = ev->value < LG_OUTBOX_SIZE ? state_from_outbox(&s.client.outbox[ev->value]) : 0;
        ring_update_from_outbox(ev->value);
        if (was == HH_MSG_READ) {
            ESP_LOGI("MSG", "[MSG] Read by the other handheld: outbox slot %" PRIu32, ev->value);
        } else if (was == HH_MSG_DELIVERED) {
            ESP_LOGI("MSG", "[MSG] Delivered to the other handheld: outbox slot %" PRIu32, ev->value);
        }
        break;
    }
    case LG_CEV_DECRYPT_FAILED:
        ESP_LOGW("MSG", "[MSG] Could not decrypt a 1:1 message from device %" PRIu32, ev->value);
        break;
    case LG_CEV_KEY_CHANGED:
        ESP_LOGW("GRID", "[GRID] Device %" PRIu32 " advertised a different key; the pinned key is kept", ev->value);
        break;
    case LG_CEV_GROUPS:
        groups_save();
        s.group_problem[0] = '\0';
        ESP_LOGI("GRID", "[GRID] Groups version %" PRIu32 " (made on AP %u): %u group(s), %" PRIu32 " removed",
                 s.roster.groups.seq, s.roster.groups.author, s.roster.groups.count, ev->value);
        break;
    case LG_CEV_VOICE_REFUSED:
        ESP_LOGW("MSG", "[MSG] The AP refused talk: status %" PRIu32, ev->value);
        if (s.voice_io != NULL && s.voice_io->on_refused != NULL) {
            s.voice_io->on_refused((int)ev->value);
        }
        break;
    case LG_CEV_POSITION:
        position_mirror(ev->value);   /* D65; never logged */
        break;
    case LG_CEV_GROUP_REFUSED:
        snprintf(s.group_problem, sizeof(s.group_problem), "%s",
                 ev->value == LG_ACK_REJ_NOT_MEMBER       ? "Only members can change this group" :
                 ev->value == LG_ACK_REJ_UNKNOWN_TARGET   ? "That group was already removed" :
                 s.roster.groups.count >= LG_MAX_GROUPS   ? "There are already 8 groups" :
                                                            "The AP refused that group");
        ESP_LOGW("GRID", "[GRID] Group edit refused: status %" PRIu32, ev->value);
        break;
    default:
        break;
    }
    s.dirty = true;
}

/* A talk frame for this handheld (D61), already opened if it was 1:1. Straight to the voice
 * module's queue: nothing here is kept, logged, or retried. */
static void io_voice(void *ctx, const lg_env_t *env, const uint8_t *payload, size_t len)
{
    (void)ctx;
    if (s.voice_io == NULL || s.voice_io->on_frame == NULL) {
        return;
    }
    s.voice_ms = now_ms();
    uint32_t conversation = env->scope == LG_SCOPE_DIRECT ? env->origin_id : env->target;
    s.voice_io->on_frame(env->origin_id, env->origin_boot, env->scope, conversation, payload, len);
}

static int io_seal(void *ctx, uint32_t peer, const uint8_t *pub, const uint8_t *nonce, const uint8_t *aad,
                   size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    (void)ctx;
    return lg_e2e_seal(&s.e2e, peer, pub, nonce, aad, aad_len, pt, pt_len, out);
}

static int io_open(void *ctx, uint32_t peer, const uint8_t *pub, const uint8_t *nonce, const uint8_t *aad,
                   size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)
{
    (void)ctx;
    return lg_e2e_open(&s.e2e, peer, pub, nonce, aad, aad_len, ct, ct_len, out);
}

/* ---- callbacks from the Wi-Fi driver: copy and return ---- */

static void on_vendor_ie(void *ctx, wifi_vendor_ie_type_t type, const uint8_t sa[6], const vendor_ie_data_t *ie,
                         int rssi)
{
    (void)ctx;
    static const uint8_t oui[3] = LG_VENDOR_OUI;
    if (!s_scanning || (type != WIFI_VND_IE_TYPE_BEACON && type != WIFI_VND_IE_TYPE_PROBE_RESP) || ie == NULL ||
        ie->element_id != WIFI_VENDOR_IE_ELEMENT_ID || ie->length < 4 + LG_DISC_LEN ||
        memcmp(ie->vendor_oui, oui, 3) != 0 || ie->vendor_oui_type != LG_VENDOR_OUI_TYPE) {
        return;
    }
    ev_t ev = { .type = EV_BEACON, .rssi = (int8_t)rssi };
    memcpy(ev.bssid, sa, 6);
    memcpy(ev.payload, ie->payload, LG_DISC_LEN);
    /* Optional tail (D46): u8 length and the AP name. Every AP shares one SSID, so this is the label. */
    size_t tail = (size_t)ie->length - 4u - LG_DISC_LEN;
    if (tail >= 1u) {
        size_t n = ie->payload[LG_DISC_LEN];
        if (n <= LG_DISC_NAME_MAX && n <= tail - 1u) {
            memcpy(ev.name, ie->payload + LG_DISC_LEN + 1u, n);
        }
    }
    xQueueSend(s.queue, &ev, 0);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    ev_t ev = { 0 };
    if (id == WIFI_EVENT_SCAN_DONE) {
        ev.type = EV_SCAN_DONE;
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ev.type = EV_STA_CONNECTED;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ev.type = EV_STA_DISCONNECTED;
        ev.value = data != NULL ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
    } else {
        return;
    }
    xQueueSend(s.queue, &ev, pdMS_TO_TICKS(20));
}

/* ---- link management ---- */

static void start_scan(uint32_t now)
{
    if (s_scanning) {
        return;
    }
    wifi_scan_config_t cfg = {
        .channel = LG_PROTO_CHANNEL,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = SCAN_DWELL_MIN_MS, .max = SCAN_DWELL_MAX_MS },
    };
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "[NET] Scan did not start: %s", esp_err_to_name(err));
        return;
    }
    s.last_scan_ms = now;
}

static bool node_usable(const node_cand_t *c, uint32_t now)
{
    return c->valid && c->ssid[0] != '\0' && c->free_slots > 0 && c->rssi >= RSSI_FLOOR_DBM &&
           now - c->heard_ms <= NODE_FRESH_MS;
}

/* Handhelds registered with AP i other than this one. The beacon counts this handheld on the AP it
 * is registered with, which must not count against staying there. */
static int others_on(int i)
{
    int clients = s.cand[i].clients;
    if (s.link == HH_LINK_ONLINE && i == (int)s.client.node && clients > 0) {
        clients--;
    }
    return clients;
}

/*
 * Selection score (answer 10, D54). In order of weight: an AP with a working backbone, then one
 * with grid time, then any AP with good signal ahead of a weak one, then the fewest other
 * handhelds, then signal (with a small bonus for the AP last used).
 */
static int pick_node(uint32_t now)
{
    bool any_backbone = false;
    bool any_time = false;
    for (int i = 0; i < HH_MAX_NODES; i++) {
        if (node_usable(&s.cand[i], now) && (s.cand[i].flags & LG_DISC_FLAG_BACKBONE)) {
            any_backbone = true;
        }
        if (node_usable(&s.cand[i], now) && (s.cand[i].flags & LG_DISC_FLAG_TIME)) {
            any_time = true;
        }
    }
    int best = -1;
    int best_score = -1000;
    for (int i = 0; i < HH_MAX_NODES; i++) {
        const node_cand_t *c = &s.cand[i];
        if (!node_usable(c, now) || (s.preferred >= 0 && i != s.preferred)) {
            continue;
        }
        int score = c->rssi;
        if (i == s.last_node) {
            score += STICKY_BONUS_DB;
        }
        if (c->rssi >= BALANCE_RSSI_DBM) {
            score += 1000 - 100 * others_on(i);   /* good signal: the fewest handhelds wins (D54) */
        }
        if (any_backbone && !(c->flags & LG_DISC_FLAG_BACKBONE)) {
            score -= 100000;
        }
        if (any_time && !(c->flags & LG_DISC_FLAG_TIME)) {
            score -= 10000 + TIME_PREFER_DB;   /* without grid time only urgent broadcasts go out (D6) */
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static void drop_link(const char *why, bool retry_now);
static void leave_gracefully(void);

/* Online on an AP with no grid time while another usable AP has it: move, once it has lasted. */
static bool time_move_due(uint32_t now)
{
    int current = (int)s.client.node;
    const node_cand_t *here = current >= 0 && current < HH_MAX_NODES ? &s.cand[current] : NULL;
    bool here_lacks = here != NULL && here->valid && !(here->flags & LG_DISC_FLAG_TIME);
    int better = -1;
    for (int i = 0; here_lacks && i < HH_MAX_NODES; i++) {
        if (i != current && node_usable(&s.cand[i], now) && (s.cand[i].flags & LG_DISC_FLAG_TIME) &&
            (s.preferred < 0 || s.preferred == i)) {
            better = i;
            break;
        }
    }
    if (better < 0) {
        if (s.no_time_since_ms != 0) {
            ESP_LOGI(TAG, "[TIME] No longer waiting to move for grid time");
        }
        s.no_time_since_ms = 0;
        return false;
    }
    if (s.no_time_since_ms == 0) {
        s.no_time_since_ms = now;
        ESP_LOGI(TAG, "[TIME] %s has no grid time and %s does; moving in %d s unless it catches up",
                 here->ssid, s.cand[better].ssid, TIME_MOVE_AFTER_MS / 1000);
        return false;
    }
    if (now - s.no_time_since_ms < TIME_MOVE_AFTER_MS ||
        (s.last_time_move_ms != 0 && now - s.last_time_move_ms < TIME_MOVE_HOLDOFF_MS)) {
        return false;
    }
    s.last_time_move_ms = now;
    s.no_time_since_ms = 0;
    ESP_LOGW(TAG, "[TIME] Moving from %s to %s, which has grid time", here->ssid, s.cand[better].ssid);
    leave_gracefully();
    drop_link("Moving to an AP that has grid time", true);
    return true;
}

/*
 * A planned move: close the session while still associated, so the AP sees the FIN and stops
 * counting this handheld at once. Dropping Wi-Fi first left the AP counting it until its 30 s idle
 * timeout, and a second handheld balancing in that window moved away from an AP that was no
 * longer crowded.
 */
static void leave_gracefully(void)
{
    if (s.sock >= 0) {
        shutdown(s.sock, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* Online, and another good AP carries fewer handhelds than this one would without us: move (D54). */
static bool balance_move_due(uint32_t now)
{
    int current = (int)s.client.node;
    if (s.preferred >= 0 || current < 0 || current >= HH_MAX_NODES || s.no_time_since_ms != 0) {
        return false;   /* a chosen AP stays chosen, and a move for grid time comes first */
    }
    uint32_t wait = BALANCE_MIN_ONLINE_MS + BALANCE_STAGGER_MS * (s.device % 8u);
    if (now - s.online_since_ms < wait ||
        (s.last_balance_ms != 0 && now - s.last_balance_ms < BALANCE_HOLDOFF_MS)) {
        return false;
    }
    const node_cand_t *here = &s.cand[current];
    int best = pick_node(now);
    if (best < 0 || best == current || !here->valid || s.cand[best].rssi < BALANCE_RSSI_DBM ||
        others_on(best) >= others_on(current)) {
        return false;
    }
    if (now - s.last_scan_ms >= BALANCE_SCAN_AGE_MS) {
        start_scan(now);   /* decide on fresh counts: another handheld may just have moved */
        return false;
    }
    if (s_scanning) {
        return false;
    }
    s.last_balance_ms = now;
    ESP_LOGW(TAG, "[ROAM] Balancing: %s has %d other handheld(s), %s has %d; moving", here->ssid, others_on(current),
             s.cand[best].ssid, others_on(best));
    leave_gracefully();
    drop_link("Moving to an AP with fewer handhelds", true);
    return true;
}

static void drop_link(const char *why, bool retry_now)
{
    uint32_t now = now_ms();
    bool was_online = s.link == HH_LINK_ONLINE;
    if (s.sock >= 0) {
        close(s.sock);
        s.sock = -1;
    }
    s.rx_fill = 0;
    s.send_failed = false;
    lg_client_disconnected(&s.client);
    if (s.wifi_associating) {
        s.expect_disconnect = true;
        esp_wifi_disconnect();
    }
    s.wifi_up = false;
    s.joining = -1;
    s.link = HH_LINK_SEARCHING;
    s.state_since_ms = now;
    if (retry_now) {
        s.backoff_ms = 0;
    } else if (was_online || s.backoff_ms == 0) {
        s.backoff_ms = BACKOFF_FIRST_MS;
    } else {
        s.backoff_ms = MIN_U32(s.backoff_ms * 2, BACKOFF_MAX_MS);
    }
    s.next_attempt_ms = now + s.backoff_ms;
    set_problem(why);
    ESP_LOGW(TAG, "[NET] %s; searching again in %" PRIu32 " s", why, s.backoff_ms / 1000);
}

static void begin_join(int node, uint32_t now)
{
    const node_cand_t *c = &s.cand[node];
    uint8_t ip[4];
    lg_proto_handheld_ip((uint16_t)node, s.device, ip);
    esp_netif_ip_info_t info = { 0 };
    esp_netif_set_ip4_addr(&info.ip, ip[0], ip[1], ip[2], ip[3]);
    esp_netif_set_ip4_addr(&info.gw, ip[0], ip[1], ip[2], 1);
    esp_netif_set_ip4_addr(&info.netmask, 255, 255, 255, 0);
    if (esp_netif_set_ip_info(s.netif, &info) != ESP_OK) {
        drop_link("Could not set this handheld's address", false);
        return;
    }

    /* One network (D46): every AP has the same SSID, and the BSSID picks this one. */
    wifi_config_t wc = { 0 };
    _Static_assert(sizeof(LG_PROTO_SSID) <= sizeof(wc.sta.ssid), "SSID too long");
    memcpy(wc.sta.ssid, LG_PROTO_SSID, sizeof(LG_PROTO_SSID) - 1);
    _Static_assert(sizeof(LG_SECRET_WIFI_PASSPHRASE) <= sizeof(wc.sta.password), "Wi-Fi passphrase too long");
    memcpy(wc.sta.password, LG_SECRET_WIFI_PASSPHRASE, sizeof(LG_SECRET_WIFI_PASSPHRASE) - 1);
    wc.sta.scan_method = WIFI_FAST_SCAN;
    wc.sta.bssid_set = true;
    memcpy(wc.sta.bssid, c->bssid, 6);
    wc.sta.channel = LG_PROTO_CHANNEL;
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    /* 802.11k/v/r stay off: BSS transition would clear the fixed BSSID (answer 10). */
    wc.sta.rm_enabled = 0;
    wc.sta.btm_enabled = 0;
    wc.sta.mbo_enabled = 0;
    wc.sta.ft_enabled = 0;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        drop_link("Could not start joining an AP", false);
        return;
    }
    s.wifi_associating = true;
    s.expect_disconnect = false;
    s.wifi_up = false;
    s.tcp_tries = 0;
    s.joining = node;
    s.rssi = c->rssi;
    s.link = HH_LINK_CONNECTING;
    s.state_since_ms = now;
    set_problem("");
    ESP_LOGI(TAG, "[NET] Joining %s (node %d, %d dBm) as %u.%u.%u.%u", c->ssid, node, c->rssi, ip[0], ip[1], ip[2],
             ip[3]);
}

static int tcp_open(int node)
{
    uint8_t ip[4];
    lg_proto_node_ip((uint16_t)node, ip);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LG_PROTO_TCP_PORT),
        .sin_addr.s_addr = htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3]),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = TCP_CONNECT_WAIT_MS / 1000, .tv_usec = 0 };
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void on_beacon(const ev_t *ev, uint32_t now)
{
    const uint8_t *p = ev->payload;
    if (p[0] != LG_DISC_MAGIC0 || p[1] != LG_DISC_MAGIC1 || p[2] != LG_DISC_VERSION ||
        memcmp(p + 3, s_discriminator, sizeof(s_discriminator)) != 0 || p[7] >= HH_MAX_NODES) {
        return;   /* another grid, another version, or not LocalGrid */
    }
    node_cand_t *c = &s.cand[p[7]];
    if (!c->valid || memcmp(c->bssid, ev->bssid, 6) != 0) {
        memset(c, 0, sizeof(*c));
        memcpy(c->bssid, ev->bssid, 6);
        c->valid = true;
    }
    if (ev->name[0] != '\0') {
        snprintf(c->ssid, sizeof(c->ssid), "%s", ev->name);
    } else if (c->ssid[0] == '\0') {
        snprintf(c->ssid, sizeof(c->ssid), "AP %u", (unsigned)p[7]);   /* AP firmware older than the name tail */
    }
    c->rssi = ev->rssi;
    c->free_slots = p[9];
    c->flags = p[10];
    c->clients = p[11];
    c->heard_ms = now;
}

static void on_scan_done(uint32_t now)
{
    s_scanning = false;
    /* Candidates come from the vendor IE alone (name included, D46), so the driver's scan
     * records are only counted and freed. */
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK) {
        n = 0;
    }
    esp_wifi_clear_ap_list();
    int found = 0;
    for (int i = 0; i < HH_MAX_NODES; i++) {
        node_cand_t *c = &s.cand[i];
        if (!c->valid) {
            continue;
        }
        if (now - c->heard_ms > NODE_EXPIRE_MS) {
            memset(c, 0, sizeof(*c));
            continue;
        }
        found += node_usable(c, now) ? 1 : 0;
        ESP_LOGI(TAG, "[NET] Heard AP %d \"%s\" %d dBm, %u attached, %u free, backbone %s, grid time %s, %" PRIu32
                 " ms ago", i, c->ssid[0] != '\0' ? c->ssid : "(name unknown)", c->rssi, c->clients, c->free_slots,
                 (c->flags & LG_DISC_FLAG_BACKBONE) ? "yes" : "no", (c->flags & LG_DISC_FLAG_TIME) ? "yes" : "no",
                 now - c->heard_ms);
    }
    ESP_LOGI(TAG, "[NET] Scan done: %u records, %d usable node(s)", (unsigned)n, found);
    s.dirty = true;
    if (s.link != HH_LINK_SEARCHING) {
        return;
    }
    int node = pick_node(now);
    if (node >= 0) {
        begin_join(node, now);
        return;
    }
    s.backoff_ms = s.backoff_ms == 0 ? BACKOFF_FIRST_MS : MIN_U32(s.backoff_ms * 2, BACKOFF_MAX_MS);
    s.next_attempt_ms = now + s.backoff_ms;
    set_problem(found > 0 && s.preferred >= 0 ? "The chosen AP is not in range" : "No LocalGrid AP in range");
    ESP_LOGI(TAG, "[NET] Scan found %d usable node(s); scanning again in %" PRIu32 " s", found, s.backoff_ms / 1000);
}

static void handle_event(const ev_t *ev, uint32_t now)
{
    switch (ev->type) {
    case EV_BEACON:
        on_beacon(ev, now);
        break;
    case EV_SCAN_DONE:
        on_scan_done(now);
        break;
    case EV_STA_CONNECTED:
        if (s.link == HH_LINK_CONNECTING) {
            s.wifi_up = true;
            s.next_tcp_ms = now + TCP_RETRY_MS;   /* let the interface come up first */
        }
        break;
    case EV_STA_DISCONNECTED:
        s.wifi_associating = false;
        if (s.expect_disconnect) {
            s.expect_disconnect = false;
        } else if (s.link != HH_LINK_SEARCHING && s.link != HH_LINK_STOPPED) {
            char why[HH_PROBLEM_MAX];
            snprintf(why, sizeof(why), "Wi-Fi link to the node lost (reason %" PRId32 ")", ev->value);
            drop_link(why, false);
        }
        break;
    case EV_PREFER:
        s.preferred = ev->value;
        s.dirty = true;
        ESP_LOGI(TAG, "[NET] Node choice: %s", ev->value < 0 ? "automatic" : "fixed");
        if (ev->value >= 0 && s.link != HH_LINK_SEARCHING && s.joining != ev->value &&
            !(s.link == HH_LINK_ONLINE && s.client.node == (uint16_t)ev->value)) {
            drop_link("Switching to the chosen node", true);
        } else if (s.link == HH_LINK_SEARCHING) {
            s.backoff_ms = 0;
            s.next_attempt_ms = now;
        }
        break;
    case EV_SCAN_NOW:
        start_scan(now);
        break;
    case EV_RECONNECT:
        drop_link("Reconnecting on request", true);
        break;
    case EV_GROUP_EDIT: {
        int rc = lg_client_edit_group(&s.client, &ev->group);
        if (rc == LG_OK) {
            s.group_problem[0] = '\0';   /* a refusal of an earlier edit is not this edit's answer */
        } else {
            snprintf(s.group_problem, sizeof(s.group_problem), "Not connected to an AP; try again");
            s.dirty = true;
        }
        ESP_LOGI("GRID", "[GRID] Group edit (op %u, group %u) %s", ev->group.op, ev->group.id,
                 rc == LG_OK ? "sent" : "not sent: offline");
        break;
    }
    case EV_MARK_READ: {
        /* The reader has seen it, so tell the author. Logged because this path had no
         * evidence at all: on the bench the receiver opened the conversation and the sender
         * still showed delivered, with nothing to say whether the report was ever sent. */
        bool told = lg_client_mark_read(&s.client, ev->id[0], ev->id[1], ev->id[2]);
        ESP_LOGI("MSG", "[MSG] Read report to device %" PRIu32 " (boot %" PRIu32 " seq %" PRIu32 "): %s",
                 ev->id[0], ev->id[1], ev->id[2], told ? "sent" : "refused");
        break;
    }
    case EV_GPS:
        gps_update();
        break;
    case EV_RENAME: {
        size_t len = strlen(ev->new_name);   /* hh_service_set_name copied at most LG_NAME_MAX - 1 bytes */
        if (lg_client_set_name(&s.client, (const uint8_t *)ev->new_name, len) != LG_OK) {
            ESP_LOGW("GRID", "[GRID] Rename refused: the name is empty, too long, or not UTF-8");
        }
        break;
    }
    default:
        break;
    }
}

static void poll_socket(uint32_t wait_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s.sock, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)wait_ms * 1000 };
    if (select(s.sock + 1, &rfds, NULL, NULL, &tv) <= 0) {
        return;
    }
    int n = recv(s.sock, s.rx + s.rx_fill, sizeof(s.rx) - s.rx_fill, 0);
    if (n == 0) {
        drop_link("The node closed the session", false);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            drop_link("Session receive error", false);
        }
        return;
    }
    s.rx_fill = (uint16_t)(s.rx_fill + n);
    while (s.sock >= 0 && s.rx_fill >= 2) {
        uint16_t flen = lg_rd16(s.rx);
        if (flen < LG_ENV_SIZE || flen > LG_FRAME_MAX) {
            drop_link("Bad frame from the node", false);
            return;
        }
        if (s.rx_fill < 2u + flen) {
            break;
        }
        lg_client_on_frame(&s.client, s.rx + 2, flen);
        uint16_t used = (uint16_t)(2u + flen);
        memmove(s.rx, s.rx + used, s.rx_fill - used);
        s.rx_fill = (uint16_t)(s.rx_fill - used);
    }
}

/* Sending happens here: lg_client belongs to this task alone. */
static void drain_send_queue(void)
{
    static send_req_t req;
    while (s.send_queue != NULL && xQueueReceive(s.send_queue, &req, 0) == pdTRUE) {
        if (req.urgent && req.scope == LG_SCOPE_BROADCAST) {
            (void)send_own_position(now_ms());   /* D65: where the emergency is, just ahead of it */
        }
        uint16_t flags = LG_FLAG_ACK_REQUESTED | (req.urgent ? LG_FLAG_URGENT : 0);
        int rc = lg_client_send_text(&s.client, req.scope, req.target, flags, (const uint8_t *)req.text, req.len);
        hh_message_t *m = ring_add();
        m->author = s.device;
        m->target = req.scope == LG_SCOPE_BROADCAST ? LG_TARGET_ALL : req.target;
        m->scope = req.scope;
        m->mine = true;
        m->urgent = req.urgent;
        ring_set_text(m, req.text, req.len);
        if (rc >= 0) {
            const lg_out_msg_t *o = &s.client.outbox[rc];
            m->seq = o->seq;
            m->grid_time = o->grid_time;
            m->state = state_from_outbox(o);
            if (req.scope == LG_SCOPE_DIRECT) {
                ESP_LOGI("MSG", "[MSG] Sent%s to device %" PRIu32 ": 1:1 boot %" PRIu32 " seq %" PRIu32 ", %u bytes",
                         req.urgent ? " URGENT" : "", m->target, o->boot, o->seq, (unsigned)req.len);
            } else {
                ESP_LOGI("MSG", "[MSG] Sent%s to %s %" PRIu32 ": %s", req.urgent ? " URGENT" : "",
                         req.scope == LG_SCOPE_GROUP ? "group" : "everyone", m->target, m->text);
            }
        } else {
            m->state = HH_MSG_REFUSED;
            m->reject = rc == LG_ERR_TIME     ? HH_REFUSE_TIME
                        : rc == LG_ERR_FULL   ? HH_REFUSE_FULL
                        : rc == LG_ERR_DENIED ? HH_REFUSE_ANNOUNCE
                                              : HH_REFUSE_INVALID;
            const char *why = m->reject == HH_REFUSE_TIME       ? "grid time is not set, so only urgent broadcasts go out"
                              : m->reject == HH_REFUSE_FULL     ? "outbox full"
                              : m->reject == HH_REFUSE_ANNOUNCE ? "the admin page does not let this handheld announce"
                                                                : "not allowed: unknown target, bad text, or no key yet";
            if (req.scope == LG_SCOPE_DIRECT) {
                ESP_LOGW("MSG", "[MSG] Not sent (%s): 1:1 to device %" PRIu32 ", %u bytes", why, m->target,
                         (unsigned)req.len);
            } else {
                ESP_LOGW("MSG", "[MSG] Not sent (%s): %s", why, m->text);
            }
        }
    }
}

/* Talk frames (D61): each goes once, as soon as it is here. A refusal this handheld makes itself
 * (no grid time, offline) is reported once per talk, not once per tenth of a second. */
static void drain_voice_queue(void)
{
    static voice_req_t req;
    while (s.voice_queue != NULL && xQueueReceive(s.voice_queue, &req, 0) == pdTRUE) {
        lg_voice_hdr_t h;
        if (lg_voice_hdr_dec(req.frame, req.len, &h) && h.frame == 0) {
            s.voice_refused = 0;   /* a new talk */
        }
        s.voice_ms = now_ms();
        s.sending_voice = true;
        int rc = s.link == HH_LINK_ONLINE ? lg_client_send_voice(&s.client, req.scope, req.target, req.frame, req.len)
                                          : LG_ERR_SHORT;
        s.sending_voice = false;
        if (rc == LG_ERR_FULL) {
            continue;   /* this frame was dropped at a full socket; the next may get through */
        }
        if (rc < 0 && rc != s.voice_refused) {
            s.voice_refused = rc;
            ESP_LOGW("MSG", "[MSG] Talk not sent: %s", lg_err_str(rc));
            if (s.voice_io != NULL && s.voice_io->on_refused != NULL) {
                s.voice_io->on_refused(rc);
            }
        }
    }
}

/* Power save off while talk flows, back on once it has been quiet a while. */
static void voice_power(void)
{
    /* Its own clock: voice_ms is stamped after the loop read `now`, and now - voice_ms would wrap. */
    uint32_t now = now_ms();
    bool want = s.voice_ms != 0 && now - s.voice_ms < VOICE_AWAKE_MS;
    if (want != s.voice_awake) {
        s.voice_awake = want;
        esp_wifi_set_ps(want ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
        ESP_LOGI(TAG, "[NET] Radio %s", want ? "kept awake for talk" : "back to power save");
    }
}

static void step(uint32_t now)
{
    voice_power();
    if (s.send_failed) {
        drop_link("Session send failed", false);
        return;
    }
    switch (s.link) {
    case HH_LINK_SEARCHING:
        if (!s_scanning && now >= s.next_attempt_ms) {
            int node = pick_node(now);
            if (node >= 0) {
                begin_join(node, now);
            } else {
                start_scan(now);
            }
        }
        break;
    case HH_LINK_CONNECTING:
        if (s.wifi_up && now >= s.next_tcp_ms) {
            s.sock = tcp_open(s.joining);
            if (s.sock >= 0) {
                s.link = HH_LINK_REGISTERING;
                s.state_since_ms = now_ms();
                s.dirty = true;
                ESP_LOGI(TAG, "[NET] Session open to node %d; registering", s.joining);
                lg_client_connected(&s.client);
            } else if (++s.tcp_tries >= TCP_TRIES) {
                drop_link("Could not open a session with the node", false);
            } else {
                s.next_tcp_ms = now + TCP_RETRY_MS;
            }
        } else if (now - s.state_since_ms > JOIN_TIMEOUT_MS) {
            drop_link("Joining the node's Wi-Fi timed out", false);
        }
        break;
    case HH_LINK_REGISTERING:
        if (now - s.state_since_ms > REGISTER_TIMEOUT_MS) {
            drop_link("The node did not accept this handheld", false);
        }
        break;
    case HH_LINK_ONLINE: {
        if (now - s.last_ping_ms >= PING_INTERVAL_MS) {
            s.last_ping_ms = now;
            lg_client_ping(&s.client);
        }
        uint32_t heard = MAX_U32(s.client.last_pong_ms, s.online_since_ms);
        if (now - heard > PONG_TIMEOUT_MS) {
            drop_link("The node stopped answering", false);
            break;
        }
        if (s.voice_awake) {
            break;   /* talk is flowing: no move to another AP and no scan until it has been quiet a while */
        }
        if (time_move_due(now)) {
            break;
        }
        if (balance_move_due(now)) {
            break;
        }
        if (now - s.last_scan_ms >= (s.no_time_since_ms != 0 ? TIME_RESCAN_MS : RESCAN_ONLINE_MS)) {
            start_scan(now);   /* keeps the nodes-in-range list fresh */
        }
        break;
    }
    default:
        break;
    }
    if (s.sock >= 0 && now - s.last_rssi_ms >= RSSI_POLL_MS) {
        s.last_rssi_ms = now;
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK && rssi != s.rssi) {
            s.rssi = (int8_t)rssi;
            s.dirty = true;
        }
    }
    if (now - s.last_tick_ms >= TICK_MS) {
        s.last_tick_ms = now;
        lg_client_tick(&s.client);
    }
    position_step(now);
}

static void publish(void)
{
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    hh_status_t *st = &s.status;
    st->version++;
    st->link = s.link;
    snprintf(st->problem, sizeof(st->problem), "%s", s.problem);
    int node = s.link == HH_LINK_ONLINE ? (int)s.client.node : s.joining;
    st->node = node;
    st->node_ssid[0] = '\0';
    memset(st->ip, 0, sizeof(st->ip));
    if (node >= 0 && node < HH_MAX_NODES) {
        snprintf(st->node_ssid, sizeof(st->node_ssid), "%s", s.cand[node].ssid);
        lg_proto_handheld_ip((uint16_t)node, s.device, st->ip);
    }
    st->rssi = s.rssi;
    st->joins = s.joins;
    st->time_restricted = lg_client_time_restricted(&s.client);
    st->grid_time = st->time_restricted ? 0 : io_local_time(NULL);
    st->preferred_node = s.preferred;
    snprintf(st->name, sizeof(st->name), "%s", roster_name(s.device));

    st->n_nodes = 0;
    for (int i = 0; i < HH_MAX_NODES; i++) {
        const node_cand_t *c = &s.cand[i];
        if (c->valid && c->ssid[0] != '\0' && now - c->heard_ms <= NODE_EXPIRE_MS) {
            hh_node_seen_t *o = &st->nodes[st->n_nodes++];
            o->node = (uint16_t)i;
            snprintf(o->ssid, sizeof(o->ssid), "%s", c->ssid);
            o->rssi = c->rssi;
            o->clients = c->clients;
            o->backbone = (c->flags & LG_DISC_FLAG_BACKBONE) != 0;
            o->has_time = (c->flags & LG_DISC_FLAG_TIME) != 0;
        }
    }

    /* Only handhelds this one has actually heard about; no placeholder people. */
    st->n_people = 0;
    const lg_roster_t *roster = &s.roster;
    for (size_t i = 0; i < roster->n_users && st->n_people < LG_MAX_DEVICES; i++) {
        uint32_t dev = roster->users[i].device;
        const lg_peer_t *p = lg_client_peer(&s.client, dev);
        if (dev == s.device || p == NULL || !p->in_use) {
            continue;
        }
        hh_person_t *o = &st->people[st->n_people++];
        o->device = dev;
        snprintf(o->name, sizeof(o->name), "%s", roster_name(dev));
        o->node = p->node;
        o->online = s.link == HH_LINK_ONLINE && p->state == LG_PRES_ONLINE;
    }
    /* Who a group can include: this handheld and those the grid has told it about. Roster
     * places no handheld has taken are never offered: they would be people who do not exist. */
    st->n_users = 0;
    for (size_t i = 0; i < roster->n_users && st->n_users < LG_MAX_DEVICES; i++) {
        uint32_t dev = roster->users[i].device;
        const lg_peer_t *p = lg_client_peer(&s.client, dev);
        if (dev != s.device && (p == NULL || !p->in_use)) {
            continue;
        }
        hh_user_t *o = &st->users[st->n_users++];
        o->device = dev;
        snprintf(o->name, sizeof(o->name), "%s", roster_name(dev));
    }
    st->n_groups = 0;
    for (size_t i = 0; i < roster->groups.count && st->n_groups < LG_MAX_GROUPS; i++) {
        const lg_group_t *rg = &roster->groups.groups[i];
        hh_group_t *g = &st->groups[st->n_groups++];
        g->id = rg->id;
        snprintf(g->name, sizeof(g->name), "%s", rg->name);
        g->member = lg_roster_is_member(roster, s.device, g->id);
        /* The denominator for a group delivery count (D42), counted here because the screens
         * cannot see the roster. Every handheld in the group, this one included. */
        g->members = 0;
        g->member_devices = 0;
        for (size_t u = 0; u < roster->n_users; u++) {
            uint32_t dev = roster->users[u].device;
            if (lg_roster_is_member(roster, dev, g->id)) {
                g->members++;
                if (dev >= 1u && dev <= 32u) {
                    g->member_devices |= 1u << (dev - 1u);
                }
            }
        }
    }
    st->groups_version = roster->groups.seq;
    st->may_announce = lg_roster_may_announce(roster, s.device);
    snprintf(st->group_problem, sizeof(st->group_problem), "%s", s.group_problem);
    st->messages_version = s.msg_version;
    st->free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    st->min_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    st->psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    st->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st->psram_min_free = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    st->positions_version = gp.version;
    xSemaphoreGive(s.lock);
    s.dirty = false;
    s.last_publish_ms = now;
}

static void service_task(void *arg)
{
    (void)arg;
    s.link = HH_LINK_SEARCHING;
    s.next_attempt_ms = now_ms();
    publish();
    for (;;) {
        uint32_t now = now_ms();
        ev_t ev;
        TickType_t wait = s.sock >= 0 ? 0 : pdMS_TO_TICKS(50);
        while (xQueueReceive(s.queue, &ev, wait) == pdTRUE) {
            handle_event(&ev, now_ms());
            wait = 0;
        }
        if (s.sock >= 0) {
            poll_socket(50);
        }
        now = now_ms();
        drain_voice_queue();
        drain_send_queue();
        step(now);
        if (s.dirty || now - s.last_publish_ms >= PUBLISH_MS) {
            publish();
        }
    }
}

/* ---- start-up ---- */

static esp_err_t commit_boot_counter(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t boot = 0;
    (void)nvs_get_u32(h, "boot", &boot);
    boot++;
    err = nvs_set_u32(h, "boot", boot);
    if (err == ESP_OK) {
        err = nvs_commit(h);   /* before any frame is sent: message ids and nonces must never repeat */
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s.boot = boot;
    }
    return err;
}

/* Needs the radio running first: randomness is only strong after Wi-Fi starts. */
static esp_err_t load_or_create_key(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t priv[LG_X25519_LEN];
    uint8_t pub[LG_X25519_LEN];
    size_t len = sizeof(priv);
    err = nvs_get_blob(h, "x25519", priv, &len);
    if (err != ESP_OK || len != sizeof(priv)) {
        if (lg_x25519_keypair(priv, pub) != 0) {
            nvs_close(h);
            return ESP_FAIL;
        }
        err = nvs_set_blob(h, "x25519", priv, sizeof(priv));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err == ESP_OK) {
            ESP_LOGI("GRID", "[GRID] Created this handheld's encryption key");
        }
    }
    nvs_close(h);
    int rc = err == ESP_OK ? lg_e2e_init(&s.e2e, s.device, priv) : -1;
    lg_secure_zero(priv, sizeof(priv));
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t wifi_start(void)
{
    /* The Wi-Fi driver prints MAC and BSSID at INFO; decision D21 keeps them out of output. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK) {
        return err;
    }
    s.netif = esp_netif_create_default_wifi_sta();
    if (s.netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_dhcpc_stop(s.netif);   /* static addressing, no DHCP delay on every join */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL)) != ESP_OK ||
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_vendor_ie_cb(on_vendor_ie, NULL)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    if (esp_wifi_set_inactive_time(WIFI_IF_STA, STA_BEACON_TIMEOUT_S) != ESP_OK) {
        ESP_LOGW(TAG, "[NET] Beacon timeout not shortened");
    }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

static esp_err_t fail_start(esp_err_t err, const char *problem)
{
    set_problem(problem);
    s.link = HH_LINK_STOPPED;
    publish();
    ESP_LOGE(TAG, "[NET] %s", problem);
    return err;
}

esp_err_t hh_service_start(const lg_identity_t *identity)
{
    hh_battery_start();   /* D62: first, so the badge works even when the network cannot start */
    s.lock = xSemaphoreCreateMutex();
    s.queue = xQueueCreate(QUEUE_LEN, sizeof(ev_t));
    s.send_queue = xQueueCreate(SEND_QUEUE_LEN, sizeof(send_req_t));
    s.voice_queue = xQueueCreate(VOICE_QUEUE_LEN, sizeof(voice_req_t));
    if (s.lock == NULL || s.queue == NULL || s.send_queue == NULL || s.voice_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s.status.node = -1;
    s.status.preferred_node = -1;
    if (!identity->present || !identity->has_device) {
        return fail_start(ESP_ERR_INVALID_STATE, "No device index. Provision with tools/flash.py --update-identity");
    }
    s.device = identity->device_index;
    lg_roster_init_prototype(&s.roster);
    s.user = lg_roster_user(&s.roster, s.device);
    if (s.user == NULL) {
        return fail_start(ESP_ERR_NOT_FOUND, "This handheld's device index is not in the grid roster");
    }
    /* The clock kept across this handheld's own restart (D60), so it has something to carry back to
     * an AP that came back without one (D53). The RTC counted the time we were away. */
    uint32_t kept = lg_timekeep_restore();
    if (kept != 0) {
        s.time_offset_s = (int64_t)kept - mono_s();
        s.time_set = true;
        gp.source = CLOCK_KEPT;
        ESP_LOGI("TIME", "[TIME] Clock %" PRIu32 " kept across the restart", kept);
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.device = s.device;
    snprintf(s.status.name, sizeof(s.status.name), "%s", s.user->name);
    xSemaphoreGive(s.lock);

    esp_err_t err;
    if (lg_crypto_init() != 0) {
        return fail_start(ESP_FAIL, "Crypto start failed");
    }
    if ((err = commit_boot_counter()) != ESP_OK) {
        return fail_start(err, "Could not save the boot counter");
    }
    if ((err = wifi_start()) != ESP_OK) {
        return fail_start(err, "Wi-Fi did not start");
    }
    if ((err = load_or_create_key()) != ESP_OK) {
        return fail_start(err, "Could not load this handheld's encryption key");
    }
    groups_load();   /* the last table this handheld heard, until an AP sends a newer one */
    ESP_LOGI("GRID", "[GRID] Groups version %" PRIu32 " from flash: %u group(s)", s.roster.groups.seq,
             s.roster.groups.count);
    const lg_client_io_t io = {
        .ctx = NULL,
        .send = io_send,
        .on_event = io_event,
        .now_ms = io_now_ms,
        .local_time = io_local_time,
        .set_time = io_set_time,
        .seal = io_seal,
        .open = io_open,
        .on_groups_removed = io_groups_removed,
        .on_voice = io_voice,
    };
    lg_client_init(&s.client, s.device, s.boot, s.e2e.pub, &s.roster, &io);
    load_names();
    snprintf(s.status.name, sizeof(s.status.name), "%s", roster_name(s.device));
    if (gp.rx >= 0 && hh_gps_start(gp.rx, gp.tx, gps_notify) != ESP_OK) {
        ESP_LOGW("TIME", "[GPS] No GPS reader; this handheld runs as one without a GPS");
    }
    if (xTaskCreate(service_task, "hh_net", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return fail_start(ESP_ERR_NO_MEM, "Network task did not start");
    }
    ESP_LOGI(TAG, "[NET] Handheld service started: device %" PRIu32 " (%s), boot %" PRIu32, s.device, roster_name(s.device),
             s.boot);
    return ESP_OK;
}

void hh_service_status(hh_status_t *out)
{
    if (s.lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->node = -1;
        out->preferred_node = -1;
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.status;
    xSemaphoreGive(s.lock);
}

/*
 * The screen says a received 1:1 message has been shown; the service task owns the client, so
 * the identity of that message goes to it through the queue (D27). Reported once per message.
 */
esp_err_t hh_service_edit_group(uint16_t id, const char *name, uint32_t members, bool remove)
{
    if (s.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ev_t ev = { .type = EV_GROUP_EDIT };
    ev.group.op = remove ? LG_GROUP_DELETE : id == 0 ? LG_GROUP_CREATE : LG_GROUP_UPDATE;
    ev.group.id = id;
    if (!remove) {
        size_t len = name != NULL ? strlen(name) : 0;
        if (len == 0 || len > HH_GROUP_NAME_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(ev.group.name, name, len);
        /* Devices to roster user bits. The roster's users are const, so reading them here,
         * outside the service task, is safe. */
        for (size_t u = 0; u < s.roster.n_users && u < 32u; u++) {
            uint32_t dev = s.roster.users[u].device;
            if (dev >= 1u && dev <= 32u && (members & (1u << (dev - 1u))) != 0) {
                ev.group.members |= 1u << u;
            }
        }
    }
    return xQueueSend(s.queue, &ev, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

uint8_t hh_service_reader_names(uint32_t mask, char *out, size_t cap)
{
    out[0] = '\0';
    size_t used = 0;
    uint8_t named = 0;
    for (size_t i = 0; i < s.roster.n_users && i < 32u; i++) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }
        const char *name = roster_name(s.roster.users[i].device);
        int n = snprintf(out + used, cap - used, "%s%s", named ? ", " : "", name);
        if (n < 0 || (size_t)n >= cap - used) {
            out[used] = '\0';   /* leave what fits whole: a half-written name names nobody */
            break;
        }
        used += (size_t)n;
        named++;
    }
    return named;
}

void hh_service_mark_read(uint32_t message_id)
{
    if (s.queue == NULL || s.lock == NULL) {
        return;
    }
    ev_t ev = { .type = EV_MARK_READ };
    bool found = false;
    if (xSemaphoreTake(s.lock, portMAX_DELAY) == pdTRUE) {
        for (uint8_t i = 0; i < s.ring_count; i++) {
            /* Through the ring head, as every other reader does: indexing the array directly
             * scans the wrong slots once the ring has wrapped, which would lose a read report
             * rather than fail loudly. */
            hh_message_t *m = &s.ring[(s.ring_head + i) % HH_MESSAGES];
            /* Every scope reports reads (D42, D58). Broadcasts were left out to save one ack
             * per handheld per announcement; the owner wants to see who has read an
             * announcement or an emergency, which is worth 13 bytes a handheld. */
            if (m->id == message_id && !m->mine && !m->read_sent) {
                m->read_sent = true;   /* one report per message, however often it is on screen */
                ev.id[0] = m->author;
                ev.id[1] = m->origin_boot;
                ev.id[2] = m->seq;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s.lock);
    }
    if (found) {
        xQueueSend(s.queue, &ev, 0);
    }
}

esp_err_t hh_service_set_name(const char *name)
{
    if (s.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = name != NULL ? strlen(name) : 0;
    if (!lg_name_valid((const uint8_t *)name, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    ev_t ev = { .type = EV_RENAME };
    memcpy(ev.new_name, name, len);   /* the rest is already zero */
    return xQueueSend(s.queue, &ev, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void hh_service_prefer_node(int node)
{
    if (s.queue == NULL) {
        return;
    }
    ev_t ev = { .type = EV_PREFER, .value = node };
    xQueueSend(s.queue, &ev, 0);
}

void hh_service_scan_now(void)
{
    if (s.queue != NULL) {
        ev_t ev = { .type = EV_SCAN_NOW };
        xQueueSend(s.queue, &ev, 0);
    }
}

void hh_service_reconnect(void)
{
    if (s.queue != NULL) {
        ev_t ev = { .type = EV_RECONNECT };
        xQueueSend(s.queue, &ev, 0);
    }
}

const char *hh_announce_problem(const hh_status_t *st)
{
    return st->may_announce ? "" : "Only urgent messages: the admin page has not let you announce";
}

const char *hh_message_state_text(const hh_message_t *m)
{
    switch (m->state) {
    case HH_MSG_IN:        return "received";
    case HH_MSG_PENDING:   return "sending";
    case HH_MSG_ACCEPTED:  return m->scope == LG_SCOPE_DIRECT ? "sent, waiting for the other handheld" : "sent";
    case HH_MSG_DELIVERED: return "delivered";
    case HH_MSG_READ:      return "read";
    case HH_MSG_REJECTED:
        switch (m->reject) {
        case LG_ACK_REJ_OFFLINE:        return "not delivered: that handheld is offline";
        case LG_ACK_REJ_NOT_MEMBER:     return "not sent: you are not in that group";
        case LG_ACK_REJ_RATE:           return "not sent: too many messages, wait a moment";
        case LG_ACK_REJ_TIME:           return "not sent: the clocks disagree by more than two minutes";
        case LG_ACK_REJ_UNKNOWN_TARGET: return "not sent: the grid does not know that handheld";
        case LG_ACK_REJ_INVALID:        return "not sent: the node refused the message";
        case LG_ACK_REJ_NOT_ALLOWED:    return "not sent: the admin page does not let you announce";
        default:                        return "not sent: the grid rejected it";
        }
    default:
        switch (m->reject) {
        case HH_REFUSE_TIME: return "not sent: grid time is not set, so only urgent broadcasts go out";
        case HH_REFUSE_FULL: return "not sent: too many messages waiting";
        case HH_REFUSE_ANNOUNCE: return "not sent: the admin page does not let you announce";
        default:             return "not sent: unknown handheld, empty text, or no encryption key yet";
        }
    }
}

esp_err_t hh_service_send(uint8_t scope, uint32_t target, bool urgent, const char *text)
{
    if (s.send_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = text != NULL ? strlen(text) : 0;
    if (len == 0 || len > HH_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    send_req_t req = { .scope = scope, .urgent = urgent, .target = target, .len = (uint16_t)len };
    memcpy(req.text, text, len);
    return xQueueSend(s.send_queue, &req, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

bool hh_service_message(size_t newest_index, hh_message_t *out)
{
    if (s.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool found = newest_index < s.ring_count;
    if (found) {
        *out = s.ring[(s.ring_head + s.ring_count - 1u - newest_index) % HH_MESSAGES];
    }
    xSemaphoreGive(s.lock);
    return found;
}

size_t hh_service_messages(hh_message_t *out, size_t max)
{
    if (s.lock == NULL || max == 0) {
        return 0;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    size_t n = s.ring_count < max ? s.ring_count : max;
    for (size_t i = 0; i < n; i++) {
        /* newest first */
        out[i] = s.ring[(s.ring_head + s.ring_count - 1u - i) % HH_MESSAGES];
    }
    xSemaphoreGive(s.lock);
    return n;
}

void hh_service_set_voice_io(const hh_voice_io_t *io)
{
    s.voice_io = io;
}

esp_err_t hh_service_voice_send(uint8_t scope, uint32_t target, const uint8_t *frame, size_t len)
{
    if ((scope != LG_SCOPE_DIRECT && scope != LG_SCOPE_GROUP) || frame == NULL || len == 0 ||
        len > HH_VOICE_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.voice_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    static voice_req_t req;   /* only the capture task sends, so one staging copy is enough */
    req.scope = scope;
    req.target = target;
    req.len = (uint16_t)len;
    memcpy(req.frame, frame, len);
    return xQueueSend(s.voice_queue, &req, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ---- GPS and positions (D65): readers on any task ---- */

void hh_service_set_gps_pins(int rx_gpio, int tx_gpio)
{
    gp.rx = rx_gpio;
    gp.tx = tx_gpio;
}

const char *hh_service_clock_source(void)
{
    switch (gp.source) {
    case CLOCK_GPS:  return "the GPS";
    case CLOCK_GRID: return "the grid";
    case CLOCK_KEPT: return "kept across a restart";
    default:         return "not set";
    }
}

/* Seconds since fix_time by this handheld's clock, 0 when either is unknown. The system clock is
 * read, not the service's offset: lg_timekeep_save keeps it equal, and it is safe from any task. */
static uint32_t age_of(uint32_t fix_time)
{
    time_t now = time(NULL);
    if (fix_time == 0 || now < (time_t)LG_POS_TIME_MIN || (uint64_t)now < fix_time) {
        return 0;
    }
    return (uint32_t)((uint64_t)now - fix_time);
}

bool hh_service_own_position(hh_position_t *out)
{
    memset(out, 0, sizeof(*out));
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (!g.started || !g.fix || !g.has_pos) {
        return false;
    }
    out->valid = true;
    out->lat_u = g.lat_u;
    out->lon_u = g.lon_u;
    out->fix_time = g.last_unix;
    out->sats = g.sats;
    out->age_s = g.fix_age_ms / 1000u;
    return true;
}

bool hh_service_position(uint32_t subject, hh_position_t *out)
{
    memset(out, 0, sizeof(*out));
    int slot = pos_slot(subject);
    if (slot < 0 || s.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    lg_position_t p = gp.pos[slot];
    xSemaphoreGive(s.lock);
    if (p.fix_time == 0) {
        return false;
    }
    out->valid = true;
    out->lat_u = p.lat_u;
    out->lon_u = p.lon_u;
    out->fix_time = p.fix_time;
    out->sats = p.sats;
    out->age_s = age_of(p.fix_time);
    return true;
}

bool hh_service_gps(uint8_t *sats, bool *fix)
{
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (sats != NULL) {
        *sats = g.sats;
    }
    if (fix != NULL) {
        *fix = g.fix;
    }
    return g.started && g.talking;
}

bool hh_service_gps_info(hh_gps_info_t *out)
{
    memset(out, 0, sizeof(*out));
    hh_gps_state_t g;
    hh_gps_state(&g);
    if (!g.started || !g.heard) {
        return false;
    }
    out->fitted = true;
    out->talking = g.talking;
    out->fix = g.fix;
    out->fix_type = g.fix_type >= 2u && g.fix_type <= 3u ? g.fix_type : 0u;
    out->corrected = g.quality == 2u;
    out->used = g.sats;
    out->in_view = g.in_view;
    out->tracked = g.tracked;
    out->best_snr = g.best_snr;
    out->hdop_c = g.hdop_c == 0u ? HH_GPS_UNKNOWN : g.hdop_c;
    out->pdop_c = g.pdop_c == 0u ? HH_GPS_UNKNOWN : g.pdop_c;
    out->has_pos = g.has_pos && g.fix;
    out->lat_u = g.lat_u;
    out->lon_u = g.lon_u;
    out->has_alt = g.has_alt && g.fix;
    out->alt_dm = g.alt_dm;
    out->speed_cms = g.fix ? g.speed_cms : HH_GPS_UNKNOWN;
    out->course_cd = g.fix ? g.course_cd : HH_GPS_UNKNOWN;
    out->utc = g.last_unix;
    out->fix_age_ms = g.fix_age_ms;
    out->sentences = g.sentences;
    out->bad = g.bad;
    return true;
}
