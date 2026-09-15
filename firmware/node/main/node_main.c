/*
 * LocalGrid infrastructure node (prototype).
 *
 * SoftAP on a fixed channel for handhelds, TCP control sessions, ESP-NOW
 * backbone to other nodes, BLE discovery adverts, serial console.
 * Node identity comes from the MAC table in lg_proto_config.h.
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
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lg_crypto.h"
#include "lg_proto_config.h"
#include "lg_secrets.h"
#include "node_app.h"
#include "nvs_flash.h"
#include "sessions.h"
#include "web_admin.h"

static const char *TAG = "GRID";

node_app_t g_app;

static const uint8_t s_backbone_key[32] = LG_SECRET_BACKBONE_KEY;
static uint8_t s_discriminator[4];
static uint8_t s_last_disc_clients = 0xFF;
static uint8_t s_last_disc_flags = 0xFF;

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

static size_t build_discovery(uint8_t out[LG_DISC_LEN])
{
    uint8_t clients = sess_registered_count();
    out[0] = LG_DISC_MAGIC0;
    out[1] = LG_DISC_MAGIC1;
    out[2] = LG_DISC_VERSION;
    memcpy(out + 3, s_discriminator, 4);
    out[7] = (uint8_t)g_app.index;
    out[8] = g_app.index == 0 ? 1 : 0;
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

    static const uint8_t oui[3] = LG_VENDOR_OUI;
    uint8_t ie[sizeof(vendor_ie_data_t) + LG_DISC_LEN];
    vendor_ie_data_t *v = (vendor_ie_data_t *)ie;
    v->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    v->length = (uint8_t)(4 + LG_DISC_LEN);
    memcpy(v->vendor_oui, oui, 3);
    v->vendor_oui_type = LG_VENDOR_OUI_TYPE;
    memcpy(ie + sizeof(vendor_ie_data_t), payload, LG_DISC_LEN);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
    esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
    ble_adv_update(payload, LG_DISC_LEN);
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
        ESP_LOGI("NET", "[NET] Station %02x:%02x:%02x:%02x:%02x:%02x joined (aid %u)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI("NET", "[NET] Station %02x:%02x:%02x:%02x:%02x:%02x left (reason %u)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->reason);
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
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wc = { 0 };
    int ssid_len = snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s%s", LG_PROTO_SSID_PREFIX, g_app.name);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    strncpy((char *)wc.ap.password, LG_SECRET_WIFI_PASSPHRASE, sizeof(wc.ap.password) - 1);
    wc.ap.channel = LG_PROTO_CHANNEL;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = LG_PROTO_MAX_STATIONS;
    wc.ap.beacon_interval = 100;
    wc.ap.dtim_period = 1;
    wc.ap.pmf_cfg.capable = true;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_AP, 30));
    ESP_LOGI("NET", "[NET] SoftAP %s on channel %d at %u.%u.%u.%u", (char *)wc.ap.ssid, LG_PROTO_CHANNEL,
             ip[0], ip[1], ip[2], ip[3]);
}

/* ---- console command handling (core task) ---- */

static const char *quality_name(uint8_t q)
{
    return q == LG_TIME_AUTHORITATIVE ? "AUTHORITATIVE" : q == LG_TIME_CARRIED ? "CARRIED" : "UNSET";
}

static void print_status(void)
{
    const lg_node_stats_t *st = &g_app.core.stats;
    printf("Node %u %s (%s), boot %" PRIu32 ", uptime %" PRIu32 " s\n", g_app.index, g_app.name,
           g_app.index == 0 ? "MASTER" : "NODE", g_app.boot, app_now_ms() / 1000);
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
            refresh_discovery();
            if (g_app.index == 0) {
                web_admin_publish_snapshot();
            }
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
    };
    lg_node_init(&g_app.core, g_app.index, g_app.boot, lg_roster_prototype(), &io);

    ESP_ERROR_CHECK(lgbb_init(g_app.index, g_app.boot, s_backbone_key, on_backbone_frame, on_link, clients_count));
    ESP_ERROR_CHECK(sess_init(LG_PROTO_TCP_PORT));
    if (g_app.index == 0) {
        if (web_admin_start() != ESP_OK) {
            ESP_LOGE(TAG, "[WEB] Admin page failed to start");
        } else {
            web_admin_publish_snapshot();
        }
    }

    uint8_t payload[LG_DISC_LEN];
    build_discovery(payload);
    esp_log_level_set("BTDM_INIT", ESP_LOG_WARN);   /* controller init logs "Bluetooth MAC: ..." at INFO (D21) */
    if (ble_adv_init(payload, sizeof(payload)) != ESP_OK) {
        ESP_LOGE(TAG, "[BLE] Disabled: init failed");
    }

    xTaskCreate(core_task, "lg_core", 8192, NULL, 5, NULL);
    console_start();
}
