/*
 * lg_fwupdate.h - taking a signed firmware update (docs/firmware-updates.md, D97).
 *
 * One way in for every source: the admin page's upload, the serial console, and a neighbour passing an
 * update on. The bytes of an update file (.lgfw) are fed in order; nothing is written until the first
 * 192 bytes - the header and its signature - have been checked against the release key this device was
 * given (D96), against what this device is, and against what it already runs. Then the image goes to
 * the spare slot as it arrives, hashed on the way, and is made bootable only if the whole of it matches
 * the header. Until that last step the running firmware is untouched and stays the one that boots.
 *
 * New firmware runs once on trial (bootloader rollback): it stays only if it comes up far enough to
 * confirm itself, which both firmwares already do.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LG_FWU_HEADER_LEN 128u
#define LG_FWU_SIG_LEN    64u
#define LG_FWU_PREFIX_LEN (LG_FWU_HEADER_LEN + LG_FWU_SIG_LEN)   /* what must arrive before anything is trusted */
#define LG_FWU_VERSION_MAX 32u

typedef struct {
    char     role;                           /* 'N' or 'H' */
    uint8_t  chip;                           /* 1 ESP32, 2 ESP32-S3, 3 ESP32-C6 */
    uint32_t serial;                         /* newer wins */
    uint32_t image_len;
    uint8_t  sha256[32];
    char     version[LG_FWU_VERSION_MAX + 1u];
} lg_fwu_info_t;

/* Why an update was refused, in words fit for a screen or a log; "" when it was not. */
const char *lg_fwu_reason(void);

/*
 * Starts taking an update for a device of `role` ('N' or 'H'). ESP_ERR_INVALID_STATE while another is
 * in hand. Only one at a time, and all calls from one task.
 */
esp_err_t lg_fwu_begin(char role);

/* The next bytes of the file. Any error ends the update and leaves the running firmware as it was. */
esp_err_t lg_fwu_write(const uint8_t *data, size_t len);

/* Everything has been fed in: check the whole image and make it the one that boots next.
 * ESP_OK means restart to run it. */
esp_err_t lg_fwu_finish(void);

/* Gives up an update in hand. Safe when none is. */
void lg_fwu_abort(void);

/* True while an update is being taken; `received` and `total` are bytes of the file. */
bool lg_fwu_progress(size_t *received, size_t *total);

/* What the header of the update in hand said, once it has been checked; false before that. */
bool lg_fwu_info(lg_fwu_info_t *out);

/* The serial and version of the firmware running now: 0 and "" when it was put there by a cable
 * rather than by an update, which any signed update is newer than. */
uint32_t lg_fwu_running_serial(void);
void lg_fwu_running_version(char out[LG_FWU_VERSION_MAX + 1u]);

/* This chip's code in an update header. */
uint8_t lg_fwu_this_chip(void);

/* Registers `update` on the console: `update` says what is running, and `update begin`, `update data
 * <base64>`, `update end` take a file over the serial line, which is how tools/lgfw.py sends one.
 * Replies begin "LGFWU: ". */
void lg_fwu_register_console(char role);
