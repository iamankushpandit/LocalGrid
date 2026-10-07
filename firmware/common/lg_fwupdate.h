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

/*
 * Passing an update on (D101). A device that is running firmware it took as a signed update kept that
 * update's first 192 bytes, and the image is the one in its running slot, so it can produce the file
 * again byte for byte. lg_fwu_held says whether it can and what the file is; lg_fwu_held_read copies
 * up to len bytes of the file from offset and returns how many (0 at or past the end, -1 on a read
 * error). A device whose firmware came by cable holds nothing: it has no signature to hand over.
 */
bool lg_fwu_held(lg_fwu_info_t *out, size_t *file_len);
int  lg_fwu_held_read(size_t offset, uint8_t *out, size_t len);

/*
 * Carrying a handheld's update (D107). An access point given a signed update for a *handheld* cannot
 * run it and does not try: it checks the signature and the image exactly as it would its own, keeps the
 * file in its spare firmware slot without making it bootable, and hands it to handhelds and to other
 * access points. It happens by itself: lg_fwu_begin('N') followed by a file whose signed header says
 * 'H' is carried, by whatever road the file came. After such a file lg_fwu_finish returns ESP_OK and
 * lg_fwu_finished_carried is true, and the caller must not restart.
 *
 * One at a time, for one chip: the spare slot holds one image. A newer serial replaces what is carried.
 * An access point's own update is written to the same slot and displaces it. Nothing is carried while
 * this access point's own firmware is still on trial, because the spare slot is then its way back.
 */
bool lg_fwu_finished_carried(void);
/*
 * Stops carrying the handheld update held here, and remembers its serial so the same one, or an older
 * one, is not taken again from a neighbour that still offers it. Each access point is told for itself.
 * Returns the serial dropped, 0 if nothing was carried. lg_fwu_carry_floor is that remembered serial:
 * a handheld update must be newer than it to be carried here.
 */
uint32_t lg_fwu_carried_drop(void);
/* D111: the floor is per chip, so withdrawing one chip's update never blocks another chip's. */
uint32_t lg_fwu_carry_floor(uint8_t chip);
/*
 * D111: a withdrawal heard from another AP: the handheld update for `chip` up to `serial` is not to be
 * carried. Drops this AP's copy if it is that chip and no newer, and raises the floor. True if anything
 * changed here, which is when the withdrawal is passed on.
 */
bool lg_fwu_carry_withdraw(uint8_t chip, uint32_t serial);
bool lg_fwu_carried(lg_fwu_info_t *out, size_t *file_len);
int  lg_fwu_carried_read(size_t offset, uint8_t *out, size_t len);

/* This chip's code in an update header. */
uint8_t lg_fwu_this_chip(void);

/* Registers `update` on the console: `update` says what is running, and `update begin`, `update data
 * <base64>`, `update end` take a file over the serial line, which is how tools/lgfw.py sends one.
 * Replies begin "LGFWU: ". */
void lg_fwu_register_console(char role);
