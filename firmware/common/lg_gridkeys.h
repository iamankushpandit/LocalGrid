/*
 * lg_gridkeys.h - the grid's keys, held by the device rather than compiled into the firmware (D96).
 *
 * A LocalGrid image used to carry its grid's Wi-Fi passphrase and keys, from the gitignored
 * lg_secrets.h, so an image could never be published: everyone who installed it would be on the same
 * grid. The keys now live in NVS, written once when the device is set up, and one image serves every
 * grid. A build that still has real keys in lg_secrets.h uses them when the device holds none, so the
 * bench carries on as before; a build made for publishing defines LG_SECRET_NONE and has nothing to
 * fall back on.
 *
 * What each kind of device holds:
 *   access point  the Wi-Fi passphrase and the backbone key; it derives the rest
 *   handheld      the Wi-Fi passphrase, the discriminator and the LoRa key - never the backbone key
 *   both          optionally the release public key, whose signature a firmware update must carry
 *
 * The setup bundle (what `provision <base64>` takes, and what the web flasher sends):
 *     0  4   "LGK1"
 *     4  1   flags: LG_GK_BACKBONE, LG_GK_DERIVED, LG_GK_RELEASE
 *     5  1   passphrase length, 8..63
 *     6  n   passphrase
 *        32  backbone key                      (LG_GK_BACKBONE: access points)
 *        4   discriminator, 32 LoRa key        (LG_GK_DERIVED: handhelds)
 *        65  release public key, P-256         (LG_GK_RELEASE)
 *        4   CRC-32 of everything before it, little-endian
 * The CRC guards against a line mangled on its way down a serial cable; it is not a signature. Whoever
 * holds the cable to an unset device is trusted, exactly as whoever flashes it is.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LG_GK_BACKBONE 0x01u
#define LG_GK_DERIVED  0x02u
#define LG_GK_RELEASE  0x04u
#define LG_GK_PASS_MIN 8u
#define LG_GK_PASS_MAX 63u
#define LG_GK_BUNDLE_MAX (6u + LG_GK_PASS_MAX + 32u + 36u + 65u + 4u)

typedef struct {
    bool    ready;          /* enough to join or serve a grid: a passphrase, the discriminator, the LoRa key */
    bool    has_backbone;
    bool    has_release;
    bool    from_build;     /* taken from lg_secrets.h, not from this device's own storage */
    char    wifi_pass[LG_GK_PASS_MAX + 1u];
    uint8_t backbone[32];
    uint8_t discriminator[4];
    uint8_t lora_hh[32];
    uint8_t release_pub[65];
} lg_gridkeys_t;

/* Reads the keys: the device's own first, then the build's. Call once, after NVS and lg_crypto are up.
 * Returns ready. `need_backbone` is true on an access point, which cannot serve without that key. */
bool lg_gridkeys_load(bool need_backbone);

/* The keys as loaded; never NULL, and all zeros with ready false before a successful load. */
const lg_gridkeys_t *lg_gridkeys(void);

/* Checks a setup bundle given as base64 and stores it. ESP_OK means stored: restart to use it.
 * ESP_ERR_INVALID_ARG for anything malformed, ESP_ERR_INVALID_CRC for a damaged one,
 * ESP_ERR_NOT_SUPPORTED when it lacks what this kind of device needs. */
esp_err_t lg_gridkeys_provision(const char *base64, bool need_backbone);

/* Forgets the device's own keys. The build's, if it has any, are used again after a restart. */
esp_err_t lg_gridkeys_erase(void);

/* Registers the `provision` console command, shared by both firmwares. Replies begin "LGPROV: " so a
 * tool or the web flasher can read them. */
void lg_gridkeys_register_console(bool need_backbone);
