/*
 * lg_identity.h - LocalGrid device identity.
 *
 * Every bench board gets a unique ID the first time tools/flash.py provisions it:
 *
 *     LG-<role>-<board>-<10 Crockford base32 characters>
 *
 *   role   N = infrastructure node, H = handheld
 *   board  three-character hardware code (see tools/bench_devices.json "boards")
 *   tail   hash of the board's MAC, the provisioning time, a text label, and random bytes;
 *          the MAC cannot be recovered from it and is never stored in the repository
 *
 * The tool writes the ID to the "lgid" NVS partition (namespace "lgid"). Node boards
 * also receive their node index and name there; handhelds receive their device index. Firmware prints the identity at boot
 * and answers the serial command "id" with a line starting "LGID: ".
 *
 * Partition keys (namespace "lgid"):
 *   id         string  "LG-<role>-<board>-<10 characters>"
 *   role       string  "N" or "H"
 *   board      string  three-character hardware code
 *   created    u32     provisioning time, Unix seconds
 *   nonce      string  the random hex string mixed into the ID when it was minted (since 2026-09)
 *   node_idx   u8      infrastructure boards only
 *   node_name  string  infrastructure boards only
 *   device_idx u32     handhelds only
 *   boot       u32     the highest boot counter this board has used (written by the firmware, D108)
 *
 * The nonce is not a secret. It exists so that the ID cannot be guessed from a MAC alone, and it
 * is stored so that the board can recompute its own ID at boot:
 *
 *     id = base32( SHA-256("LocalGrid device id v1|<role><board>|<mac>|<created>|<nonce>") )
 *
 * with <mac> the board's factory MAC written lowercase with colons. The board reads its own MAC
 * in memory only and never prints, logs, or stores it (decision D21). A board that answers with an
 * identity minted for a different board - a cloned identity, which breaks delivery, presence, and
 * addressing for both boards - is caught at boot by lg_identity_verified() and must not join the
 * grid. An identity written before the nonce was stored cannot be checked and changes nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_IDENTITY_ID_MAX     32
#define LG_IDENTITY_NAME_MAX   16
#define LG_IDENTITY_NONCE_MAX  33   /* hex text, 32 characters and the terminator */

/* Does the stored identity belong to the board it is running on? */
typedef enum {
    LG_IDENTITY_UNCHECKED = 0,   /* no identity, no stored nonce, or the MAC could not be read */
    LG_IDENTITY_OK,              /* the ID recomputes from this board's own MAC */
    LG_IDENTITY_MISMATCH,        /* this board is running another board's identity */
} lg_identity_check_t;

typedef struct {
    bool     present;                          /* false: board was never provisioned */
    char     id[LG_IDENTITY_ID_MAX];
    char     role[2];
    char     board[8];
    uint32_t created;                          /* provisioning time, Unix seconds */
    bool     has_node;
    uint8_t  node_index;
    char     node_name[LG_IDENTITY_NAME_MAX];
    bool     has_device;
    uint32_t device_index;                     /* handheld's address in the grid roster (one user per handheld) */
    char     nonce[LG_IDENTITY_NONCE_MAX];     /* mint nonce, hex; empty on boards provisioned before it was stored */
    lg_identity_check_t check;                 /* filled in by lg_identity_load */
} lg_identity_t;

/* Reads the identity partition. A missing partition or record yields present == false and ESP_OK. */
esp_err_t lg_identity_load(lg_identity_t *out);

/*
 * Mints an identity for this board and writes it to the "lgid" partition (D96): what tools/flash.py
 * --new-id does from the PC, done by the board itself, so a board the bench has never seen can be set up
 * from a browser. `role` is "N" or "H" and comes from the firmware, never from whoever is asking;
 * `board` is the three-character hardware code; `created` is the time now in Unix seconds, supplied by
 * the caller because a board that has never been on a grid does not know it; `index` is the access
 * point's index (0..15) or the handheld's device index (1..32); `name` is the access point's name and is
 * ignored for a handheld. The random part comes from the board's own generator. On ESP_OK the ID is in
 * `id_out` and takes effect at the next start.
 * ESP_ERR_INVALID_ARG for anything out of range, ESP_ERR_INVALID_STATE when the board's MAC cannot be read.
 */
esp_err_t lg_identity_mint(const char *role, const char *board, uint32_t created, uint32_t index,
                           const char *name, char *id_out, size_t id_len);

/*
 * Registers `identity new <board> <unix seconds> <index> [name] [replace]` on the console, for the web
 * flasher and for anyone with a cable. A board that already has an identity keeps it unless the last
 * word is "replace": two boards must never share one, and a board must not lose its own by accident.
 * The reply begins "LGIDNEW: ".
 */
void lg_identity_register_console(const char *role);

/*
 * The result of the check made by the last lg_identity_load: LG_IDENTITY_UNCHECKED before it
 * runs. Firmware refuses to join the grid on LG_IDENTITY_MISMATCH and behaves as it always did
 * on LG_IDENTITY_UNCHECKED.
 */
lg_identity_check_t lg_identity_verified(void);

/*
 * D108: the boot counter's floor, kept beside the identity. A board's boot counter lives in its main
 * settings storage, which an installer flash or an erase of the settings clears while the identity and
 * the grid keys survive: the counter then started again at 1, so the board reused (author, boot, seq)
 * - message ids, and the nonces they make, under the same key - and every neighbour threw its frames
 * away as replays. Firmware takes the larger of the two, adds one, and writes both before it transmits.
 * Minting a new identity keeps the floor. 0 when none was ever written.
 */
uint32_t  lg_identity_boot_floor(void);
esp_err_t lg_identity_set_boot_floor(uint32_t boot);

/* "this board's own", "another board's", or "not checked" - for logs and screens. */
const char *lg_identity_check_text(lg_identity_check_t check);

/*
 * Recomputes the device ID from the fields it was minted from. Pure: no NVS, no hardware.
 * nonce_hex is the stored mint nonce. Returns ESP_ERR_INVALID_ARG for a missing or malformed
 * field, ESP_FAIL if SHA-256 is unavailable.
 */
esp_err_t lg_identity_compute_id(const char *role, const char *board, const uint8_t mac[6], uint32_t created,
                                 const char *nonce_hex, char *out, size_t out_len);

/*
 * Checks a loaded identity against a given MAC. Pure, so tests can pass a fixed MAC.
 * Returns LG_IDENTITY_UNCHECKED when there is nothing to check against.
 */
lg_identity_check_t lg_identity_check(const lg_identity_t *id, const uint8_t mac[6]);

/* Prints "LGID: <id> role=<r> board=<b> [node=<n> <name>]" or "LGID: NONE".
   A board running another board's identity gets " identity=MISMATCH" on the end. */
void lg_identity_print(const lg_identity_t *id);

/*
 * For firmware without an esp_console REPL: installs the console driver and starts a
 * small task that answers "id" lines on the serial console. id must outlive the task.
 */
esp_err_t lg_identity_start_responder(const lg_identity_t *id);

#ifdef __cplusplus
}
#endif
