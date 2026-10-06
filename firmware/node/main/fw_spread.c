/* fw_spread.c - see fw_spread.h. Core task only; no heap, no task of its own. */
#include "fw_spread.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lg_envelope.h"
#include "lg_fwupdate.h"
#include "lg_node.h"
#include "node_app.h"

static const char *TAG = "GRID";

#define LAYOUT            1u
#define OFFER_LEN         16u
#define GET_LEN           16u
#define DATA_HEADER       12u
#define OFFER_EVERY_MS    60000u    /* a holder says so this often, and at once when a link comes up */
#define SETTLE_MS         120000u   /* an AP this young neither fetches nor serves: it may be about to roll back */
#define REQUEST_RETRY_MS  1500u     /* no answer to a request: ask again */
#define REQUEST_RETRIES   20u       /* ... this many times, then give the update up for now */
#define SERVE_HOLD_MS     5000u     /* a holder belongs to the neighbour it is serving until that one is this quiet */
#define REFUSED_WAIT_MS   600000u   /* an update lg_fwupdate refused is not asked for again for this long */
#define FAILED_WAIT_MS    120000u   /* one that stopped arriving is tried again after this */
#define RESTART_AFTER_MS  3000u     /* between "it is in the slot" and the restart, so the log line gets out */

_Static_assert(DATA_HEADER + FW_SPREAD_CHUNK <= LG_FW_BODY_MAX, "a chunk fits the frame the core carries");

static struct {
    /* what this AP can pass on: read once, when first wanted */
    bool          held_known;
    bool          held;
    lg_fwu_info_t held_info;
    size_t        held_len;          /* of the file: 192 + image */
    bool          offer_due;
    uint32_t      offered_ms;
    uint16_t      serve_node;        /* the neighbour being served, LG_NODE_NONE when free */
    uint32_t      serve_ms;
    uint32_t      served_chunks;
    uint32_t      serve_dev;         /* D107: the handheld being served, 0 when free */
    uint32_t      serve_dev_ms;
    uint32_t      handhelds_served;  /* handhelds that asked for the last piece of a carried update */

    /* what this AP is fetching */
    bool          fetching;
    char          kind;              /* 'N': this AP's own update. 'H': a handheld's, to carry (D107) */
    uint8_t       chip;              /* the chip the offered file is for */
    char          wait_kind;
    uint16_t      from;
    uint32_t      serial;
    size_t        file_len;
    size_t        have;
    uint32_t      asked_ms;
    uint8_t       retries;
    uint32_t      started_ms;
    uint32_t      wait_serial;       /* an update not to ask for again until wait_until_ms */
    uint32_t      wait_until_ms;
    uint32_t      restart_at_ms;     /* nonzero: the update is in its slot, restart then */
    uint32_t      now_ms;
} s = { .serve_node = LG_NODE_NONE };

static void held_load(void)
{
    if (!s.held_known) {
        s.held_known = true;
        s.held = lg_fwu_held(&s.held_info, &s.held_len) && s.held_info.role == 'N';
        if (s.held) {
            ESP_LOGI(TAG, "[GRID] Firmware update %s (serial %" PRIu32 ") is this AP's own and can be passed on",
                     s.held_info.version, s.held_info.serial);
        }
    }
}

static void offer(void)
{
    if (s.held) {
        uint8_t body[OFFER_LEN] = { LAYOUT, 'N', lg_fwu_this_chip(), 0 };
        lg_wr32(body + 4, s.held_info.serial);
        lg_wr32(body + 8, (uint32_t)s.held_len);
        (void)lg_node_send_fw(&g_app.core, LG_NODE_NONE, LG_T_FW_OFFER, body, sizeof(body));
    }
    /*
     * D107: the handheld update this AP carries, to its neighbours so that every AP comes to carry it,
     * and to each handheld with a session here. A handheld on another chip, or already as new, simply
     * does not ask. lg_node_send_fw_client refuses a device with no session here, so the loop costs
     * nothing for the others.
     */
    lg_fwu_info_t c;
    size_t c_len = 0;
    if (lg_fwu_carried(&c, &c_len)) {
        uint8_t body[OFFER_LEN] = { LAYOUT, 'H', c.chip, 0 };
        lg_wr32(body + 4, c.serial);
        lg_wr32(body + 8, (uint32_t)c_len);
        (void)lg_node_send_fw(&g_app.core, LG_NODE_NONE, LG_T_FW_OFFER, body, sizeof(body));
        for (uint32_t device = 1; device <= LG_MAX_DEVICES; device++) {
            (void)lg_node_send_fw_client(&g_app.core, device, LG_T_FW_OFFER, body, sizeof(body));
        }
    }
    s.offered_ms = s.now_ms;
    s.offer_due = false;
}

static void request(void)
{
    /* The signed 192 bytes first and alone, so the update is judged before any of the image is asked for. */
    size_t want = s.have < LG_FWU_PREFIX_LEN ? LG_FWU_PREFIX_LEN - s.have : s.file_len - s.have;
    if (want > FW_SPREAD_CHUNK) {
        want = FW_SPREAD_CHUNK;
    }
    uint8_t body[GET_LEN] = { LAYOUT, (uint8_t)s.kind, s.chip, 0 };
    lg_wr32(body + 4, s.serial);
    lg_wr32(body + 8, (uint32_t)s.have);
    lg_wr16(body + 12, (uint16_t)want);
    (void)lg_node_send_fw(&g_app.core, s.from, LG_T_FW_GET, body, sizeof(body));
    s.asked_ms = s.now_ms;
}

static void give_up(const char *why, uint32_t wait_ms)
{
    ESP_LOGW(TAG, "[GRID] Firmware update from AP %u given up at %u of %u bytes: %s", s.from, (unsigned)s.have,
             (unsigned)s.file_len, why);
    lg_fwu_abort();
    s.fetching = false;
    s.wait_serial = s.serial;
    s.wait_kind = s.kind;
    s.wait_until_ms = s.now_ms + wait_ms;
}

static void on_offer(uint16_t from, const uint8_t *b, size_t len)
{
    if (len != OFFER_LEN || b[0] != LAYOUT || (b[1] != 'N' && b[1] != 'H') ||
        (b[1] == 'N' && b[2] != lg_fwu_this_chip())) {
        return;   /* an AP's firmware for another chip: not ours to take */
    }
    char kind = (char)b[1];
    uint32_t serial = lg_rd32(b + 4);
    uint32_t file_len = lg_rd32(b + 8);
    lg_fwu_info_t c;
    uint32_t have = kind == 'N' ? lg_fwu_running_serial() : lg_fwu_carried(&c, NULL) ? c.serial : 0u;
    if (kind == 'H' && lg_fwu_carry_floor() > have) {
        have = lg_fwu_carry_floor();   /* one withdrawn here is not fetched again */
    }
    if (s.fetching || s.restart_at_ms != 0 || s.now_ms < SETTLE_MS || serial <= have ||
        file_len <= LG_FWU_PREFIX_LEN || lg_fwu_progress(NULL, NULL)) {
        return;   /* busy, too young, already as new, or an upload is in hand by another road */
    }
    if (serial == s.wait_serial && kind == s.wait_kind && (int32_t)(s.now_ms - s.wait_until_ms) < 0) {
        return;
    }
    /* 'N' either way: what the file is, is for its signed header to say, and lg_fwupdate carries a
     * handheld's file instead of installing it (D107). */
    if (lg_fwu_begin('N') != ESP_OK) {
        return;
    }
    s.fetching = true;
    s.kind = kind;
    s.chip = b[2];
    s.from = from;
    s.serial = serial;
    s.file_len = file_len;
    s.have = 0;
    s.retries = 0;
    s.started_ms = s.now_ms;
    if (kind == 'N') {
        ESP_LOGW(TAG, "[GRID] AP %u offers firmware serial %" PRIu32 ", newer than this AP's %" PRIu32 ": asking for it",
                 from, serial, have);
    } else {
        ESP_LOGW(TAG, "[GRID] AP %u offers a handheld update, serial %" PRIu32 " (this AP carries %" PRIu32
                      "): asking for it, to carry", from, serial, have);
    }
    request();
}

/* One piece of a file this AP can hand on: its own update ('N') or the handheld one it carries ('H'). */
static int piece(const uint8_t *b, size_t len, uint8_t *out, uint32_t *serial, uint32_t *offset)
{
    if (len != GET_LEN || b[0] != LAYOUT || s.now_ms < SETTLE_MS) {
        return -1;
    }
    *serial = lg_rd32(b + 4);
    *offset = lg_rd32(b + 8);
    size_t want = lg_rd16(b + 12);
    if (want == 0 || want > FW_SPREAD_CHUNK) {
        return -1;
    }
    if (b[1] == 'N') {
        held_load();
        if (!s.held || b[2] != lg_fwu_this_chip() || *serial != s.held_info.serial || *offset >= s.held_len) {
            return -1;
        }
        return lg_fwu_held_read(*offset, out, want);
    }
    lg_fwu_info_t c;
    size_t c_len = 0;
    if (b[1] != 'H' || !lg_fwu_carried(&c, &c_len) || b[2] != c.chip || *serial != c.serial || *offset >= c_len) {
        return -1;
    }
    return lg_fwu_carried_read(*offset, out, want);
}

static void on_get(uint16_t from, const uint8_t *b, size_t len)
{
    if (s.serve_node != LG_NODE_NONE && s.serve_node != from && s.now_ms - s.serve_ms < SERVE_HOLD_MS) {
        return;   /* one neighbour at a time: the other's requests go unanswered and it tries again later */
    }
    static uint8_t body[DATA_HEADER + FW_SPREAD_CHUNK];   /* core task only; too big for its stack beside a frame */
    uint32_t serial = 0;
    uint32_t offset = 0;
    int n = piece(b, len, body + DATA_HEADER, &serial, &offset);
    if (n <= 0) {
        return;
    }
    memset(body, 0, DATA_HEADER);
    body[0] = LAYOUT;
    lg_wr32(body + 4, serial);
    lg_wr32(body + 8, offset);
    if (s.serve_node != from) {
        ESP_LOGI(TAG, "[GRID] Passing %s serial %" PRIu32 " to AP %u", b[1] == 'H' ? "the handheld update" : "firmware",
                 serial, from);
        s.served_chunks = 0;
    }
    s.serve_node = from;
    s.serve_ms = s.now_ms;
    s.served_chunks++;
    (void)lg_node_send_fw(&g_app.core, from, LG_T_FW_DATA, body, DATA_HEADER + (size_t)n);
}

static void on_data(uint16_t from, const uint8_t *b, size_t len)
{
    if (!s.fetching || from != s.from || len <= DATA_HEADER || b[0] != LAYOUT || lg_rd32(b + 4) != s.serial ||
        lg_rd32(b + 8) != s.have) {
        return;   /* not the piece asked for: a late answer to a request already repeated */
    }
    size_t n = len - DATA_HEADER;
    if (s.have + n > s.file_len) {
        give_up("more arrived than was offered", REFUSED_WAIT_MS);
        return;
    }
    size_t taken = 0;
    if (!lg_fwu_progress(&taken, NULL) || taken != s.have) {
        s.fetching = false;   /* the console or the admin page took the update slot for a file of its own */
        return;
    }
    if (lg_fwu_write(b + DATA_HEADER, n) != ESP_OK) {
        const char *why = lg_fwu_reason();
        s.fetching = false;   /* lg_fwupdate has already let go of it */
        s.wait_serial = s.serial;
        s.wait_kind = s.kind;
        s.wait_until_ms = s.now_ms + REFUSED_WAIT_MS;
        ESP_LOGW(TAG, "[GRID] Firmware update from AP %u not taken: %s", from, why);
        return;
    }
    s.have += n;
    s.retries = 0;
    if (s.have < s.file_len) {
        request();
        return;
    }
    s.fetching = false;
    if (lg_fwu_finish() != ESP_OK) {
        s.wait_serial = s.serial;
        s.wait_kind = s.kind;
        s.wait_until_ms = s.now_ms + REFUSED_WAIT_MS;
        ESP_LOGW(TAG, "[GRID] Firmware update from AP %u not installed: %s", from, lg_fwu_reason());
        return;
    }
    if (lg_fwu_finished_carried()) {
        /* D107: a handheld's update. Nothing restarts; it is offered on at once. */
        ESP_LOGW(TAG, "[GRID] Handheld update serial %" PRIu32 " taken from AP %u in %" PRIu32 " s; carrying it", s.serial,
                 from, (s.now_ms - s.started_ms) / 1000u);
        s.offer_due = true;
        return;
    }
    ESP_LOGW(TAG, "[GRID] Firmware serial %" PRIu32 " taken from AP %u in %" PRIu32 " s; restarting into it", s.serial, from,
             (s.now_ms - s.started_ms) / 1000u);
    s.restart_at_ms = (s.now_ms + RESTART_AFTER_MS) | 1u;
}

void fw_spread_on_frame(uint16_t from_node, uint8_t type, const uint8_t *body, size_t len)
{
    if (body == NULL) {
        return;
    }
    switch (type) {
    case LG_T_FW_OFFER: on_offer(from_node, body, len); break;
    case LG_T_FW_GET:   on_get(from_node, body, len); break;
    case LG_T_FW_DATA:  on_data(from_node, body, len); break;
    default:            break;
    }
}

/*
 * D107: a handheld with a session here asks for a piece of the update this AP carries. One handheld at
 * a time, so two of them never restart together and the AP is never reading flash for a crowd; another
 * that asks meanwhile is not answered, and asks again when it is next offered the update.
 */
void fw_spread_on_client_get(uint32_t device, const uint8_t *b, size_t len)
{
    if (b == NULL || len != GET_LEN || b[1] != 'H') {
        return;
    }
    if (s.serve_dev != 0 && s.serve_dev != device && s.now_ms - s.serve_dev_ms < SERVE_HOLD_MS) {
        return;
    }
    static uint8_t body[DATA_HEADER + FW_SPREAD_CHUNK];   /* core task only */
    uint32_t serial = 0;
    uint32_t offset = 0;
    int n = piece(b, len, body + DATA_HEADER, &serial, &offset);
    if (n <= 0) {
        return;
    }
    memset(body, 0, DATA_HEADER);
    body[0] = LAYOUT;
    lg_wr32(body + 4, serial);
    lg_wr32(body + 8, offset);
    if (s.serve_dev != device) {
        ESP_LOGI(TAG, "[GRID] Handing the handheld update, serial %" PRIu32 ", to device %" PRIu32, serial, device);
    }
    lg_fwu_info_t c;
    size_t c_len = 0;
    if (lg_fwu_carried(&c, &c_len) && offset + (uint32_t)n >= c_len) {
        s.handhelds_served++;
    }
    s.serve_dev = device;
    s.serve_dev_ms = s.now_ms;
    (void)lg_node_send_fw_client(&g_app.core, device, LG_T_FW_DATA, body, DATA_HEADER + (size_t)n);
}

void fw_spread_on_neighbor_up(uint16_t node)
{
    (void)node;
    s.offer_due = true;
}

void fw_spread_tick(uint32_t now_ms)
{
    s.now_ms = now_ms;
    if (s.restart_at_ms != 0) {
        if ((int32_t)(now_ms - s.restart_at_ms) >= 0) {
            esp_restart();
        }
        return;
    }
    if (s.fetching && now_ms - s.asked_ms >= REQUEST_RETRY_MS) {
        if (!lg_fwu_progress(NULL, NULL)) {
            s.fetching = false;   /* taken away by an upload */
        } else if (++s.retries > REQUEST_RETRIES) {
            give_up("the AP that offered it stopped answering", FAILED_WAIT_MS);
        } else {
            request();
        }
    }
    if (now_ms < SETTLE_MS) {
        return;
    }
    held_load();
    if ((s.held || lg_fwu_carried(NULL, NULL)) && (s.offer_due || now_ms - s.offered_ms >= OFFER_EVERY_MS)) {
        offer();
    }
}

void fw_spread_print(void)
{
    held_load();
    if (s.held) {
        printf("Firmware to pass on: %s, serial %" PRIu32 ", %u bytes; %s\n", s.held_info.version, s.held_info.serial,
               (unsigned)s.held_len,
               s.serve_node != LG_NODE_NONE && s.now_ms - s.serve_ms < SERVE_HOLD_MS ? "passing it to a neighbour now"
                                                                                       : "offered to neighbours every minute");
    } else {
        printf("Firmware to pass on: none (this AP's firmware came by cable, or it holds no signed copy)\n");
    }
    lg_fwu_info_t c;
    size_t c_len = 0;
    if (lg_fwu_carried(&c, &c_len)) {
        printf("Handheld update carried: %s, serial %" PRIu32 ", chip %u, %u bytes; handed whole to %" PRIu32
               " handheld(s) since this AP started\n", c.version, c.serial, c.chip, (unsigned)c_len, s.handhelds_served);
    } else if (lg_fwu_carry_floor() != 0) {
        printf("Handheld update carried: none (serial %" PRIu32 " and older were withdrawn here)\n", lg_fwu_carry_floor());
    } else {
        printf("Handheld update carried: none\n");
    }
    if (s.fetching) {
        printf("Fetching %s serial %" PRIu32 " from AP %u: %u of %u bytes\n", s.kind == 'H' ? "handheld update" : "firmware",
               s.serial, s.from, (unsigned)s.have, (unsigned)s.file_len);
    } else if (s.restart_at_ms != 0) {
        printf("An update is in its slot; restarting into it\n");
    }
}
