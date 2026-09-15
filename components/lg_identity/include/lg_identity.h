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
 * also receive their node index and name there. Firmware prints the identity at boot
 * and answers the serial command "id" with a line starting "LGID: ".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_IDENTITY_ID_MAX    32
#define LG_IDENTITY_NAME_MAX  16

typedef struct {
    bool     present;                          /* false: board was never provisioned */
    char     id[LG_IDENTITY_ID_MAX];
    char     role[2];
    char     board[8];
    uint32_t created;                          /* provisioning time, Unix seconds */
    bool     has_node;
    uint8_t  node_index;
    char     node_name[LG_IDENTITY_NAME_MAX];
} lg_identity_t;

/* Reads the identity partition. A missing partition or record yields present == false and ESP_OK. */
esp_err_t lg_identity_load(lg_identity_t *out);

/* Prints "LGID: <id> role=<r> board=<b> [node=<n> <name>]" or "LGID: NONE". */
void lg_identity_print(const lg_identity_t *id);

/*
 * For firmware without an esp_console REPL: installs the console driver and starts a
 * small task that answers "id" lines on the serial console. id must outlive the task.
 */
esp_err_t lg_identity_start_responder(const lg_identity_t *id);

#ifdef __cplusplus
}
#endif
