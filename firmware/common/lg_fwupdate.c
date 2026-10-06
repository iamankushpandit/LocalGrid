/* lg_fwupdate.c - see lg_fwupdate.h (D97). */
#include "lg_fwupdate.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_crypto.h"
#include "lg_gridkeys.h"
#include "lg_lora_wire.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "GRID";

#define NVS_NAMESPACE "lgfw"

static struct {
    bool            active;
    bool            checked;          /* the header and signature passed; the image is being written */
    char            role;
    uint8_t         prefix[LG_FWU_PREFIX_LEN];
    size_t          have;             /* bytes of the file taken so far */
    lg_fwu_info_t   info;
    const esp_partition_t *target;
    esp_ota_handle_t ota;
    lg_sha256_ctx_t hash;
    const char     *reason;
    bool            carry;            /* D107: a handheld's update, kept in the spare slot and never booted */
    bool            carried_done;     /* the last file finished was one of those */
    size_t          erased;           /* bytes of the spare slot erased so far for it */
} u = { .reason = "" };

#define CARRY_SECTOR 4096u

const char *lg_fwu_reason(void)
{
    return u.reason;
}

uint8_t lg_fwu_this_chip(void)
{
#if CONFIG_IDF_TARGET_ESP32
    return 1;
#elif CONFIG_IDF_TARGET_ESP32S3
    return 2;
#elif CONFIG_IDF_TARGET_ESP32C6
    return 3;
#else
    return 0;
#endif
}

/* The serial and version are kept per slot, so "what is running" needs no bookkeeping at the moment an
 * update is confirmed or rolled back: it is whatever was recorded for the slot that booted. */
static void slot_key(const esp_partition_t *p, char kind, char out[16])
{
    snprintf(out, 16, "%c_%.12s", kind, p != NULL ? p->label : "none");
}

uint32_t lg_fwu_running_serial(void)
{
    nvs_handle_t h;
    uint32_t serial = 0;
    char key[16];
    slot_key(esp_ota_get_running_partition(), 's', key);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, key, &serial);
        nvs_close(h);
    }
    return serial;
}

void lg_fwu_running_version(char out[LG_FWU_VERSION_MAX + 1u])
{
    nvs_handle_t h;
    char key[16];
    out[0] = '\0';
    slot_key(esp_ota_get_running_partition(), 'v', key);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = LG_FWU_VERSION_MAX + 1u;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

/* What a signed header says. Only to be believed once its signature has been checked. */
static void parse_header(const uint8_t *h, lg_fwu_info_t *info)
{
    info->role = (char)h[5];
    info->chip = h[6];
    info->serial = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
    info->image_len = (uint32_t)h[12] | ((uint32_t)h[13] << 8) | ((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
    memcpy(info->sha256, h + 16, 32);
    memcpy(info->version, h + 56, LG_FWU_VERSION_MAX);
    info->version[LG_FWU_VERSION_MAX] = '\0';
}

/* ---- passing it on (D101): the file this device is running, served again ---- */

static struct {
    bool          known;
    bool          ok;
    uint8_t       prefix[LG_FWU_PREFIX_LEN];
    lg_fwu_info_t info;
    const esp_partition_t *slot;
} held;

static void held_load(void)
{
    if (held.known) {
        return;
    }
    held.known = true;
    held.slot = esp_ota_get_running_partition();
    nvs_handle_t h;
    char key[16];
    size_t len = sizeof(held.prefix);
    slot_key(held.slot, 'p', key);
    if (held.slot == NULL || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    bool got = nvs_get_blob(h, key, held.prefix, &len) == ESP_OK && len == sizeof(held.prefix);
    nvs_close(h);
    if (!got || memcmp(held.prefix, "LGFW", 4) != 0 || held.prefix[4] != 1) {
        return;
    }
    parse_header(held.prefix, &held.info);
    /* The kept bytes must be this slot's: same serial as was recorded for it, and an image that fits. */
    held.ok = held.info.serial != 0 && held.info.serial == lg_fwu_running_serial() && held.info.image_len != 0 &&
              held.info.image_len <= held.slot->size && held.info.chip == lg_fwu_this_chip();
}

bool lg_fwu_held(lg_fwu_info_t *out, size_t *file_len)
{
    held_load();
    if (!held.ok) {
        return false;
    }
    if (out != NULL) {
        *out = held.info;
    }
    if (file_len != NULL) {
        *file_len = LG_FWU_PREFIX_LEN + held.info.image_len;
    }
    return true;
}

int lg_fwu_held_read(size_t offset, uint8_t *out, size_t len)
{
    held_load();
    if (!held.ok || out == NULL) {
        return -1;
    }
    size_t total = LG_FWU_PREFIX_LEN + held.info.image_len;
    if (offset >= total) {
        return 0;
    }
    if (len > total - offset) {
        len = total - offset;
    }
    size_t done = 0;
    if (offset < LG_FWU_PREFIX_LEN) {
        done = LG_FWU_PREFIX_LEN - offset < len ? LG_FWU_PREFIX_LEN - offset : len;
        memcpy(out, held.prefix + offset, done);
    }
    if (done < len &&
        esp_partition_read(held.slot, offset + done - LG_FWU_PREFIX_LEN, out + done, len - done) != ESP_OK) {
        return -1;
    }
    return (int)len;
}

/* ---- carrying a handheld's update (D107) ---- */

static struct {
    bool          known;
    bool          ok;
    uint8_t       prefix[LG_FWU_PREFIX_LEN];
    lg_fwu_info_t info;
    const esp_partition_t *slot;
} carried;

static void carried_load(void)
{
    if (carried.known) {
        return;
    }
    carried.known = true;
    carried.ok = false;
    carried.slot = esp_ota_get_next_update_partition(NULL);
    nvs_handle_t h;
    char label[17] = "";
    size_t len = sizeof(carried.prefix);
    size_t llen = sizeof(label);
    if (carried.slot == NULL || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    bool got = nvs_get_blob(h, "c_hdr", carried.prefix, &len) == ESP_OK && len == sizeof(carried.prefix) &&
               nvs_get_str(h, "c_slot", label, &llen) == ESP_OK;
    nvs_close(h);
    /* The file is in the slot it was written to only while that is still the spare one. */
    if (!got || strcmp(label, carried.slot->label) != 0 || memcmp(carried.prefix, "LGFW", 4) != 0 ||
        carried.prefix[4] != 1) {
        return;
    }
    parse_header(carried.prefix, &carried.info);
    carried.ok = carried.info.role == 'H' && carried.info.serial != 0 && carried.info.image_len != 0 &&
                 carried.info.image_len <= carried.slot->size;
}

/* The spare slot is about to be written: whatever was carried in it is no longer whole. */
static void carried_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, "c_hdr");
        (void)nvs_erase_key(h, "c_slot");
        (void)nvs_commit(h);
        nvs_close(h);
    }
    carried.known = false;
}

bool lg_fwu_carried(lg_fwu_info_t *out, size_t *file_len)
{
    carried_load();
    if (!carried.ok || (u.active && u.checked)) {
        return false;   /* nothing, or the slot is being written this moment */
    }
    if (out != NULL) {
        *out = carried.info;
    }
    if (file_len != NULL) {
        *file_len = LG_FWU_PREFIX_LEN + carried.info.image_len;
    }
    return true;
}

int lg_fwu_carried_read(size_t offset, uint8_t *out, size_t len)
{
    if (out == NULL || !lg_fwu_carried(NULL, NULL)) {
        return -1;
    }
    size_t total = LG_FWU_PREFIX_LEN + carried.info.image_len;
    if (offset >= total) {
        return 0;
    }
    if (len > total - offset) {
        len = total - offset;
    }
    size_t done = 0;
    if (offset < LG_FWU_PREFIX_LEN) {
        done = LG_FWU_PREFIX_LEN - offset < len ? LG_FWU_PREFIX_LEN - offset : len;
        memcpy(out, carried.prefix + offset, done);
    }
    if (done < len &&
        esp_partition_read(carried.slot, offset + done - LG_FWU_PREFIX_LEN, out + done, len - done) != ESP_OK) {
        return -1;
    }
    return (int)len;
}

bool lg_fwu_finished_carried(void)
{
    return u.carried_done;
}

uint32_t lg_fwu_carry_floor(void)
{
    nvs_handle_t h;
    uint32_t floor = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, "c_floor", &floor);
        nvs_close(h);
    }
    return floor;
}

uint32_t lg_fwu_carried_drop(void)
{
    lg_fwu_info_t have;
    if (!lg_fwu_carried(&have, NULL)) {
        return 0;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (have.serial > lg_fwu_carry_floor()) {
            (void)nvs_set_u32(h, "c_floor", have.serial);
        }
        (void)nvs_commit(h);
        nvs_close(h);
    }
    carried_forget();
    ESP_LOGW(TAG, "[GRID] Handheld update serial %u is no longer carried here, and will not be taken again",
             (unsigned)have.serial);
    return have.serial;
}

static esp_err_t refuse(esp_err_t err, const char *why)
{
    u.reason = why;
    ESP_LOGW(TAG, "[GRID] Firmware update refused: %s", why);
    lg_fwu_abort();
    u.reason = why;   /* abort clears nothing of this, but be plain about it */
    return err;
}

void lg_fwu_abort(void)
{
    if (u.active && u.checked) {
        if (!u.carry) {
            (void)esp_ota_abort(u.ota);
        }
        lg_sha256_abort(&u.hash);
    }
    u.active = false;
    u.checked = false;
    u.carry = false;
    u.have = 0;
}

esp_err_t lg_fwu_begin(char role)
{
    if (u.active) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&u.info, 0, sizeof(u.info));
    u.role = role;
    u.have = 0;
    u.checked = false;
    u.carry = false;
    u.carried_done = false;
    u.reason = "";
    u.active = true;
    return ESP_OK;
}

/*
 * D107: the signed header says this is a handheld's firmware and this is an access point. It is kept,
 * not run: written raw to the spare slot (the OTA calls would refuse an image built for another chip,
 * and would be right to), hashed as it arrives, and never made bootable.
 */
static esp_err_t carry_begin(void)
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        return refuse(ESP_ERR_INVALID_STATE, "this access point's own new firmware is still on trial, and the spare "
                                             "slot is its way back; give it the handheld update again in a few minutes");
    }
    lg_fwu_info_t have;
    if (lg_fwu_carried(&have, NULL) && u.info.serial <= have.serial) {
        return refuse(ESP_ERR_INVALID_VERSION, "it is not newer than the handheld update this access point already carries");
    }
    if (u.info.serial <= lg_fwu_carry_floor()) {
        return refuse(ESP_ERR_INVALID_VERSION, "that handheld update, or a newer one, was withdrawn on this access point");
    }
    u.target = esp_ota_get_next_update_partition(NULL);
    if (u.target == NULL) {
        return refuse(ESP_ERR_NOT_FOUND, "this access point has no spare firmware slot to carry it in");
    }
    if (u.info.image_len == 0 || u.info.image_len > u.target->size) {
        return refuse(ESP_ERR_INVALID_SIZE, "the image does not fit the spare slot");
    }
    carried_forget();
    if (lg_sha256_begin(&u.hash) != 0) {
        return refuse(ESP_FAIL, "the image could not be checked");
    }
    u.carry = true;
    u.erased = 0;
    u.checked = true;
    ESP_LOGW(TAG, "[GRID] Handheld update %s (serial %u, chip %u), %u bytes, signed by the release key: "
                  "carrying it in %s for handhelds, not running it",
             u.info.version, (unsigned)u.info.serial, u.info.chip, (unsigned)u.info.image_len, u.target->label);
    return ESP_OK;
}

/* The first 192 bytes are in hand: trust nothing past this point unless every check passes. */
static esp_err_t check_prefix(void)
{
    const uint8_t *h = u.prefix;
    const lg_gridkeys_t *k = lg_gridkeys();
    if (memcmp(h, "LGFW", 4) != 0 || h[4] != 1) {
        return refuse(ESP_ERR_INVALID_ARG, "that is not a LocalGrid update file");
    }
    if (!k->has_release) {
        return refuse(ESP_ERR_NOT_ALLOWED, "this device was not given a release key, so it accepts no update");
    }
    uint8_t kid[32];
    if (lg_sha256(k->release_pub, LG_P256_PUB_LEN, kid) != 0 || memcmp(kid, h + 48, 8) != 0 ||
        lg_ecdsa_p256_verify(k->release_pub, h, LG_FWU_HEADER_LEN, h + LG_FWU_HEADER_LEN) != 0) {
        return refuse(ESP_ERR_NOT_ALLOWED, "it is not signed by the release key this device trusts");
    }
    /* Signed, so the header can be believed. */
    parse_header(h, &u.info);
    if (u.info.role != u.role) {
        if (u.role == 'N' && u.info.role == 'H') {
            return carry_begin();   /* D107: an access point carries a handheld's update */
        }
        return refuse(ESP_ERR_NOT_SUPPORTED, u.role == 'N' ? "it is firmware for a handheld, and this is an access point"
                                                           : "it is firmware for an access point, and this is a handheld");
    }
    if (u.info.chip != lg_fwu_this_chip()) {
        return refuse(ESP_ERR_NOT_SUPPORTED, "it was built for a different chip");
    }
    if (u.info.serial <= lg_fwu_running_serial()) {
        return refuse(ESP_ERR_INVALID_VERSION, "it is not newer than the firmware already running");
    }
    u.target = esp_ota_get_next_update_partition(NULL);
    if (u.target == NULL) {
        return refuse(ESP_ERR_NOT_FOUND, "this device has no second firmware slot; it must be reflashed over USB once");
    }
    if (u.info.image_len == 0 || u.info.image_len > u.target->size) {
        return refuse(ESP_ERR_INVALID_SIZE, "the image does not fit the update slot");
    }
    carried_forget();   /* D107: this access point's own update takes the slot a handheld's was carried in */
    /* Erased a sector at a time as the image arrives, not all at once here: erasing a megabyte up
     * front holds the calling task for seconds, and since D101 that task can be an AP's core task. */
    if (esp_ota_begin(u.target, OTA_WITH_SEQUENTIAL_WRITES, &u.ota) != ESP_OK) {
        return refuse(ESP_FAIL, "the update slot could not be opened");
    }
    if (lg_sha256_begin(&u.hash) != 0) {
        (void)esp_ota_abort(u.ota);
        return refuse(ESP_FAIL, "the image could not be checked");
    }
    u.checked = true;
    ESP_LOGW(TAG, "[GRID] Firmware update %s (serial %u), %u bytes, signed by the release key: writing to %s",
             u.info.version, (unsigned)u.info.serial, (unsigned)u.info.image_len, u.target->label);
    return ESP_OK;
}

esp_err_t lg_fwu_write(const uint8_t *data, size_t len)
{
    if (!u.active) {
        return ESP_ERR_INVALID_STATE;
    }
    while (len > 0) {
        if (u.have < LG_FWU_PREFIX_LEN) {
            size_t n = LG_FWU_PREFIX_LEN - u.have < len ? LG_FWU_PREFIX_LEN - u.have : len;
            memcpy(u.prefix + u.have, data, n);
            u.have += n;
            data += n;
            len -= n;
            if (u.have == LG_FWU_PREFIX_LEN) {
                esp_err_t err = check_prefix();
                if (err != ESP_OK) {
                    return err;
                }
            }
            continue;
        }
        size_t done = u.have - LG_FWU_PREFIX_LEN;
        if (done + len > u.info.image_len) {
            return refuse(ESP_ERR_INVALID_SIZE, "more arrived than the header said the image holds");
        }
        if (u.carry) {
            while (u.erased < done + len) {   /* a sector at a time, as it is reached */
                if (esp_partition_erase_range(u.target, u.erased, CARRY_SECTOR) != ESP_OK) {
                    return refuse(ESP_FAIL, "the spare slot could not be erased");
                }
                u.erased += CARRY_SECTOR;
            }
            if (esp_partition_write(u.target, done, data, len) != ESP_OK || lg_sha256_update(&u.hash, data, len) != 0) {
                return refuse(ESP_FAIL, "the image could not be written");
            }
        } else if (esp_ota_write(u.ota, data, len) != ESP_OK || lg_sha256_update(&u.hash, data, len) != 0) {
            return refuse(ESP_FAIL, "the image could not be written");
        }
        u.have += len;
        len = 0;
    }
    return ESP_OK;
}

esp_err_t lg_fwu_finish(void)
{
    if (!u.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!u.checked || u.have != LG_FWU_PREFIX_LEN + u.info.image_len) {
        return refuse(ESP_ERR_INVALID_SIZE, "the file stopped before the image was complete");
    }
    uint8_t digest[32];
    if (lg_sha256_finish(&u.hash, digest) != 0 || memcmp(digest, u.info.sha256, 32) != 0) {
        if (!u.carry) {
            (void)esp_ota_abort(u.ota);
        }
        u.checked = false;
        return refuse(ESP_ERR_INVALID_CRC, "the image is not the one that was signed");
    }
    if (u.carry) {
        /* Whole, and the one that was signed. Said to be carried only now, so a file that stopped
         * halfway is never handed to anybody. */
        nvs_handle_t ch;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &ch);
        if (err == ESP_OK) {
            err = nvs_set_blob(ch, "c_hdr", u.prefix, sizeof(u.prefix));
            if (err == ESP_OK) {
                err = nvs_set_str(ch, "c_slot", u.target->label);
            }
            if (err == ESP_OK) {
                err = nvs_commit(ch);
            }
            nvs_close(ch);
        }
        u.checked = false;
        if (err != ESP_OK) {
            return refuse(ESP_FAIL, "the handheld update could not be recorded as carried");
        }
        carried.known = false;
        ESP_LOGW(TAG, "[GRID] Handheld update %s (serial %u) is carried in %s and will be offered to handhelds",
                 u.info.version, (unsigned)u.info.serial, u.target->label);
        u.active = false;
        u.carry = false;
        u.carried_done = true;
        u.reason = "";
        return ESP_OK;
    }
    if (esp_ota_end(u.ota) != ESP_OK) {
        u.checked = false;
        return refuse(ESP_FAIL, "the image is not a valid application for this chip");
    }
    /* Recorded against the slot before it is made bootable, so the new firmware knows its own serial
     * the first time it runs, and a rolled-back one simply reads the other slot's. */
    nvs_handle_t h;
    char key[16];
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        slot_key(u.target, 's', key);
        (void)nvs_set_u32(h, key, u.info.serial);
        slot_key(u.target, 'v', key);
        (void)nvs_set_str(h, key, u.info.version);
        /* D101: the signed 192 bytes, so this device can hand the same update to another. */
        slot_key(u.target, 'p', key);
        (void)nvs_set_blob(h, key, u.prefix, sizeof(u.prefix));
        (void)nvs_commit(h);
        nvs_close(h);
    }
    if (esp_ota_set_boot_partition(u.target) != ESP_OK) {
        u.checked = false;
        return refuse(ESP_FAIL, "the new firmware could not be made the one that boots");
    }
    ESP_LOGW(TAG, "[GRID] Firmware update %s (serial %u) is in %s and boots next; it stays only if it comes up",
             u.info.version, (unsigned)u.info.serial, u.target->label);
    u.active = false;
    u.checked = false;
    u.reason = "";
    return ESP_OK;
}

bool lg_fwu_progress(size_t *received, size_t *total)
{
    if (received != NULL) {
        *received = u.have;
    }
    if (total != NULL) {
        *total = u.checked ? LG_FWU_PREFIX_LEN + u.info.image_len : 0;
    }
    return u.active;
}

bool lg_fwu_info(lg_fwu_info_t *out)
{
    if (!u.active || !u.checked || out == NULL) {
        return false;
    }
    *out = u.info;
    return true;
}

/* ---- the console: an update over the serial line ---- */

static char s_console_role = 'H';

static void restart_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static int cmd_update(int argc, char **argv)
{
    if (argc < 2) {
        char version[LG_FWU_VERSION_MAX + 1u];
        lg_fwu_running_version(version);
        const esp_partition_t *run = esp_ota_get_running_partition();
        printf("LGFWU: running serial %u version %s slot %s chip %u role %c%s\n", (unsigned)lg_fwu_running_serial(),
               version[0] != '\0' ? version : "(installed by cable)", run != NULL ? run->label : "?",
               lg_fwu_this_chip(), s_console_role, lg_gridkeys()->has_release ? "" : " (no release key: accepts no update)");
        return 0;
    }
    if (strcmp(argv[1], "begin") == 0) {
        lg_fwu_abort();
        esp_err_t err = lg_fwu_begin(s_console_role);
        printf("LGFWU: %s\n", err == ESP_OK ? "ready" : esp_err_to_name(err));
    } else if (strcmp(argv[1], "data") == 0 && argc >= 3) {
        static uint8_t raw[600];
        size_t n = lora_b64_decode(argv[2], strlen(argv[2]), raw, sizeof(raw));
        if (n == 0) {
            lg_fwu_abort();
            printf("LGFWU: refused: a line did not arrive whole\n");
            return 0;
        }
        esp_err_t err = lg_fwu_write(raw, n);
        size_t have = 0;
        (void)lg_fwu_progress(&have, NULL);
        if (err == ESP_OK) {
            printf("LGFWU: at %u\n", (unsigned)have);
        } else {
            printf("LGFWU: refused: %s\n", lg_fwu_reason()[0] != '\0' ? lg_fwu_reason() : esp_err_to_name(err));
        }
    } else if (strcmp(argv[1], "end") == 0) {
        esp_err_t err = lg_fwu_finish();
        if (err == ESP_OK && lg_fwu_finished_carried()) {
            printf("LGFWU: ok, carried for handhelds; this access point keeps running its own firmware\n");
        } else if (err == ESP_OK) {
            printf("LGFWU: ok, restarting into the new firmware\n");
            xTaskCreate(restart_soon, "lgfwu", 2048, NULL, 1, NULL);
        } else {
            printf("LGFWU: refused: %s\n", lg_fwu_reason()[0] != '\0' ? lg_fwu_reason() : esp_err_to_name(err));
        }
    } else {
        printf("LGFWU: usage: update | update begin | update data <base64> | update end\n");
    }
    return 0;
}

void lg_fwu_register_console(char role)
{
    s_console_role = role;
    const esp_console_cmd_t cmd = {
        .command = "update",
        .help = "update | update begin | update data <base64> | update end: firmware by signed update file (D97)",
        .func = cmd_update,
    };
    (void)esp_console_cmd_register(&cmd);
}
