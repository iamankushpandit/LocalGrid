/*
 * LocalGrid infrastructure node (prototype).
 *
 * SoftAP on a fixed channel for handhelds, TCP control sessions, ESP-NOW
 * backbone to other nodes, BLE discovery adverts, serial console.
 * Node index and name come from the identity partition (decisions D20, D21).
 */
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "backbone.h"
#include "ble_adv.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_attr.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "grid_state.h"
#include "lg_crypto.h"
#include "lg_proto_config.h"
#include "lg_timekeep.h"
#include "gps.h"
#include "lg_secrets.h"
#include "node_app.h"
#include "nvs_flash.h"
#include "power_trace.h"
#include "sessions.h"
#include "settings.h"
#include "web_admin.h"

static const char *TAG = "GRID";

node_app_t g_app;

static const uint8_t s_backbone_key[32] = LG_SECRET_BACKBONE_KEY;
static uint8_t s_discriminator[4];
static uint8_t s_last_disc_clients = 0xFF;
static uint8_t s_last_disc_flags = 0xFF;

_Static_assert(LG_PROTO_DHCP_INDICES == LG_MAX_NODES, "one phone DHCP block per possible AP index (D46)");

#define STATUS_LOG_MS        30000u
#define TIME_ANNOUNCE_MS     60000u
#define NAMES_ANNOUNCE_MS    300000u   /* handheld names again, so a missed flood heals (D48, D50) */

/* ---- time ---- */

uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * Grid time is kept in milliseconds (monotonic time plus an offset) and handed between APs with
 * its milliseconds and a stratum, the distance from the AP where an admin set it.
 *
 * Whole seconds used to travel alone, so every hand-off could be up to a second out and two
 * hand-offs two; and any AP carrying time accepted a correction from any other that differed by
 * more than a second, so carrying APs pushed each other back and forth. Now:
 *   - an AP takes time only from a lower stratum than its own (or when it has none), so time
 *     flows outward from where it was set and never loops between siblings;
 *   - differences under TIME_DEADBAND_MS are left alone (radio and queue delay, not error);
 *   - a correction under TIME_STEP_MS is slewed in at TIME_SLEW_MS_PER_S, so the clock never
 *     jumps; anything larger, or an AP with no time at all, steps at once.
 */
#define TIME_DEADBAND_MS       100
#define TIME_STEP_MS           1000
#define TIME_SLEW_MS_PER_S     100

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

uint64_t app_grid_time_ms(void)
{
    if (g_app.time_quality == LG_TIME_UNSET) {
        return 0;
    }
    return (uint64_t)(mono_ms() + g_app.time_offset_ms);
}

uint32_t app_grid_time(void)
{
    return (uint32_t)(app_grid_time_ms() / 1000u);
}

static void set_grid_time_ms(uint64_t unix_ms, uint8_t quality, uint8_t stratum)
{
    g_app.time_offset_ms = (int64_t)unix_ms - mono_ms();
    g_app.time_slew_ms = 0;
    g_app.time_quality = quality;
    g_app.time_stratum = stratum;
    lg_timekeep_save((uint32_t)(unix_ms / 1000u));   /* so a restart keeps it (D60) */
    ptrace_event(PTRACE_TIME);
}

void app_time_follow_new_generation(void)
{
    g_app.time_stratum = LG_STRATUM_UNKNOWN;
}

void app_time_slew(uint32_t elapsed_ms)
{
    if (g_app.time_slew_ms == 0) {
        return;
    }
    int32_t step = (int32_t)((int64_t)TIME_SLEW_MS_PER_S * elapsed_ms / 1000);
    step = step < 1 ? 1 : step;
    if (g_app.time_slew_ms > 0) {
        step = step > g_app.time_slew_ms ? g_app.time_slew_ms : step;
    } else {
        step = -step < g_app.time_slew_ms ? g_app.time_slew_ms : -step;
    }
    g_app.time_offset_ms += step;
    g_app.time_slew_ms -= step;
}

static void io_time_now(void *ctx, lg_time_sync_t *out)
{
    (void)ctx;
    uint64_t ms = app_grid_time_ms();
    out->grid_time = (uint32_t)(ms / 1000u);
    out->millis = (uint16_t)(ms % 1000u);
    out->stratum = g_app.time_quality == LG_TIME_UNSET ? LG_STRATUM_UNKNOWN : g_app.time_stratum;
}

/* ---- lg_core io ---- */

static void io_to_client(void *ctx, uint32_t device, const uint8_t *frame, size_t len)
{
    (void)ctx;
    sess_send(device, frame, len);
}

static void io_unicast(void *ctx, uint16_t node, const uint8_t *frame, size_t len)
{
    (void)ctx;
    lgbb_send_unicast(node, frame, len);
}

static void io_flood(void *ctx, uint16_t except, const uint8_t *frame, size_t len)
{
    (void)ctx;
    lgbb_flood(except, frame, len);
}

static bool io_is_neighbor(void *ctx, uint16_t node)
{
    (void)ctx;
    return lgbb_is_neighbor(node);
}

static uint32_t io_now_ms(void *ctx)
{
    (void)ctx;
    return app_now_ms();
}

static uint32_t io_grid_time(void *ctx)
{
    (void)ctx;
    return app_grid_time();
}

static void io_on_diag(void *ctx, uint16_t origin_node, uint8_t hops, const uint8_t *text, size_t len)
{
    (void)ctx;
    ESP_LOGI("BB", "[BB] Echo from node %u after %u hop(s): %.*s", origin_node, hops, (int)len, (const char *)text);
}

static void io_on_grid_state(void *ctx, uint16_t origin_node, const uint8_t *body, size_t len)
{
    (void)ctx;
    grid_state_on_frame(origin_node, body, len);
}

static void io_on_groups_changed(void *ctx)
{
    (void)ctx;
    const lg_groups_t *g = &g_app.roster.groups;
    esp_err_t err = settings_groups_save(g);
    ESP_LOGI(TAG, "[GRID] Groups version %" PRIu32 " (made on AP %u): %u group(s)%s", g->seq, g->author,
             g->count, err == ESP_OK ? "" : "; not saved to flash");
}

/*
 * Handheld names (D50) live in flash as one blob of packed NAME records (D49), rewritten only
 * when a newer name arrives, so an AP that restarts still knows every name and hands them to
 * handhelds and APs that missed them.
 */
static void io_on_name(void *ctx, const lg_name_t *name)
{
    (void)ctx;
    static uint8_t blob[LG_MAX_DEVICES * LG_NAME_LEN_MAX];   /* about 1 KB: kept off the stack */
    size_t used = 0;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (g_app.core.names[i].version != 0) {
            used += lg_name_enc(&g_app.core.names[i], blob + used);
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("lg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "names", blob, used);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "[GRID] Device %" PRIu32 " is now called \"%s\" (version %" PRIu32 ")%s", name->device, name->text,
             name->version, err == ESP_OK ? "" : ", not saved");
}

static void load_names(void)
{
    static uint8_t blob[LG_MAX_DEVICES * LG_NAME_LEN_MAX];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    if (nvs_open("lg", NVS_READONLY, &h) != ESP_OK) {
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
        loaded += lg_node_restore_name(&g_app.core, &name) ? 1u : 0u;
        at += rec;
    }
    ESP_LOGI(TAG, "[GRID] Loaded %u handheld name(s) from flash", loaded);
}

static void io_on_time(void *ctx, uint16_t origin_node, const lg_time_sync_t *t)
{
    (void)ctx;
    if (t->grid_time == 0 || t->quality == LG_TIME_UNSET || g_app.time_quality == LG_TIME_AUTHORITATIVE) {
        return;   /* nothing to take, or this AP is where the time was set */
    }
    uint8_t theirs = t->stratum;
    uint8_t mine_stratum = g_app.time_quality == LG_TIME_UNSET ? LG_STRATUM_UNKNOWN : g_app.time_stratum;
    bool have_none = g_app.time_quality == LG_TIME_UNSET;
    /* Only from closer to the source. An unknown stratum is as far as can be, so two APs that
     * both lost track can still agree, but never override an AP that knows it is closer. */
    if (!have_none && !(theirs < mine_stratum || (theirs == LG_STRATUM_UNKNOWN && mine_stratum == LG_STRATUM_UNKNOWN))) {
        return;
    }
    uint8_t next_stratum = theirs >= LG_STRATUM_UNKNOWN - 1u ? LG_STRATUM_UNKNOWN : (uint8_t)(theirs + 1u);
    uint64_t sent_ms = (uint64_t)t->grid_time * 1000u + t->millis;
    if (have_none) {
        set_grid_time_ms(sent_ms, LG_TIME_CARRIED, next_stratum);
        grid_state_note_sync(origin_node, 0);
        ESP_LOGI("TIME", "[TIME] Took grid time %" PRIu32 ".%03u from AP %u (stratum %u)", t->grid_time, t->millis,
                 origin_node, next_stratum);
        return;
    }
    int64_t diff = (int64_t)sent_ms - (int64_t)app_grid_time_ms();
    g_app.time_stratum = next_stratum;
    grid_state_note_sync(origin_node, (int32_t)(diff > INT32_MAX ? INT32_MAX : diff < INT32_MIN ? INT32_MIN : diff));
    if (diff > -TIME_DEADBAND_MS && diff < TIME_DEADBAND_MS) {
        return;   /* agrees: the difference is delay on the way, not error */
    }
    if (diff >= TIME_STEP_MS || diff <= -TIME_STEP_MS) {
        set_grid_time_ms(sent_ms, LG_TIME_CARRIED, next_stratum);
        ESP_LOGW("TIME", "[TIME] Stepped grid time by %+" PRId64 " ms from AP %u", diff, origin_node);
    } else {
        g_app.time_slew_ms = (int32_t)diff;
        ESP_LOGI("TIME", "[TIME] Slewing grid time by %+" PRId64 " ms from AP %u", diff, origin_node);
    }
}

/*
 * Grid time back from a handheld (D48, D53). This AP restarted with no clock, and a handheld
 * whose clock the grid set is registering. Its distance from the source is unknown, so any AP
 * that knows it is closer corrects this one at the next TIME_SYNC. Other APs with no time get it
 * from here straight away.
 */
static void io_on_client_time(void *ctx, uint32_t device, uint32_t unix_s)
{
    (void)ctx;
    if (g_app.time_quality != LG_TIME_UNSET) {
        return;
    }
    if (unix_s < 1700000000u) {
        /* Before 2023-11-14 is not a clock the grid ever set. Logged, because a silent drop here
         * looks exactly like the carry-back never happening. */
        ESP_LOGW("TIME", "[TIME] Handheld %" PRIu32 " offered grid time %" PRIu32 ": too old to believe", device,
                 unix_s);
        return;
    }
    set_grid_time_ms((uint64_t)unix_s * 1000u, LG_TIME_CARRIED, LG_STRATUM_UNKNOWN);
    grid_state_note_sync(GRID_NO_AP, 0);
    ESP_LOGI("TIME", "[TIME] Took grid time %" PRIu32 " from handheld %" PRIu32 " (this AP had none)", unix_s, device);
    lg_node_announce_time(&g_app.core, LG_TIME_CARRIED);
}

/* ---- backbone callbacks ---- */

static void on_backbone_frame(uint16_t from_node, const uint8_t *frame, size_t len)
{
    lg_node_on_backbone_frame(&g_app.core, from_node, frame, len);
}

static void on_link(uint16_t node, bool up)
{
    ptrace_event(up ? PTRACE_LINK_UP : PTRACE_LINK_DOWN);
    if (up) {
        lg_node_on_neighbor_up(&g_app.core, node);
        /* A returning or new AP learns everything it missed: settings first, so a time
         * generation it has not seen demotes it before the time itself arrives (D45). */
        grid_state_announce();
        (void)lg_node_announce_groups(&g_app.core);
        grid_state_avail_announce();   /* a returning AP fills the minutes it missed from this */
        if (g_app.time_quality != LG_TIME_UNSET) {
            lg_node_announce_time(&g_app.core, g_app.time_quality);
        }
    }
}

static uint8_t clients_count(void)
{
    return sess_registered_count();
}

/* ---- discovery payload: beacons and BLE ---- */

/* Fixed discovery part (LG_DISC_LEN bytes), shared by the Wi-Fi vendor IE and BLE. */
static size_t build_discovery(uint8_t out[LG_DISC_LEN])
{
    uint8_t clients = sess_registered_count();
    out[0] = LG_DISC_MAGIC0;
    out[1] = LG_DISC_MAGIC1;
    out[2] = LG_DISC_VERSION;
    memcpy(out + 3, s_discriminator, 4);
    out[7] = (uint8_t)g_app.index;
    out[8] = 0;   /* was the master flag; there is no master AP (D45) */
    out[9] = (uint8_t)(LG_PROTO_MAX_STATIONS - clients);
    out[10] = (uint8_t)((lgbb_link_count() > 0 ? LG_DISC_FLAG_BACKBONE : 0) |
                        (g_app.time_quality != LG_TIME_UNSET ? LG_DISC_FLAG_TIME : 0));
    out[11] = clients;
    return LG_DISC_LEN;
}

static void refresh_discovery(void)
{
    uint8_t payload[LG_DISC_LEN];
    build_discovery(payload);
    if (payload[11] == s_last_disc_clients && payload[10] == s_last_disc_flags) {
        return;
    }
    s_last_disc_clients = payload[11];
    s_last_disc_flags = payload[10];

    /* Wi-Fi carries the AP name after the fixed part: every AP shares one SSID (D46), so this
     * is the only per-AP label a handheld can show. BLE keeps the fixed part only. */
    static const uint8_t oui[3] = LG_VENDOR_OUI;
    uint8_t ie[sizeof(vendor_ie_data_t) + LG_DISC_WIFI_MAX];
    size_t name_len = strnlen(g_app.name, LG_DISC_NAME_MAX);
    size_t wifi_len = LG_DISC_LEN + 1 + name_len;
    vendor_ie_data_t *v = (vendor_ie_data_t *)ie;
    v->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    v->length = (uint8_t)(4 + wifi_len);
    memcpy(v->vendor_oui, oui, 3);
    v->vendor_oui_type = LG_VENDOR_OUI_TYPE;
    memcpy(ie + sizeof(vendor_ie_data_t), payload, LG_DISC_LEN);
    ie[sizeof(vendor_ie_data_t) + LG_DISC_LEN] = (uint8_t)name_len;
    memcpy(ie + sizeof(vendor_ie_data_t) + LG_DISC_LEN + 1, g_app.name, name_len);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
#if NODE_BLE_ADV_ENABLED
    ble_adv_update(payload, LG_DISC_LEN);
#endif
}

/* ---- identity and Wi-Fi ---- */

static uint32_t next_boot_counter(void)
{
    nvs_handle_t h;
    uint32_t boot = 0;
    ESP_ERROR_CHECK(nvs_open("lg", NVS_READWRITE, &h));
    (void)nvs_get_u32(h, "boot", &boot);
    boot++;
    ESP_ERROR_CHECK(nvs_set_u32(h, "boot", boot));
    ESP_ERROR_CHECK(nvs_commit(h));   /* committed before any radio transmit: nonce uniqueness */
    nvs_close(h);
    return boot;
}

/*
 * Why this board last restarted, kept so a crash leaves evidence.
 *
 * Opening a serial port restarts an AP, so a crash that happened an hour ago is gone by the
 * time anybody looks. Each boot therefore reads esp_reset_reason() and adds one to a counter
 * for that kind of restart in NVS, and `status` prints the counts. One NVS write per boot, next
 * to the boot counter, so flash wear is unchanged in kind.
 *
 * On the classic ESP32 a pulse on EN (the serial tools' reset, or the button) reports the same
 * as a power-on, so "power-on or reset" covers both. Crashes and watchdogs are distinct.
 *
 * How long the previous run lasted survives in RTC memory, which a crash or software restart
 * keeps and a power loss clears; it is printed only when it is valid.
 */
typedef struct {
    esp_reset_reason_t reason;
    const char        *key;    /* NVS key, at most 15 characters */
    const char        *text;
} restart_kind_t;

static const restart_kind_t RESTART_KINDS[] = {
    { ESP_RST_POWERON,   "rr_power",    "power-on or reset" },
    { ESP_RST_EXT,       "rr_ext",      "external reset" },
    { ESP_RST_SW,        "rr_sw",       "software restart" },
    { ESP_RST_PANIC,     "rr_panic",    "crash (panic)" },
    { ESP_RST_INT_WDT,   "rr_int_wdt",  "crash (interrupt watchdog)" },
    { ESP_RST_TASK_WDT,  "rr_task_wdt", "crash (task watchdog)" },
    { ESP_RST_WDT,       "rr_wdt",      "crash (other watchdog)" },
    { ESP_RST_BROWNOUT,  "rr_brownout", "low supply voltage (brownout)" },
    { ESP_RST_DEEPSLEEP, "rr_sleep",    "wake from deep sleep" },
    { ESP_RST_UNKNOWN,   "rr_unknown",  "unknown" },
};

#define RUN_MARK 0x4C475255u   /* "LGRU": the RTC record below was written by this firmware */

static RTC_NOINIT_ATTR uint32_t s_run_mark;
static RTC_NOINIT_ATTR uint32_t s_run_uptime_s;

static const restart_kind_t *restart_kind(esp_reset_reason_t reason)
{
    for (size_t i = 0; i < sizeof(RESTART_KINDS) / sizeof(RESTART_KINDS[0]); i++) {
        if (RESTART_KINDS[i].reason == reason) {
            return &RESTART_KINDS[i];
        }
    }
    return &RESTART_KINDS[sizeof(RESTART_KINDS) / sizeof(RESTART_KINDS[0]) - 1];
}

static void record_restart(void)
{
    const restart_kind_t *kind = restart_kind(esp_reset_reason());
    nvs_handle_t h;
    if (nvs_open("lg", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t n = 0;
        (void)nvs_get_u32(h, kind->key, &n);
        if (nvs_set_u32(h, kind->key, n + 1) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    bool ran = s_run_mark == RUN_MARK && kind->reason != ESP_RST_POWERON && kind->reason != ESP_RST_BROWNOUT;
    if (ran) {
        ESP_LOGW(TAG, "[GRID] Last restart: %s, after %" PRIu32 " s running", kind->text, s_run_uptime_s);
    } else {
        ESP_LOGI(TAG, "[GRID] Last restart: %s", kind->text);
    }
    s_run_mark = RUN_MARK;
    s_run_uptime_s = 0;
    ptrace_event(PTRACE_NVS_WRITE);
    ptrace_boot(kind->text);
}

static const char *quality_name(uint8_t q);

/* Once a second: which APs this AP can reach over the backbone, for the availability graph. */
static void record_availability(uint32_t now)
{
    bool up[LG_MAX_NODES] = { false };
    if (g_app.index < LG_MAX_NODES) {
        up[g_app.index] = true;
    }
    lgbb_link_info_t links[LG_MAX_NODES];
    size_t n = lgbb_links(links, LG_MAX_NODES, now);
    for (size_t i = 0; i < n; i++) {
        if (links[i].up && links[i].node < LG_MAX_NODES) {
            up[links[i].node] = true;
        }
    }
    grid_state_avail_second(up);
}

/* Called once a second so a crash can say how long the board had been up. */
static void note_uptime(void)
{
    s_run_uptime_s = app_now_ms() / 1000u;
}

void node_print_restarts(void)
{
    nvs_handle_t h;
    if (nvs_open("lg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    printf("Restarts since counting began:");
    bool any = false;
    for (size_t i = 0; i < sizeof(RESTART_KINDS) / sizeof(RESTART_KINDS[0]); i++) {
        uint32_t n = 0;
        if (nvs_get_u32(h, RESTART_KINDS[i].key, &n) == ESP_OK && n > 0) {
            printf("%s %s %" PRIu32, any ? "," : "", RESTART_KINDS[i].text, n);
            any = true;
        }
    }
    printf("%s\n", any ? "" : " none recorded");
    nvs_close(h);
}

static void identify_node(void)
{
    if (lg_identity_load(&g_app.identity) != ESP_OK) {
        memset(&g_app.identity, 0, sizeof(g_app.identity));
    }
    lg_identity_print(&g_app.identity);
    if (g_app.identity.present && g_app.identity.has_node && strcmp(g_app.identity.role, "N") == 0) {
        g_app.index = g_app.identity.node_index;
        g_app.name = g_app.identity.node_name;
    } else {
        g_app.index = LG_PROTO_UNKNOWN_NODE;
        g_app.name = "UNKNOWN";
        ESP_LOGW(TAG, "[GRID] No node identity on this board; provision it with tools/flash.py. Using node index %d",
                 LG_PROTO_UNKNOWN_NODE);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        /* Decision D21: no hardware addresses in output; the association id is enough to follow a station. */
        ESP_LOGI("NET", "[NET] Station joined (aid %u)", e->aid);
        ptrace_event(PTRACE_STA_JOIN);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI("NET", "[NET] Station left (aid %u, reason %u)", e->aid, e->reason);
        ptrace_event(PTRACE_STA_LEAVE);
    }
}

static void wifi_start(void)
{
    /* The Wi-Fi driver logs the SoftAP MAC at INFO ("wifi:mode : softAP (...)"); decision D21 keeps MACs out of output. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    uint8_t ip[4];
    lg_proto_node_ip(g_app.index, ip);
    esp_netif_ip_info_t info = { 0 };
    esp_netif_set_ip4_addr(&info.ip, ip[0], ip[1], ip[2], ip[3]);
    esp_netif_set_ip4_addr(&info.gw, ip[0], ip[1], ip[2], ip[3]);
    esp_netif_set_ip4_addr(&info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &info));
    /* DHCP serves phones only, from this AP's own block (D46: every AP shares the subnet, so the
     * blocks never overlap). Handhelds use static .100 + device index (answer 10); the default
     * pool would run to .101 and could hand out a handheld's address. */
    uint8_t first, last;
    lg_proto_dhcp_block(g_app.index, &first, &last);
    dhcps_lease_t lease = { .enable = true };
    IP4_ADDR(&lease.start_ip, ip[0], ip[1], ip[2], first);
    IP4_ADDR(&lease.end_ip, ip[0], ip[1], ip[2], last);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease,
                                           sizeof(lease)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wc = { 0 };
    _Static_assert(sizeof(LG_PROTO_SSID) <= sizeof(wc.ap.ssid), "SSID too long");
    memcpy(wc.ap.ssid, LG_PROTO_SSID, sizeof(LG_PROTO_SSID) - 1);
    wc.ap.ssid_len = (uint8_t)(sizeof(LG_PROTO_SSID) - 1);
    strncpy((char *)wc.ap.password, LG_SECRET_WIFI_PASSPHRASE, sizeof(wc.ap.password) - 1);
    wc.ap.channel = LG_PROTO_CHANNEL;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = LG_PROTO_MAX_STATIONS;
    wc.ap.beacon_interval = 100;
    wc.ap.dtim_period = 1;
    wc.ap.pmf_cfg.capable = NODE_PMF_CAPABLE;   /* node_app.h: A/B switch for reason-2 disassociations */
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_LG_NODE_WIFI_TX_POWER_QDBM > 0
    /* A/B test for brownouts: transmit bursts are the largest current spikes on this board. */
    if (esp_wifi_set_max_tx_power((int8_t)CONFIG_LG_NODE_WIFI_TX_POWER_QDBM) != ESP_OK) {
        ESP_LOGW("NET", "[NET] Transmit power ceiling not applied");
    }
#endif
    int8_t tx_qdbm = 0;
    if (esp_wifi_get_max_tx_power(&tx_qdbm) == ESP_OK) {
        ESP_LOGI("NET", "[NET] Transmit power ceiling %d.%02d dBm", tx_qdbm / 4, (tx_qdbm % 4) * 25);
    }
    ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_AP, 30));
    ESP_LOGI("NET", "[NET] SoftAP \"%s\" (AP %u %s) on channel %d at %u.%u.%u.%u, phone DHCP .%u to .%u, PMF %s",
             LG_PROTO_SSID, g_app.index, g_app.name, LG_PROTO_CHANNEL, ip[0], ip[1], ip[2], ip[3], first, last,
             NODE_PMF_CAPABLE ? "capable" : "off (A/B build)");
}

/* ---- console command handling (core task) ---- */

static const char *quality_name(uint8_t q)
{
    return q == LG_TIME_AUTHORITATIVE ? "AUTHORITATIVE" : q == LG_TIME_CARRIED ? "CARRIED" : "UNSET";
}

static void print_status(void)
{
    const lg_node_stats_t *st = &g_app.core.stats;
    printf("AP %u %s, boot %" PRIu32 ", uptime %" PRIu32 " s\n", g_app.index, g_app.name, g_app.boot,
           app_now_ms() / 1000);
    uint32_t t = app_grid_time();
    if (t != 0) {
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        printf("Grid time %" PRIu32 " (%04d-%02d-%02d %02d:%02d:%02d UTC), %s\n", t, tm.tm_year + 1900,
               tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, quality_name(g_app.time_quality));
    } else {
        printf("Grid time UNSET: handhelds can only receive and send urgent broadcasts\n");
    }
    printf("Links %u, handhelds %u\n", lgbb_link_count(), sess_registered_count());
    printf("Heap free %" PRIu32 ", min %" PRIu32 ", largest block %u\n", esp_get_free_heap_size(),
           esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    printf("Core: rx_client %" PRIu32 " rx_backbone %" PRIu32 " delivered %" PRIu32 " forwarded %" PRIu32
           " duplicates %" PRIu32 " rejected %" PRIu32 " malformed %" PRIu32 "\n",
           st->rx_client, st->rx_backbone, st->delivered_local, st->forwarded, st->duplicates, st->rejected,
           st->malformed);
    grid_state_print();
    node_print_restarts();
}

/* Decision D28: every device answers configuration questions over its serial port.
 * Read-only: settings change on the admin page of any AP (D45). Secrets are never printed. */
static void print_config(void)
{
    node_settings_t cfg;
    grid_state_settings(&cfg);
    uint8_t ip[4];
    lg_proto_node_ip(g_app.index, ip);
    printf("AP configuration\n");
    printf("  id: %s\n", g_app.identity.present ? g_app.identity.id : "none");
    printf("  AP: index %u, name %s (every AP is equal; there is no master, D45)\n", g_app.index, g_app.name);
    uint8_t first, last;
    lg_proto_dhcp_block(g_app.index, &first, &last);
    printf("  network: \"%s\" on every AP (D46), channel %d, address %u.%u.%u.%u on every AP, up to %d stations\n",
           LG_PROTO_SSID, LG_PROTO_CHANNEL, ip[0], ip[1], ip[2], ip[3], LG_PROTO_MAX_STATIONS);
    printf("  DHCP for phones on this AP: %u.%u.%u.%u to %u.%u.%u.%u; handhelds are static at .%u plus device index\n",
           ip[0], ip[1], ip[2], first, ip[0], ip[1], ip[2], last, (unsigned)LG_PROTO_HANDHELD_HOST_BASE);
    printf("  BLE advertising: %s; PMF: %s\n", NODE_BLE_ADV_ENABLED ? "on" : "off (A/B build)",
           NODE_PMF_CAPABLE ? "capable" : "off (A/B build)");
    if (cfg.configured) {
        printf("  grid name: %s\n", cfg.grid_name);
        printf("  time zone: %s (display only)\n", cfg.timezone[0] ? cfg.timezone : "not set");
        printf("  admin password: set, %" PRIu32 " PBKDF2 iterations\n", cfg.iterations);
    } else {
        printf("  admin setup: not done; any AP serves the setup page\n");
    }
    printf("  settings version %" PRIu32 ", made on AP %u; copied to every AP over the backbone\n", cfg.seq, cfg.author);
    printf("  grid time: %s\n", quality_name(g_app.time_quality));
    printf("  settings change on the admin page, http://%u.%u.%u.%u/ on any AP, not over serial\n", ip[0], ip[1],
           ip[2], ip[3]);
}

static void print_devices(void)
{
    const lg_roster_t *r = g_app.core.roster;
    printf("Grid presence as seen by node %u:\n", g_app.index);
    printf("  DEVICE  NAME     STATE    NODE\n");
    for (size_t i = 0; i < r->n_users; i++) {
        const lg_presence_entry_t *p = lg_node_presence(&g_app.core, r->users[i].device);
        const char *state = p == NULL ? "UNKNOWN" : p->state == LG_PRES_ONLINE ? "ONLINE" : "OFFLINE";
        if (p != NULL && p->node != LG_NODE_NONE) {
            printf("  %-6" PRIu32 "  %-7s  %-7s  %u\n", r->users[i].device, r->users[i].name, state, p->node);
        } else {
            printf("  %-6" PRIu32 "  %-7s  %-7s  --\n", r->users[i].device, r->users[i].name, state);
        }
    }
    sess_print();
}

/* GPS (D63), defined below the command handler. */
static bool s_gps_owns;   /* this AP's authority came from the GPS, not from a hand */
static void gps_time(const node_cmd_t *cmd);
static void print_gps(void);

static void handle_command(const node_cmd_t *cmd)
{
    switch (cmd->type) {
    case NODE_CMD_CONFIG:
        print_config();
        break;
    case NODE_CMD_GRID_ANNOUNCE:
        grid_state_announce();
        break;
    case NODE_CMD_GROUP_EDIT: {
        uint8_t status = lg_node_edit_groups(&g_app.core, &cmd->group);
        if (status != 0) {
            ESP_LOGW(TAG, "[GRID] Admin group edit (op %u, group %u) refused: status %u", cmd->group.op,
                     cmd->group.id, status);
        }
        break;
    }
    case NODE_CMD_GROUPS: {
        const lg_roster_t *r = &g_app.roster;
        printf("Groups version %" PRIu32 " (made on AP %u), next id %u:\n", r->groups.seq, r->groups.author,
               r->groups.next_id);
        for (size_t i = 0; i < r->groups.count; i++) {
            const lg_group_t *g = &r->groups.groups[i];
            printf("  %-3u %-15s", g->id, g->name);
            for (size_t u = 0; u < r->n_users && u < 32u; u++) {
                if (g->members & (1u << u)) {
                    printf(" %s,", r->users[u].name);
                }
            }
            printf("\n");
        }
        if (r->groups.count == 0) {
            printf("  none; make them on the admin page or a handheld\n");
        }
        printf("Announcements (D56): ");
        if (r->groups.announcers == LG_ANNOUNCE_EVERYONE) {
            printf("everyone\n");
        } else {
            for (size_t u = 0; u < r->n_users && u < 32u; u++) {
                if (r->groups.announcers & (1u << u)) {
                    printf("%s, ", r->users[u].name);
                }
            }
            printf("%s\n", r->groups.announcers == 0 ? "nobody" : "and nobody else");
        }
        break;
    }
    case NODE_CMD_STATUS:
        print_status();
        break;
    case NODE_CMD_NODES:
        lgbb_print();
        break;
    case NODE_CMD_DEVICES:
        print_devices();
        break;
    case NODE_CMD_PING: {
        int rc = lg_node_send_diag(&g_app.core, (const uint8_t *)cmd->text, strlen(cmd->text));
        printf(rc == 0 ? "Echo sent to %u link(s): %s\n" : "Echo not sent (%u links): %s\n", lgbb_link_count(), cmd->text);
        break;
    }
    case NODE_CMD_TIME_SHOW:
        printf("Grid time %" PRIu32 " (%s)\n", app_grid_time(), quality_name(g_app.time_quality));
        break;
    case NODE_CMD_GPS_TIME:
        gps_time(cmd);
        break;
    case NODE_CMD_GPS:
        print_gps();
        break;
    case NODE_CMD_TIME_SET:
        if (gps_has_fix()) {
            /* D63: the GPS wins while it has a fix. The admin page refuses first; this is the console. */
            printf("Not set: grid time comes from the GPS while it has a fix\n");
            break;
        }
        s_gps_owns = false;
        set_grid_time_ms((uint64_t)cmd->value * 1000u + cmd->millis, LG_TIME_AUTHORITATIVE, 0);
        grid_state_time_set_here(cmd->value, false);
        grid_state_announce();   /* the new generation goes first, so other APs stop defending theirs */
        lg_node_announce_time(&g_app.core, LG_TIME_AUTHORITATIVE);
        printf("Grid time set to %" PRIu32 " and announced\n", cmd->value);
        break;
    }
}

/* ---- GPS (D63) ---- */

/*
 * Grid time from MAIN's GPS. The first fix, or a fix after the grid followed a time set elsewhere,
 * makes this AP the source exactly as setting it on the admin page does: AUTHORITATIVE at stratum
 * 0, a new time generation, announced. After that each second's fix only corrects the clock:
 * slewed when within a second, stepped and announced beyond it. The other APs follow as they do
 * for a hand-set time. If someone sets the time on another AP's page, that generation demotes
 * MAIN, and the next fix takes the grid back: the GPS wins.
 */

/*
 * This AP's own position on the grid (D65): shared with every AP and handheld, in RAM only, so the
 * handhelds can show distance and direction to MAIN and every admin page can draw it. Sent at most
 * every POS_EVERY_MS, or at once when the fix moved more than POS_MOVE_M.
 */
#define POS_EVERY_MS 30000u
#define POS_MOVE_M   20.0f

static void gps_position(uint32_t fix_unix)
{
    static bool     s_sent;
    static uint32_t s_sent_ms;
    static int32_t  s_lat, s_lon;
    gps_state_t st;
    gps_state(&st);
    if (!st.fix || !st.has_pos) {
        return;
    }
    uint32_t now = app_now_ms();
    bool moved = false;
    if (s_sent) {
        /* Equirectangular: exact enough over tens of metres. 1 microdegree of latitude = 0.111 m. */
        float dy = (float)(st.lat_u - s_lat) * 0.111f;
        float dx = (float)(st.lon_u - s_lon) * 0.111f * cosf((float)st.lat_u * 1.745329e-8f);
        moved = dx * dx + dy * dy > POS_MOVE_M * POS_MOVE_M;
        if (!moved && now - s_sent_ms < POS_EVERY_MS) {
            return;
        }
    }
    int rc = lg_node_set_own_position(&g_app.core, st.lat_u, st.lon_u, fix_unix, st.sats);
    if (rc != LG_OK) {
        return;   /* no newer than the fix already shared */
    }
    if (!s_sent || moved) {
        ESP_LOGI(TAG, "[GRID] Shared this AP's GPS position (%u satellites)%s", st.sats, s_sent ? ": it moved" : "");
    }
    s_sent = true;
    s_sent_ms = now;
    s_lat = st.lat_u;
    s_lon = st.lon_u;
}

static void gps_time(const node_cmd_t *cmd)
{
    gps_position(cmd->value);
    uint64_t gps_ms = (uint64_t)cmd->value * 1000u + cmd->millis + (app_now_ms() - cmd->at_ms);
    if (!s_gps_owns || g_app.time_quality != LG_TIME_AUTHORITATIVE) {
        set_grid_time_ms(gps_ms, LG_TIME_AUTHORITATIVE, 0);
        grid_state_time_set_here(cmd->value, true);
        grid_state_announce();   /* the new generation first, so other APs stop defending theirs */
        lg_node_announce_time(&g_app.core, LG_TIME_AUTHORITATIVE);
        s_gps_owns = true;
        gps_state_t st;
        gps_state(&st);
        ESP_LOGI("TIME", "[TIME] Grid time %" PRIu32 " from GPS (%u satellites); announced", cmd->value, st.sats);
        return;
    }
    int64_t diff = (int64_t)gps_ms - (int64_t)app_grid_time_ms();
    if (diff > -TIME_DEADBAND_MS && diff < TIME_DEADBAND_MS) {
        return;
    }
    if (diff >= TIME_STEP_MS || diff <= -TIME_STEP_MS) {
        set_grid_time_ms(gps_ms, LG_TIME_AUTHORITATIVE, 0);
        lg_node_announce_time(&g_app.core, LG_TIME_AUTHORITATIVE);
        ESP_LOGW("TIME", "[TIME] Stepped grid time by %+" PRId64 " ms to the GPS", diff);
    } else {
        g_app.time_slew_ms = (int32_t)diff;
    }
}

static void print_gps(void)
{
    gps_state_t st;
    gps_state(&st);
    if (!st.started) {
        printf("GPS: not listening (only MAIN reads one; pin CONFIG_LG_NODE_GPS_RX_GPIO)\n");
        return;
    }
    printf("GPS: %s, %u satellites, %" PRIu32 " sentences, %" PRIu32 " bad checksums\n",
           !st.heard ? "none heard (not fitted, or check module TXD to GPIO16)" : st.fix ? "FIX" : "no fix yet",
           st.sats, st.sentences, st.bad);
    if (st.last_unix != 0) {
        printf("GPS: last fix %" PRIu32 ", %" PRIu32 " ms ago; grid time %s\n", st.last_unix, st.fix_age_ms,
               s_gps_owns && g_app.time_quality == LG_TIME_AUTHORITATIVE ? "comes from it" : "does not come from it");
    }
}

/* ---- core task ---- */

static void core_task(void *arg)
{
    (void)arg;
    uint32_t last_status = 0;
    uint32_t last_announce = 0;
    uint32_t last_grid_state = 0;
    uint32_t last_disc = 0;
    node_cmd_t cmd;
    /*
     * The task watchdog watches this loop, not only the idle tasks: a core task stuck for 5 s
     * now panics with a backtrace of where it was, restarts, and is counted as a task-watchdog
     * restart, with the power trace showing the seconds before it (CONFIG_ESP_TASK_WDT_PANIC).
     */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "[GRID] Task watchdog not watching the core task");
    }
    for (;;) {
        esp_task_wdt_reset();
        sess_poll(20);
        uint32_t now = app_now_ms();
        static uint32_t last_slew;
        app_time_slew(now - last_slew);
        last_slew = now;
        lgbb_poll(now);
        while (xQueueReceive(g_app.cmd_queue, &cmd, 0) == pdTRUE) {
            handle_command(&cmd);
        }
        if (now - last_disc >= 1000) {
            last_disc = now;
            note_uptime();
            ptrace_second(now / 1000u, lgbb_tx_frame_count(), lgbb_link_count(), quality_name(g_app.time_quality));
            record_availability(now);
            refresh_discovery();
            web_admin_publish_snapshot();
        }
        if (now - last_grid_state >= GRID_STATE_ANNOUNCE_MS) {
            last_grid_state = now;
            grid_state_announce();
            (void)lg_node_announce_groups(&g_app.core);   /* an AP that missed a change catches up */
        }
        static uint32_t last_names;
        if (now - last_names >= NAMES_ANNOUNCE_MS) {
            last_names = now;
            lg_node_announce_names(&g_app.core);
        }
        if (g_app.time_quality != LG_TIME_UNSET && now - last_announce >= TIME_ANNOUNCE_MS) {
            /* Every AP with time repeats it, not only the one it was set on: with strata a copy
             * can never pull a closer AP back, and it keeps each AP's last sync recent. */
            last_announce = now;
            lg_node_announce_time(&g_app.core, g_app.time_quality);
        }
        if (now - last_status >= STATUS_LOG_MS) {
            last_status = now;
            ESP_LOGI(TAG, "[GRID] links %u handhelds %u heap %" PRIu32 " min %" PRIu32 " time %s",
                     lgbb_link_count(), sess_registered_count(), esp_get_free_heap_size(),
                     esp_get_minimum_free_heap_size(), quality_name(g_app.time_quality));
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    memset(&g_app, 0, sizeof(g_app));
    identify_node();
    g_app.boot = next_boot_counter();
    g_app.cmd_queue = xQueueCreate(4, sizeof(node_cmd_t));
    ESP_LOGI(TAG, "[GRID] LocalGrid node %u %s starting, boot %" PRIu32, g_app.index, g_app.name, g_app.boot);
    record_restart();

    ESP_ERROR_CHECK(lg_crypto_init() == 0 ? ESP_OK : ESP_FAIL);
    wifi_start();
    ESP_ERROR_CHECK(lg_hkdf_sha256(NULL, 0, s_backbone_key, sizeof(s_backbone_key), (const uint8_t *)"lg-disc", 7,
                                   s_discriminator, sizeof(s_discriminator)) == 0 ? ESP_OK : ESP_FAIL);

    const lg_node_io_t io = {
        .ctx = NULL,
        .to_client = io_to_client,
        .backbone_unicast = io_unicast,
        .backbone_flood = io_flood,
        .is_neighbor = io_is_neighbor,
        .now_ms = io_now_ms,
        .grid_time = io_grid_time,
        .on_diag = io_on_diag,
        .on_time = io_on_time,
        .time_now = io_time_now,
        .on_grid_state = io_on_grid_state,
        .on_groups_changed = io_on_groups_changed,
        .on_client_time = io_on_client_time,
        .on_name = io_on_name,
    };
    lg_roster_init_prototype(&g_app.roster);
    if (settings_groups_load(&g_app.roster.groups) == ESP_OK) {
        ESP_LOGI(TAG, "[GRID] Groups version %" PRIu32 " (made on AP %u): %u group(s)", g_app.roster.groups.seq,
                 g_app.roster.groups.author, g_app.roster.groups.count);
    } else {
        ESP_LOGI(TAG, "[GRID] No groups saved; waiting for another AP or an edit");
    }
    lg_node_init(&g_app.core, g_app.index, g_app.boot, &g_app.roster, &io);
    load_names();

    /*
     * The clock this AP had before it restarted (D60). The RTC counted the seconds it was away, so
     * this is the time now rather than the time then; it comes back as CARRIED at unknown stratum,
     * so any AP that knows it is closer to where an admin set it corrects this one at once.
     */
    uint32_t kept = lg_timekeep_restore();
    if (kept != 0) {
        set_grid_time_ms((uint64_t)kept * 1000u, LG_TIME_CARRIED, LG_STRATUM_UNKNOWN);
        grid_state_note_sync(GRID_NO_AP, 0);
        ESP_LOGI("TIME", "[TIME] Grid time %" PRIu32 " kept across the restart", kept);
    }

    grid_state_init(g_app.index);
    ESP_ERROR_CHECK(lgbb_init(g_app.index, g_app.boot, s_backbone_key, on_backbone_frame, on_link, clients_count));
    ESP_ERROR_CHECK(sess_init(LG_PROTO_TCP_PORT));
    /* Every AP serves the admin page (D45). */
    if (web_admin_start() != ESP_OK) {
        ESP_LOGE(TAG, "[WEB] Admin page failed to start");
    } else {
        web_admin_publish_snapshot();
    }

#if NODE_BLE_ADV_ENABLED
    uint8_t payload[LG_DISC_LEN];
    build_discovery(payload);
    esp_log_level_set("BTDM_INIT", ESP_LOG_WARN);   /* controller init logs "Bluetooth MAC: ..." at INFO (D21) */
    if (ble_adv_init(payload, sizeof(payload)) != ESP_OK) {
        ESP_LOGE(TAG, "[BLE] Disabled: init failed");
    }
#else
    ESP_LOGW("BLE", "[BLE] Advertising disabled in this build (NODE_BLE_ADV_ENABLED 0, coexistence A/B test)");
#endif

    xTaskCreate(core_task, "lg_core", 8192, NULL, 5, NULL);
    if (g_app.index == 0 && CONFIG_LG_NODE_GPS_RX_GPIO >= 0) {
        (void)gps_start(CONFIG_LG_NODE_GPS_RX_GPIO);   /* D63: MAIN only */
    }
    console_start();
}
