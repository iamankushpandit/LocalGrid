/* fw_spread.c - see fw_spread.h. Core task only; no heap, no task of its own. */
#include "fw_spread.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backbone.h"   /* the update frames go one ESP-NOW hop and never by LoRa */
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
#define NEIGHBOUR_FRESH_MS 180000u  /* D111: what a neighbour carries is believed this long after it last said so */
#define WITHDRAW_LEN      8u        /* D111: {LAYOUT, 'W', chip, 0, serial u32}: withdrawn up to this serial */

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

    /*
     * D111: what each neighbour carries for handhelds, from its offers. One AP holds one handheld image,
     * and there are three handheld chips, so the grid shares them out: a handheld whose chip another AP
     * carries is served through its own AP, which asks that neighbour for each piece.
     */
    struct {
        uint8_t  chip;               /* 0: nothing heard */
        uint32_t serial;
        uint32_t file_len;
        uint32_t heard_ms;
    } nb[LG_MAX_NODES];
    /* a handheld's piece being fetched from a neighbour on its behalf */
    uint32_t      proxy_dev;         /* 0 when none */
    uint16_t      proxy_node;
    uint32_t      proxy_serial;
    uint32_t      proxy_offset;
    uint32_t      proxy_ms;
    uint32_t      proxied_pieces;
} s = { .serve_node = LG_NODE_NONE };

static bool nb_fresh(uint16_t n)
{
    return n < LG_MAX_NODES && s.nb[n].chip != 0 && s.now_ms - s.nb[n].heard_ms < NEIGHBOUR_FRESH_MS &&
           lgbb_is_neighbor(n);
}

bool fw_spread_carried_elsewhere(uint8_t chip)
{
    for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
        if (nb_fresh(n) && s.nb[n].chip == chip) {
            return true;
        }
    }
    return false;
}

size_t fw_spread_neighbours(fw_spread_nb_t *out, size_t max)
{
    size_t k = 0;
    for (uint16_t n = 0; n < LG_MAX_NODES && k < max; n++) {
        if (nb_fresh(n)) {
            out[k++] = (fw_spread_nb_t){ .node = n, .chip = s.nb[n].chip, .serial = s.nb[n].serial };
        }
    }
    return k;
}

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
    bool mine = lg_fwu_carried(&c, &c_len);
    if (mine) {
        uint8_t body[OFFER_LEN] = { LAYOUT, 'H', c.chip, 0 };
        lg_wr32(body + 4, c.serial);
        lg_wr32(body + 8, (uint32_t)c_len);
        (void)lg_node_send_fw(&g_app.core, LG_NODE_NONE, LG_T_FW_OFFER, body, sizeof(body));
    }
    /*
     * D111: to this AP's handhelds, the newest update for each chip within reach - its own, and what its
     * neighbours carry, which it fetches piece by piece on a handheld's behalf.
     */
    for (uint8_t chip = 1; chip <= 3u; chip++) {
        uint32_t serial = mine && c.chip == chip ? c.serial : 0u;
        uint32_t flen = mine && c.chip == chip ? (uint32_t)c_len : 0u;
        for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
            if (nb_fresh(n) && s.nb[n].chip == chip && s.nb[n].serial > serial) {
                serial = s.nb[n].serial;
                flen = s.nb[n].file_len;
            }
        }
        if (serial == 0 || serial <= lg_fwu_carry_floor(chip)) {
            continue;
        }
        uint8_t body[OFFER_LEN] = { LAYOUT, 'H', chip, 0 };
        lg_wr32(body + 4, serial);
        lg_wr32(body + 8, flen);
        for (uint32_t device = 1; device <= LG_MAX_DEVICES; device++) {
            (void)lg_node_send_fw_client(&g_app.core, device, LG_T_FW_OFFER, body, sizeof(body));
        }
    }
    s.offered_ms = s.now_ms;
    s.offer_due = false;
}

/*
 * D111: a withdrawal, passed to every neighbour. Each AP passes one on only when it changed something
 * there, so it crosses the grid once and stops.
 */
static void send_withdraw(uint8_t chip, uint32_t serial, uint16_t except)
{
    uint8_t body[WITHDRAW_LEN] = { LAYOUT, 'W', chip, 0 };
    lg_wr32(body + 4, serial);
    for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
        if (n != except && lgbb_is_neighbor(n)) {
            (void)lg_node_send_fw(&g_app.core, n, LG_T_FW_OFFER, body, sizeof(body));
        }
    }
}

uint32_t fw_spread_withdraw(void)
{
    lg_fwu_info_t c;
    if (!lg_fwu_carried(&c, NULL)) {
        return 0;
    }
    uint32_t dropped = lg_fwu_carried_drop();
    send_withdraw(c.chip, c.serial, LG_NODE_NONE);
    for (uint16_t n = 0; n < LG_MAX_NODES; n++) {   /* and stop offering what the neighbours had of it */
        if (s.nb[n].chip == c.chip && s.nb[n].serial <= c.serial) {
            s.nb[n].chip = 0;
        }
    }
    return dropped;
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
    if (len == WITHDRAW_LEN && b[0] == LAYOUT && b[1] == 'W' && b[2] >= 1u && b[2] <= 3u) {
        uint32_t upto = lg_rd32(b + 4);
        if (lg_fwu_carry_withdraw(b[2], upto)) {
            send_withdraw(b[2], upto, from);   /* changed something here: pass it on */
        }
        for (uint16_t n = 0; n < LG_MAX_NODES; n++) {
            if (s.nb[n].chip == b[2] && s.nb[n].serial <= upto) {
                s.nb[n].chip = 0;
            }
        }
        return;
    }
    if (len != OFFER_LEN || b[0] != LAYOUT || (b[1] != 'N' && b[1] != 'H') ||
        (b[1] == 'N' && b[2] != lg_fwu_this_chip())) {
        return;   /* an AP's firmware for another chip: not ours to take */
    }
    char kind = (char)b[1];
    uint32_t serial = lg_rd32(b + 4);
    uint32_t file_len = lg_rd32(b + 8);
    if (kind == 'H' && from < LG_MAX_NODES && b[2] >= 1u && b[2] <= 3u) {
        s.nb[from].chip = b[2];          /* D111: what that neighbour carries */
        s.nb[from].serial = serial;
        s.nb[from].file_len = file_len;
        s.nb[from].heard_ms = s.now_ms;
    }
    lg_fwu_info_t c;
    bool carrying = kind == 'H' && lg_fwu_carried(&c, NULL);
    uint32_t have = kind == 'N' ? lg_fwu_running_serial() : carrying ? c.serial : 0u;
    if (kind == 'H') {
        /*
         * D111: one image per AP. Take a newer one for the chip already carried here; take any if nothing
         * is carried here; never let one chip push out another - that is how a chip would vanish from the
         * grid. An admin who wants this AP to carry another chip gives it the file.
         */
        if (carrying && c.chip != b[2]) {
            return;
        }
        if (lg_fwu_carry_floor(b[2]) > have) {
            have = lg_fwu_carry_floor(b[2]);   /* one withdrawn here is not fetched again */
        }
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
    if (s.proxy_dev != 0 && from == s.proxy_node && len > DATA_HEADER && b[0] == LAYOUT &&
        lg_rd32(b + 4) == s.proxy_serial && lg_rd32(b + 8) == s.proxy_offset) {
        /* D111: the piece asked for on a handheld's behalf: the same bytes go on to it. */
        (void)lg_node_send_fw_client(&g_app.core, s.proxy_dev, LG_T_FW_DATA, b, len);
        s.proxied_pieces++;
        s.proxy_dev = 0;
        return;
    }
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
        /*
         * D111: not carried here. If a neighbour carries this chip at this serial, ask it for the very
         * piece; its answer comes to on_data and is passed straight on. The handheld repeats a request
         * that goes unanswered, so one lost on either hop costs a second and a half, not the update.
         */
        uint32_t want_serial = lg_rd32(b + 4);
        for (uint16_t nb = 0; nb < LG_MAX_NODES; nb++) {
            if (nb_fresh(nb) && s.nb[nb].chip == b[2] && s.nb[nb].serial == want_serial &&
                want_serial > lg_fwu_carry_floor(b[2])) {
                s.proxy_dev = device;
                s.proxy_node = nb;
                s.proxy_serial = want_serial;
                s.proxy_offset = lg_rd32(b + 8);
                s.proxy_ms = s.now_ms;
                if (s.serve_dev != device) {
                    ESP_LOGI(TAG, "[GRID] Fetching the handheld update, serial %" PRIu32 ", from AP %u for device %" PRIu32,
                             want_serial, nb, device);
                }
                s.serve_dev = device;
                s.serve_dev_ms = s.now_ms;
                (void)lg_node_send_fw(&g_app.core, nb, LG_T_FW_GET, b, len);
                return;
            }
        }
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
    } else {
        printf("Handheld update carried: none\n");
    }
    for (uint8_t chip = 1; chip <= 3u; chip++) {
        if (lg_fwu_carry_floor(chip) != 0) {
            printf("  withdrawn here for chip %u: serial %" PRIu32 " and older\n", chip, lg_fwu_carry_floor(chip));
        }
    }
    for (uint16_t n = 0; n < LG_MAX_NODES; n++) {   /* D111 */
        if (nb_fresh(n)) {
            printf("  AP %u carries chip %u, serial %" PRIu32 "\n", n, s.nb[n].chip, s.nb[n].serial);
        }
    }
    if (s.proxied_pieces != 0) {
        printf("  pieces fetched from a neighbour for a handheld here: %" PRIu32 "\n", s.proxied_pieces);
    }
    if (s.fetching) {
        printf("Fetching %s serial %" PRIu32 " from AP %u: %u of %u bytes\n", s.kind == 'H' ? "handheld update" : "firmware",
               s.serial, s.from, (unsigned)s.have, (unsigned)s.file_len);
    } else if (s.restart_at_ms != 0) {
        printf("An update is in its slot; restarting into it\n");
    }
}
