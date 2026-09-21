/*
 * hh_lora.h - a LoRa module on a handheld (D71's "Optional on handhelds too", unblocked by D76).
 *
 * The same Reyax RYLR998 the APs carry, on the board profile's lora_rx/lora_tx/lora_reset pins,
 * driven by the same radio-independent half the APs use (firmware/common/lora_wire.c): base64, the
 * part header, reassembly, the bounded send queue, the AT parser and the airtime sum. Only the
 * driver below is new, and it is deliberately smaller than the AP's.
 *
 * **Optional, and that comes first.** Most handhelds will never have a module. A handheld with
 * none, or with one that does not answer, behaves exactly as a handheld built before this file:
 * one [LORA] line at most, no frame buffers taken, nothing transmitted, no retry storm, no delay
 * at start-up, and nothing on screen beyond "no module". A board whose profile names no LoRa pin
 * does not even open a serial port.
 *
 * **What it carries** (docs/lora.md, "Ready for handhelds later"): this handheld's position (D65),
 * its keepalive and battery, an SOS or urgent broadcast, short text, and the acknowledgements
 * those need. Never live voice, never registration, the roster, names, group edits or the shared
 * state - lora_policy_for_handheld_frame() is the whole list and the tests hold it to it.
 *
 * **When it carries it** (D74): only when the handheld cannot reach an AP over Wi-Fi, except that
 * an alert always goes on both radios. Nothing is ever sent twice over a working link, and a
 * message the outbox keeps retrying is put on the air at most once every HH_LORA_REOFFER_MS, so a
 * three-second retransmission timer cannot become a transmission every three seconds.
 *
 * **Threading.** The service task seals frames, offers them, and drains what arrived; everything
 * slow - a transmission is seconds of airtime - happens on the "hh_lora" task, which shares only a
 * bounded queue and a mutex with it. Service-side only (D27): screens learn about the radio
 * through hh_service.h, never from here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_crypto.h"

/*
 * A frame an outbox keeps retrying is offered to the radio again and again. One transmission of a
 * short text is about two seconds of airtime and over 100 mA, so the same message identity goes on
 * the air at most this often, however often it is offered. An alert is not exempt: an SOS that
 * went out is an SOS that went out, and repeating it every three seconds would drown the grid
 * and the cell both.
 */
#define HH_LORA_REOFFER_MS   30000u

/*
 * A frame that carries no message identity - the keepalive, a position - is rate-limited by its
 * type instead, so a handheld that has fallen off Wi-Fi reports itself about once a minute rather
 * than at the ten-second cadence Wi-Fi is happy with.
 */
#define HH_LORA_ROUTINE_MS   60000u

/* A frame that arrived for this handheld, on the service task: hand it to lg_client. */
typedef void (*hh_lora_frame_cb_t)(const uint8_t *frame, size_t len);

typedef struct {
    int      rx_gpio;      /* the module's TXD; LG_PIN_NONE means this board has no connector */
    int      tx_gpio;      /* the module's RXD */
    int      rst_gpio;     /* the module's RST, or LG_PIN_NONE */
    uint32_t device;       /* this handheld's device number: its address is 100 + it */
    uint32_t boot;         /* the NVS boot counter, already committed: nonces depend on it */
    uint8_t  discriminator[4];
    uint8_t  key[LG_AEAD_KEY_LEN];        /* LG_SECRET_LORA_KEY (D76), never the backbone key */
    hh_lora_frame_cb_t on_frame;
} hh_lora_cfg_t;

/*
 * Starts the reader task. ESP_ERR_INVALID_ARG when this board has no LoRa pin or the device number
 * is above what the wire format's peer nibble carries - in both cases nothing is started, nothing
 * is allocated and no port is opened. ESP_OK once the task is running, whether or not a module
 * answered: that is decided on the task, not in the boot path.
 */
esp_err_t hh_lora_start(const hh_lora_cfg_t *cfg);

/*
 * Service task, every pass: hands over frames that arrived and expires the link. Returns at once,
 * never waits on the radio, and does nothing at all when no module is fitted.
 */
void hh_lora_poll(uint32_t now_ms);

/*
 * Service task: offer one unsealed lg frame to the radio. wifi_ok says whether an AP is reachable
 * over Wi-Fi right now. Returns true when the frame was sealed and queued. Never blocks, never
 * allocates, and does nothing when no module is fitted.
 */
bool hh_lora_offer(const uint8_t *frame, size_t len, bool wifi_ok);

/* Whether a module answered, so the service can decide to build frames for it at all. */
bool hh_lora_fitted(void);

/* ---- what the Status screen and the console show ---- */

#define HH_LORA_F_FITTED     0x01u
#define HH_LORA_F_CONFIGURED 0x02u
#define HH_LORA_F_BROADCAST  0x04u
#define HH_LORA_F_OFF        0x08u   /* the console hook is holding it off */
#define HH_LORA_F_STARTED    0x10u   /* this board has pins and the reader runs */
#define HH_LORA_F_EVER       0x20u   /* a module answered at some point since boot */
#define HH_LORA_F_LINK       0x40u   /* an AP has been heard within HH_LORA_LINK_TIMEOUT_MS */

#define HH_LORA_LINK_TIMEOUT_MS 300000u   /* nothing heard for five minutes: call the link down */

typedef struct {
    uint8_t  flags;
    uint8_t  address;        /* this handheld's LoRa address, 100 + device */
    uint8_t  networkid;
    uint8_t  ap;             /* the AP last heard, by its LoRa address; 0 none */
    int8_t   rssi;           /* of the last part received */
    int8_t   snr;
    uint32_t frames_out, frames_in;
    uint32_t parts_out, parts_in, parts_dropped;
    uint32_t seal_fail;      /* frames that would not seal, open, or authenticate */
    uint32_t held_back;      /* offers the policy or the rate limit refused: airtime not spent */
    uint32_t queue_dropped;
    uint32_t airtime_ms;
    uint32_t retries;
    uint32_t heard_age_ms;   /* since the last part arrived; 0xFFFFFFFF never */
    uint8_t  restarts;
    char     version[28];    /* the module's firmware string, or "" */
    int8_t   rx_gpio, tx_gpio, rst_gpio;
} hh_lora_info_t;

/* Safe from any task, before or without a start (all zero, pins -1). */
void hh_lora_info(hh_lora_info_t *out);

/* ---- console (hh_console.c), mirroring the AP's `lora` ---- */

void hh_lora_print(void);
void hh_lora_request_reset(void);
void hh_lora_request_at(const char *command);
void hh_lora_set_broadcast(bool on);
void hh_lora_force_next(void);      /* the next frame offered goes whatever the policy says */
void hh_lora_disable(uint32_t seconds);
void hh_lora_enable(void);
bool hh_lora_is_off(void);
