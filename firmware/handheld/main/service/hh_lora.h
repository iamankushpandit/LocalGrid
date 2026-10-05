/*
 * hh_lora.h - a LoRa module on a handheld (D77).
 *
 * The same Reyax RYLR998 the APs carry (D71), on the board profile's LoRa connector, speaking AT
 * commands over a UART of its own. This is the handheld half of the second radio: it carries a
 * handheld's position, its presence, short text (which is how an SOS, an urgent broadcast and an
 * all clear travel) and message acknowledgements, to and from the APs. Nothing else.
 *
 * What it deliberately is NOT:
 *
 * - **Not a backbone peer.** A handheld talks to AP addresses only (D77). Handheld-to-handheld
 *   LoRa is not built: it is a peer relationship lg_core has never had, and it would need its own
 *   routing and duplicate suppression.
 * - **Not sealed with the backbone key.** Handhelds do not hold it. Frames are sealed with
 *   LG_SECRET_LORA_HH_KEY through lg_lora_hh.h, a separate derived key (D77), so a handheld that
 *   is lost cannot read or forge one byte of AP-to-AP traffic.
 * - **Not a voice path.** Live push-to-talk cannot work on this radio at any setting (D71), and
 *   the 10-second voice note of D78 is not built yet.
 * - **Not required.** A handheld with no module, or a board with no connector, behaves exactly as
 *   it does today: the module is looked for once, its absence is logged once, and nothing else in
 *   the firmware changes. Every board but the Freenove FNK0104B has no connector at all.
 *
 * Threading. The service task seals frames, so the one sequence counter stays single-threaded, and
 * calls hh_lora_poll() to take received frames back. Everything slow - one part is about a second
 * of airtime - happens on the "hh_lora" task, which shares a bounded queue and a mutex with it.
 * Screens never see this header (D27): what they show comes through hh_service.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- the radio and the addresses ----------------------------------------------------------- */

/*
 * Both come from firmware/common/lg_lora_wire.h, the file this firmware and the APs' both compile,
 * so a handheld cannot end up on a different channel from the grid through a one-sided edit. This
 * header adds only what is a handheld's own business: how often it speaks, and its own address.
 */
#include "lg_lora_wire.h"

/* This handheld's own address: 100 + its device number (LG_LORA_ADDR_HANDHELD). */
#define HH_LORA_ADDR_SELF(dev)   LG_LORA_ADDR_HANDHELD(dev)

/*
 * How long an AP stays reachable after its last word over LoRa, and how often a handheld should say
 * it is there once something builds that announcement (see hh_lora_poll: nothing does yet). Slower
 * than the APs' own minute (D74) because a handheld runs on a battery and has far less to say, and
 * it would speak at all only while Wi-Fi is not carrying.
 */
#define HH_LORA_HEARTBEAT_MS     120000u   /* not sent yet: no announcement is built */
#define HH_LORA_PEER_TIMEOUT_MS  400000u   /* three missed heartbeats and a little */
#define HH_LORA_PROBE_MS         60000u    /* how often a handheld with no module looks again */

/* ---- starting it ---- */

/*
 * Starts the reader task on the board profile's pins. rst may be -1 where the board leaves the
 * module's reset unconnected; the firmware then falls back to the module's own AT+RESET.
 *
 * `device` is this handheld's device index and `boot` its boot counter, already committed to NVS
 * before this is called, because a nonce must never repeat. `key` is LG_SECRET_LORA_HH_KEY.
 *
 * Returns ESP_OK once the task is running, whether or not a module answered, and
 * ESP_ERR_INVALID_ARG for a board with no connector - which is not a failure and needs no log.
 */
typedef void (*hh_lora_frame_cb_t)(const uint8_t *frame, size_t len, uint16_t from_ap);

esp_err_t hh_lora_start(int rx_gpio, int tx_gpio, int rst_gpio, uint32_t device, uint32_t boot,
                        const uint8_t key[32], const uint8_t discriminator[4],
                        hh_lora_frame_cb_t on_frame);

/*
 * Service task, every pass: hands over frames that arrived, expires peers whose last word has aged
 * out, and restores the radio after a timed outage. Returns at once and never waits on the radio.
 *
 * It sends **nothing** of its own. Unlike an AP, which heartbeats to keep its links measured, a
 * handheld speaks only when the service offers it a frame: the periodic announcement that tells an
 * AP a handheld is reachable this way is the service's to send, through hh_lora_offer, because only
 * the service can build one. Until it does, an AP learns of a handheld the first time that handheld
 * has something to say -- which for an SOS is exactly when it matters, and for nothing else yet.
 */
void hh_lora_poll(uint32_t now_ms);

/*
 * Service task: offer one unsealed lg frame to the radio. `wifi_ok` says whether this handheld has
 * a working AP session right now; as on the APs (D74), one radio carries at a time, except that
 * urgent broadcasts, an SOS and an all clear always go on both. A frame this path does not carry
 * (lglh_type_allowed) is refused and counted, never sent.
 *
 * Never blocks, never allocates, and does nothing when no module is fitted.
 */
void hh_lora_offer(const uint8_t *frame, size_t len, bool wifi_ok);

/* Whether a module answered, so the screens and the console can say so. Any task. */
bool hh_lora_fitted(void);

/* Whether any AP has been heard over LoRa recently enough to be worth transmitting at. Any task. */
bool hh_lora_has_peer(void);

/* ---- reporting (D23: a handheld shows this on its screen, not only on serial) ---- */

typedef struct {
    bool     connector;      /* this board has somewhere to wire a module */
    bool     fitted;         /* a module answered */
    bool     configured;     /* and took every setting */
    bool     off;            /* switched off from the console */
    char     version[24];    /* the module's own firmware string, "" if unread */
    uint16_t address;
    uint8_t  networkid;
    int8_t   rssi;           /* of the last frame in */
    int8_t   snr;
    uint32_t frames_out;
    uint32_t frames_in;
    uint32_t parts_out;
    uint32_t parts_in;
    uint32_t parts_dropped;
    uint32_t reasm_timeouts;
    uint32_t seal_fail;      /* could not open or authenticate: wrong key, or another grid */
    uint32_t refused_type;   /* a message type this path does not carry (D77's allow-list) */
    uint32_t queue_dropped;
    uint32_t retries;
    uint32_t airtime_ms;
    uint32_t restarts;       /* times a wedged module was reset */
    uint32_t heard_age_ms;   /* since the last frame arrived; UINT32_MAX never */
    uint8_t  peers;          /* bit n: AP n has been heard recently */
} hh_lora_info_t;

void hh_lora_info(hh_lora_info_t *out);

/*
 * Console: stop using the radio for `seconds`, and put it back by itself even if nobody returns.
 * Capped, like the APs' `lora off` and D78's `wifi off <seconds>`, so a typo cannot take a
 * handheld's second radio out for a day.
 */
#define HH_LORA_OFF_MAX_S 3600u
void hh_lora_disable(uint32_t seconds);
void hh_lora_enable(void);

/*
 * Bring-up: point the LoRa UART at different pins, without a restart, and look for a module again.
 *
 * Why this can work at all: the ESP32-S3 routes any UART through the GPIO matrix, so the pins are a
 * runtime property of an already-open port rather than something fixed at install. The reader task
 * looks for a module on every pass anyway - that is how a module plugged in later is picked up - so
 * re-pointing the port and clearing `fitted` is the whole of it.
 *
 * Why it exists: a board profile's LoRa pins are a guess until a module has answered on them. The
 * Freenove profile proposed GPIO40/41/42 from free header pins, a module was fitted somewhere else,
 * and the only symptom was one line saying no module was found - which is indistinguishable from no
 * module being fitted. Probing beats re-flashing a profile per guess.
 *
 * Refuses pins this board must never drive. Returns ESP_OK once the port is re-pointed, which is not
 * a claim that anything answered: watch [LORA] for that.
 */
esp_err_t hh_lora_set_pins(int rx_gpio, int tx_gpio);

/* Console: reset and reconfigure the module, or ask it something directly, for bring-up. */
void hh_lora_request_reset(void);
void hh_lora_request_at(const char *command);

/* D98: transmit power, one of LG_LORA_POWER_FULL, _MEDIUM, _LOW (anything else goes to the nearest).
 * May be called before hh_lora_start, which then configures the module with it. */
void hh_lora_set_power(int dbm);
int  hh_lora_power(void);
