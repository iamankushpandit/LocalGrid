/*
 * lora.h - the second backbone between APs, over a Reyax RYLR998 on UART1 (D71).
 *
 * The module speaks AT commands over a serial port. lora.c owns that port from its own task,
 * detects the module at boot, configures it, keeps it alive, and turns sealed backbone frames
 * into base64 parts and back. lora_wire.c holds everything that needs no radio.
 *
 * An AP with no module answers nothing at boot, is logged once, and is then indistinguishable
 * from an AP built before this file existed: no frame is ever offered to a radio that is not
 * there, and nothing else in the firmware changes its behaviour.
 *
 * Threading. The core task seals frames (so the one frame-sequence counter stays single-threaded)
 * and calls lora_poll() to take received frames back. Everything slow - a transmission is seconds
 * of airtime - happens on the "lora" task, which shares only a bounded queue and a mutex with it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backbone.h"
#include "esp_err.h"

/* ---- wiring (docs/lora.md, "Wiring (as built)") ----------------------------------------------
 *
 * The pins are NOT the same on every AP: MAIN was wired around its GPS, NORTH and SOUTH were
 * wired first and keep theirs. This table is the only place any LoRa pin number appears; the
 * firmware picks the row by AP index at start-up, so one build runs on every board.
 *
 *   AP index 0 (MAIN, also has a GPS on UART2 GPIO 16): module TXD -> 32, RXD -> 33, RST -> 25
 *   every other AP (NORTH, SOUTH):                      module TXD ->  4, RXD ->  5, RST -> 13
 *
 * "TXD ->" is the pin the ESP32 receives on; the pair is crossed over. RST may be left
 * unconnected: the firmware then falls back to the module's own AT+RESET.
 */
typedef struct {
    int8_t rx;    /* ESP32 receives here (module TXD) */
    int8_t tx;    /* ESP32 transmits here (module RXD) */
    int8_t rst;   /* -1 if this board leaves reset unconnected */
} lora_pins_t;

#define LORA_PINS_MAIN   { .rx = 32, .tx = 33, .rst = 25 }
#define LORA_PINS_OTHER  { .rx =  4, .tx =  5, .rst = 13 }

/* ---- radio settings ---- */

/*
 * Band. 868.5 MHz is the European licence-free LoRa band; a 915 MHz region must change this one
 * number (and only this one) before flashing.
 */
#define LORA_BAND_HZ        868500000u
#define LORA_UART_BAUD      115200        /* the RYLR998 leaves the factory at this rate */
/* AT+PARAMETER: spreading factor 9, bandwidth 125 kHz (code 7), coding rate 4/5 (code 1),
 * preamble 12. docs/lora.md names SF9/BW125; preamble 12 is the module's own default and is
 * required when the network ID is left at 18. */
#define LORA_SF             9
#define LORA_BW_CODE        7
#define LORA_CR_CODE        1
#define LORA_PREAMBLE       12
#define LORA_POWER_DBM      22            /* AT+CRFOP, 0..22 */

/*
 * The address plan (docs/lora.md, "Ready for handhelds later"): 0-15 are APs, by AP index, and
 * 100 + device number are handhelds, which nothing sends to yet. The RYLR998's AT+SEND to address
 * 0 reaches every address on the network ID, so it is the broadcast address and nothing else:
 * an AP is 1 + its index. Measured on the bench, 2026-09-20: with MAIN sitting on address 0 as
 * well, NORTH and SOUTH linked to each other but MAIN's link kept timing out, because a module
 * cannot be both a broadcast target and a private one. Keeping 0 free fixed it. `lora broadcast
 * off` still falls back to one transmission per peer.
 */
/* Long payloads (D72 voice notes) are off until a board has the heap for them. Measured on the
 * bench 2026-09-20: with the 6 KB receive and send slots taken, an AP that had 22-28 KB free ran
 * at 9-12 KB with a 1 KB low-water mark once handhelds joined, and MAIN reached 0. Nothing sends a
 * long payload yet, so the slots are not taken; a long payload is refused at its first part,
 * logged and counted. The wire format still carries 48 parts, so turning this on later changes
 * nothing on the air. */
#define LORA_LONG_PAYLOAD   0

#define LORA_ADDR_BROADCAST 0u
#define LORA_ADDR_AP(i)     ((uint16_t)((i) + 1u))
#define LORA_ADDR_HANDHELD(d) ((uint16_t)(100u + (d)))

/* ---- timing ---- */

/*
 * The keepalive that proves the radio still works and measures the link, and on a quiet grid it is
 * most of what LoRa carries: on the bench 2026-09-20, about 18 of MAIN's 35 frames in nine minutes
 * were heartbeats. One a minute still finds a dead link inside three minutes, which is far quicker
 * than anyone would notice, and halves what the radio says when nobody needs it.
 */
#define LORA_HEARTBEAT_MS     60000u
#define LORA_HB_STAGGER_MS    3000u    /* times the AP index, so three APs never speak together */
#define LORA_LINK_TIMEOUT_MS  190000u  /* three missed heartbeats and a little: the link is down */
#define LORA_PROBE_MS         60000u   /* how often an AP with no module looks for one again */

/*
 * Starts the reader task. on_frame and on_link are the same two the ESP-NOW backbone calls, so a
 * frame that crossed on LoRa takes exactly the path a frame that crossed on Wi-Fi takes, and a
 * peer whose only link is LoRa brings the same catch-up announcements with it (D48, D53).
 * Returns ESP_OK once the task is running, whether or not a module answered.
 */
esp_err_t lora_start(uint16_t ap_index, const uint8_t discriminator[4], lgbb_frame_cb_t on_frame,
                     lgbb_link_cb_t on_link);

/*
 * Core task, every pass: delivers frames that arrived, expires links, and sends the heartbeat.
 * Returns at once and never waits on the radio. Safe to call when no module is fitted.
 */
void lora_poll(uint32_t now_ms);

/*
 * Core task: offer one unsealed lg frame to the second backbone. wifi_ok says whether ESP-NOW is
 * carrying traffic to that AP right now; the policy in docs/lora.md decides from the frame and
 * that flag whether this costs airtime. node is the destination AP index, or LG_NODE_NONE to
 * reach every AP. Never blocks, never allocates, and does nothing when no module is fitted.
 */
void lora_offer(uint16_t node, const uint8_t *frame, size_t len, bool wifi_ok);

/* Core task: like lora_offer, for a frame being flooded to every AP except one. */
void lora_offer_flood(uint16_t except_node, const uint8_t *frame, size_t len);

/* The next frame offered goes on LoRa whatever the policy says. For `lora test`. */
void lora_force_next(void);

/* Whether AP `node` has been heard over LoRa recently enough to route to it. Core task. */
bool lora_is_peer(uint16_t node);

/* Whether a module answered at boot. */
bool lora_fitted(void);

/* Console: print the link, the counters and the settings. Core task. */
void lora_print(void);

/* Console: ask the LoRa task to reset and reconfigure the module. Any task. */
void lora_request_reset(void);

/* Console: send one AT command and log what comes back, for bring-up. Any task. */
void lora_request_at(const char *command);

/* Console: use the broadcast address for frames meant for every AP, or send one copy per peer. */
void lora_set_broadcast(bool on);

/* ---- chaos hooks (console only: never the BLE link, never the admin page) ---- */

/* Stop using LoRa for `seconds`, so a chaos run can prove the grid falls back to Wi-Fi. It comes
 * back by itself when the time is up, even if whatever turned it off never returns. */
void lora_disable(uint32_t seconds);
void lora_enable(void);
bool lora_is_off(void);

/* ---- reporting ---- */

#define LORA_TF_FITTED      0x01u
#define LORA_TF_CONFIGURED  0x02u
#define LORA_TF_BROADCAST   0x04u
#define LORA_TF_OFF         0x08u   /* the chaos hook is holding it off */
#define LORA_TF_BIG         0x10u   /* the large payload buffers were found */

/* The LoRa part of the D70 traffic record (docs/ble-link.md) and of /api/status. */
typedef struct {
    uint8_t  flags;            /* LORA_TF_* */
    uint8_t  address;
    uint8_t  networkid;
    int8_t   rssi;
    int8_t   snr;
    uint32_t frames_out;
    uint32_t frames_in;
    uint32_t frames_first;     /* of those, the ones Wi-Fi had not already delivered */
    uint32_t parts_out;
    uint32_t parts_in;
    uint32_t parts_dropped;
    uint32_t reasm_timeouts;
    uint32_t seal_fail;        /* frames we could not seal, open, or authenticate */
    uint32_t refused_big;      /* payloads larger than this AP carries: refused, never truncated */
    uint16_t queue_depth;
    uint16_t queue_high;
    uint32_t airtime_ms;
    uint32_t retries;
    uint32_t queue_dropped;
    uint32_t heard_age_ms;     /* since the last frame arrived; 0xFFFFFFFF never */
    uint8_t  peers;            /* bit n: AP n's heartbeat is current */
    uint8_t  restarts;         /* times a wedged module was reset and reconfigured */
} lora_traffic_t;

void lora_traffic(lora_traffic_t *out);

/* One line per AP this one has ever heard over LoRa, for the admin page. Core task. */
typedef struct {
    uint8_t  node;
    bool     up;
    int8_t   rssi;
    int8_t   snr;
    uint32_t age_ms;   /* since its last heartbeat or frame */
} lora_peer_info_t;

size_t lora_peer_info(lora_peer_info_t *out, size_t max, uint32_t now_ms);

/* The module's firmware string, or "" when none was read. */
const char *lora_version(void);
