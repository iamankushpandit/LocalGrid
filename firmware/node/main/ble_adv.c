#include "ble_adv.h"

#include "ble_link.h"
#include "power_trace.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "lg_proto_config.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "BLE";

#define ADV_INTERVAL_UNITS  800u   /* 500 ms in 0.625 ms units */

static uint8_t s_mfg[2 + 26];
static size_t  s_mfg_len;
static uint8_t s_own_addr_type;
static bool    s_synced;
static bool    s_connectable;   /* D70: the advert accepts an admin-link connection */
static bool    s_link_ready;    /* the GATT service registered */
static struct ble_npl_event s_mode_ev;
static bool    s_host_ready;   /* nimble_port_init succeeded: the default event queue exists */

/*
 * D68: the scan response carries the sealed status frame (ble_status.c) as manufacturer data under
 * the same company ID. The core task seals a frame every 500 ms and hands it over with
 * ble_adv_set_status, which only copies it under a spinlock and posts an event; the NimBLE host
 * task applies it, so the core task never waits on an HCI command. The advert itself is already
 * scannable (ADV_SCAN_IND: non-connectable, general discoverable), so it is left as it was.
 */
#define RSP_FRAME_MAX 27u   /* 2 + 27 bytes of manufacturer data fill the 31-byte scan response */
static portMUX_TYPE          s_rsp_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t               s_rsp_pending[RSP_FRAME_MAX];   /* under s_rsp_mux */
static size_t                s_rsp_pending_len;
static uint8_t               s_rsp[2 + RSP_FRAME_MAX];       /* host task only */
static size_t                s_rsp_len;
static struct ble_npl_event  s_rsp_ev;

static void set_payload(const uint8_t *payload, size_t len)
{
    if (len > sizeof(s_mfg) - 2) {
        len = sizeof(s_mfg) - 2;
    }
    s_mfg[0] = (uint8_t)(LG_BLE_COMPANY_ID & 0xFF);
    s_mfg[1] = (uint8_t)(LG_BLE_COMPANY_ID >> 8);
    memcpy(s_mfg + 2, payload, len);
    s_mfg_len = 2 + len;
}

static int gap_event(struct ble_gap_event *event, void *arg);

static int set_fields(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = s_mfg;
    fields.mfg_data_len = (uint8_t)s_mfg_len;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] set adv fields rc=%d", rc);
    }
    return rc;
}

/* Host task: the scan response as it stands, or an empty one. */
static int set_rsp_fields(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (s_rsp_len > 0) {
        fields.mfg_data = s_rsp;
        fields.mfg_data_len = (uint8_t)s_rsp_len;
    }
    int rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] set scan response rc=%d", rc);
    }
    return rc;
}

/* Host task: takes the frame the core task left and puts it in the scan response. */
static void on_rsp_event(struct ble_npl_event *ev)
{
    (void)ev;
    uint8_t frame[RSP_FRAME_MAX];
    taskENTER_CRITICAL(&s_rsp_mux);
    size_t len = s_rsp_pending_len;
    memcpy(frame, s_rsp_pending, len);
    taskEXIT_CRITICAL(&s_rsp_mux);
    s_rsp[0] = (uint8_t)(LG_BLE_COMPANY_ID & 0xFF);
    s_rsp[1] = (uint8_t)(LG_BLE_COMPANY_ID >> 8);
    memcpy(s_rsp + 2, frame, len);
    s_rsp_len = len > 0 ? 2 + len : 0;
    if (s_synced) {
        (void)set_rsp_fields();   /* LE Set Scan Response Data is allowed while advertising */
    }
}

/*
 * D70: the advert becomes connectable so a watcher can open the admin link, unless one is already
 * connected or this AP is short of heap. The payload and the scan response are byte for byte the
 * ones D68 sends either way; only the advertising PDU changes (ADV_IND while connectable,
 * ADV_SCAN_IND otherwise), and both are scannable, so the status beacon never stops.
 */
/*
 * Measured 2026-09-20: this was 24576, the same floor ble_link.c used, and an AP carrying a LoRa
 * module runs at 17-25 KB free. So the AP either never offered the link at all, or sat right on
 * the threshold and tore its advert down and rebuilt it every second as the heap crossed back and
 * forth - a watcher's connection attempt could never complete, and the map, positions and traffic
 * never worked. Two changes: the floor is the link's own (one number, not two), and it has
 * hysteresis, so a heap wobbling around the line cannot flap the advert.
 */
#define ADV_MIN_HEAP  LINK_MIN_HEAP            /* rise above this to start offering the link */
#define ADV_KEEP_HEAP (LINK_MIN_HEAP - 1024u)  /* fall below this to stop offering it */

static bool connectable_now(void)
{
    if (!s_link_ready || ble_link_busy()) {
        return false;
    }
    uint32_t heap = esp_get_free_heap_size();
    return s_connectable ? heap >= ADV_KEEP_HEAP : heap >= ADV_MIN_HEAP;
}

static void start_advertising(void)
{
    if (set_fields() != 0) {
        return;
    }
    (void)set_rsp_fields();   /* after a host reset the controller has forgotten it */
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    s_connectable = connectable_now();
    params.conn_mode = s_connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = ADV_INTERVAL_UNITS;
    params.itvl_max = ADV_INTERVAL_UNITS;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] adv start rc=%d", rc);
    }
}

/* Host task: puts advertising back in the mode connectable_now() asks for. */
static void restart_advertising(void)
{
    (void)ble_gap_adv_stop();
    start_advertising();
}

static void on_adv_mode_event(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_synced && s_connectable != connectable_now()) {
        restart_advertising();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_link_on_connect(event->connect.conn_handle);
        }
        restart_advertising();   /* non-connectable now: the beacon goes on, nobody else may join */
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_link_on_disconnect(event->disconnect.conn.conn_handle);
        restart_advertising();
        break;
    case BLE_GAP_EVENT_MTU:
        ble_link_on_mtu(event->mtu.conn_handle, event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ble_link_on_subscribe(event->subscribe.conn_handle, event->subscribe.attr_handle,
                              event->subscribe.cur_notify != 0);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        ble_link_on_notify_tx();   /* one reply chunk has left the controller */
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] address setup rc=%d", rc);
        return;
    }
    s_synced = true;
    start_advertising();
    ESP_LOGI(TAG, "[BLE] Advertising LocalGrid discovery every 500 ms");
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "[BLE] Host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_adv_init(const uint8_t *payload, size_t len)
{
    set_payload(payload, len);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[BLE] nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_npl_event_init(&s_rsp_ev, on_rsp_event, NULL);
    ble_npl_event_init(&s_mode_ev, on_adv_mode_event, NULL);
    s_link_ready = ble_link_gatt_register() == ESP_OK;   /* D70, before the host starts */
    s_host_ready = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void ble_adv_set_status(const uint8_t *frame, size_t len)
{
    if (!s_host_ready) {
        return;
    }
    if (frame == NULL || len > RSP_FRAME_MAX) {
        len = 0;
    }
    taskENTER_CRITICAL(&s_rsp_mux);
    if (len > 0) {
        memcpy(s_rsp_pending, frame, len);
    }
    s_rsp_pending_len = len;
    taskEXIT_CRITICAL(&s_rsp_mux);
    /* Posting an event that is still queued does nothing: the host task takes the newest frame. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_rsp_ev);
}

void ble_adv_review_connectable(void)
{
    if (s_host_ready) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_mode_ev);
    }
}

void ble_adv_update(const uint8_t *payload, size_t len)
{
    size_t n = len > sizeof(s_mfg) - 2 ? sizeof(s_mfg) - 2 : len;
    if (s_mfg_len == 2 + n && memcmp(s_mfg + 2, payload, n) == 0) {
        return;   /* unchanged: leave the running advertisement alone */
    }
    set_payload(payload, len);
    ptrace_event(PTRACE_BLE_UPDATE);
    if (!s_synced) {
        return;   /* on_sync starts advertising with the new payload */
    }
    /* LE Set Advertising Data is allowed while advertising, so the radio schedule is not
     * interrupted. Stop and start only if the controller refuses the update. */
    if (set_fields() != 0) {
        (void)ble_gap_adv_stop();
        start_advertising();
    }
}
