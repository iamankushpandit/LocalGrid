/*
 * LocalGrid infrastructure node (prototype).
 *
 * SoftAP on a fixed channel for handhelds, TCP control sessions, ESP-NOW
 * backbone to other nodes, BLE discovery adverts, serial console.
 * Node index and name come from the identity partition (decisions D20, D21).
 */
#include <inttypes.h>
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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "grid_state.h"
#include "lg_crypto.h"
#include "lg_proto_config.h"
#include "lg_secrets.h"
#include "node_app.h"
#include "nvs_flash.h"
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

/* ---- time ---- */

uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

uint32_t app_grid_time(void)
{
    if (g_app.time_quality == LG_TIME_UNSET) {
        return 0;
    }
    return (uint32_t)(mono_s() + g_app.time_offset_s);
}

static void set_grid_time(uint32_t unix_s, uint8_t quality)
{
    int64_t new_offset = (int64_t)unix_s - mono_s();
    if (g_app.time_quality != LG_TIME_UNSET) {
        ESP_LOGI("TIME", "[TIME] Offset corrected %+" PRId64 " s", new_offset - g_app.time_offset_s);
    }
    g_app.time_offset_s = new_offset;
    g_app.time_quality = quality;
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

static void io_on_time(void *ctx, uint16_t origin_node, uint32_t grid_time, uint8_t quality)
{
    (void)ctx;
    if (grid_time == 0 || quality == LG_TIME_UNSET || g_app.time_quality == LG_TIME_AUTHORITATIVE) {
        return;
    }
    uint32_t mine = app_grid_time();
    if (g_app.time_quality == LG_TIME_UNSET || mine > grid_time + 1 || grid_time > mine + 1) {
        set_grid_time(grid_time, LG_TIME_CARRIED);
        ESP_LOGI("TIME", "[TIME] Adopted grid time %" PRIu32 " from node %u", grid_time, origin_node);
    }
}

/* ---- backbone callbacks ---- */

static void on_backbone_frame(uint16_t from_node, const uint8_t *frame, size_t len)
{
    lg_node_on_backbone_frame(&g_app.core, from_node, frame, len);
}

static void on_link(uint16_t node, bool up)
{
    if (up) {
        lg_node_on_neighbor_up(&g_app.core, node);
        /* A returning or new AP learns everything it missed: settings first, so a time
         * generation it has not seen demotes it before the time itself arrives (D45). */
        grid_state_announce();
        (void)lg_node_announce_groups(&g_app.core);
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
    out[10] = lgbb_link_count() > 0 ? LG_DISC_FLAG_BACKBONE : 0;
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
}

/* Called once a second so a crash can say how long the board had been up. */
static void note_uptime(void)
{
    s_run_uptime_s = app_now_ms() / 1000u;
}

static void print_restarts(void)
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
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI("NET", "[NET] Station left (aid %u, reason %u)", e->aid, e->reason);
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
    print_restarts();
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
    case NODE_CMD_TIME_SET:
        set_grid_time(cmd->value, LG_TIME_AUTHORITATIVE);
        grid_state_time_set_here();
        grid_state_announce();   /* the new generation goes first, so other APs stop defending theirs */
        lg_node_announce_time(&g_app.core, LG_TIME_AUTHORITATIVE);
        printf("Grid time set to %" PRIu32 " and announced\n", cmd->value);
        break;
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
    for (;;) {
        sess_poll(20);
        uint32_t now = app_now_ms();
        lgbb_poll(now);
        while (xQueueReceive(g_app.cmd_queue, &cmd, 0) == pdTRUE) {
            handle_command(&cmd);
        }
        if (now - last_disc >= 1000) {
            last_disc = now;
            note_uptime();
            refresh_discovery();
            web_admin_publish_snapshot();
        }
        if (now - last_grid_state >= GRID_STATE_ANNOUNCE_MS) {
            last_grid_state = now;
            grid_state_announce();
            (void)lg_node_announce_groups(&g_app.core);   /* an AP that missed a change catches up */
        }
        if (g_app.time_quality == LG_TIME_AUTHORITATIVE && now - last_announce >= TIME_ANNOUNCE_MS) {
            last_announce = now;
            lg_node_announce_time(&g_app.core, LG_TIME_AUTHORITATIVE);
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
        .on_grid_state = io_on_grid_state,
        .on_groups_changed = io_on_groups_changed,
    };
    lg_roster_init_prototype(&g_app.roster);
    if (settings_groups_load(&g_app.roster.groups) == ESP_OK) {
        ESP_LOGI(TAG, "[GRID] Groups version %" PRIu32 " (made on AP %u): %u group(s)", g_app.roster.groups.seq,
                 g_app.roster.groups.author, g_app.roster.groups.count);
    } else {
        ESP_LOGI(TAG, "[GRID] No groups saved; waiting for another AP or an edit");
    }
    lg_node_init(&g_app.core, g_app.index, g_app.boot, &g_app.roster, &io);

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
    console_start();
}
