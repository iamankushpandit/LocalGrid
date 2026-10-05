/* lg_gridkeys.c - see lg_gridkeys.h (D96). */
#include "lg_gridkeys.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_crypto.h"
#include "lg_lora_wire.h"
#include "lg_secrets.h"
#include "nvs.h"

static const char *TAG = "GRID";

#define NVS_NAMESPACE "lgkeys"
#define NVS_BUNDLE    "bundle"

static lg_gridkeys_t s_keys;
static bool s_need_backbone;

const lg_gridkeys_t *lg_gridkeys(void)
{
    return &s_keys;
}

static bool derive(lg_gridkeys_t *k)
{
    return lg_hkdf_sha256(NULL, 0, k->backbone, sizeof(k->backbone), (const uint8_t *)"lg-disc", 7,
                          k->discriminator, sizeof(k->discriminator)) == 0 &&
           lg_hkdf_sha256(NULL, 0, k->backbone, sizeof(k->backbone), (const uint8_t *)"lg-lora-hh", 10,
                          k->lora_hh, sizeof(k->lora_hh)) == 0;
}

/* Parses a bundle into `out`. The same code checks one before it is stored and reads one back. */
static esp_err_t parse(const uint8_t *b, size_t len, bool need_backbone, lg_gridkeys_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 6u + LG_GK_PASS_MIN + 4u || len > LG_GK_BUNDLE_MAX || memcmp(b, "LGK1", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t flags = b[4];
    size_t pass = b[5];
    if ((flags & (uint8_t)~(LG_GK_BACKBONE | LG_GK_DERIVED | LG_GK_RELEASE)) != 0 || pass < LG_GK_PASS_MIN ||
        pass > LG_GK_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t want = 6u + pass + ((flags & LG_GK_BACKBONE) ? 32u : 0u) + ((flags & LG_GK_DERIVED) ? 36u : 0u) +
                  ((flags & LG_GK_RELEASE) ? 65u : 0u) + 4u;
    if (len != want) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t crc = (uint32_t)b[len - 4] | ((uint32_t)b[len - 3] << 8) | ((uint32_t)b[len - 2] << 16) |
                   ((uint32_t)b[len - 1] << 24);
    if (esp_rom_crc32_le(0, b, len - 4u) != crc) {
        return ESP_ERR_INVALID_CRC;
    }
    const uint8_t *p = b + 6;
    for (size_t i = 0; i < pass; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) {
            return ESP_ERR_INVALID_ARG;   /* WPA2 passphrases are printable ASCII */
        }
    }
    memcpy(out->wifi_pass, p, pass);
    p += pass;
    if (flags & LG_GK_BACKBONE) {
        memcpy(out->backbone, p, 32);
        p += 32;
        out->has_backbone = true;
    }
    if (flags & LG_GK_DERIVED) {
        memcpy(out->discriminator, p, 4);
        memcpy(out->lora_hh, p + 4, 32);
        p += 36;
    }
    if (flags & LG_GK_RELEASE) {
        if (p[0] != 0x04) {
            return ESP_ERR_INVALID_ARG;   /* an uncompressed P-256 point, as lg_ecdsa_p256_verify takes */
        }
        memcpy(out->release_pub, p, 65);
        out->has_release = true;
    }
    if (need_backbone) {
        /* An access point must be given the backbone key, and works the others out from it, so a
         * bundle cannot hand it a discriminator that disagrees with its own key. */
        if (!out->has_backbone || !derive(out)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        /* A handheld is never given the backbone key (D77): a bundle that carries it is refused
         * rather than stored, so the key cannot end up on a device that walks about. */
        if (out->has_backbone || !(flags & LG_GK_DERIVED)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    out->ready = true;
    return ESP_OK;
}

static bool load_stored(bool need_backbone)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t b[LG_GK_BUNDLE_MAX];
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(h, NVS_BUNDLE, b, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    lg_gridkeys_t k;
    if (parse(b, len, need_backbone, &k) != ESP_OK) {
        ESP_LOGE(TAG, "[GRID] The grid keys stored on this device do not read back; set it up again");
        return false;
    }
    s_keys = k;
    return true;
}

#ifndef LG_SECRET_NONE
static bool load_built(bool need_backbone)
{
    static const char pass[] = LG_SECRET_WIFI_PASSPHRASE;
    static const uint8_t disc[4] = LG_SECRET_DISCRIMINATOR;
    static const uint8_t lhh[32] = LG_SECRET_LORA_HH_KEY;
    lg_gridkeys_t k;
    memset(&k, 0, sizeof(k));
    if (sizeof(pass) - 1u < LG_GK_PASS_MIN || sizeof(pass) - 1u > LG_GK_PASS_MAX) {
        return false;
    }
    memcpy(k.wifi_pass, pass, sizeof(pass) - 1u);
    memcpy(k.discriminator, disc, sizeof(disc));
    memcpy(k.lora_hh, lhh, sizeof(lhh));
#if LG_GRIDKEYS_WITH_BACKBONE
    static const uint8_t bb[32] = LG_SECRET_BACKBONE_KEY;
    memcpy(k.backbone, bb, sizeof(bb));
    k.has_backbone = true;
    if (!derive(&k)) {
        return false;
    }
#endif
    static const uint8_t zero[4] = { 0 };
    if (memcmp(k.discriminator, zero, sizeof(zero)) == 0 || (need_backbone && !k.has_backbone)) {
        return false;   /* the example header, or a handheld image asked to be an access point */
    }
    k.ready = true;
    k.from_build = true;
    s_keys = k;
    return true;
}
#endif

bool lg_gridkeys_load(bool need_backbone)
{
    s_need_backbone = need_backbone;
    memset(&s_keys, 0, sizeof(s_keys));
    if (load_stored(need_backbone)) {
        ESP_LOGI(TAG, "[GRID] Grid keys: this device's own%s", s_keys.has_release ? ", with a release key" : "");
        return true;
    }
#ifndef LG_SECRET_NONE
    if (load_built(need_backbone)) {
        ESP_LOGI(TAG, "[GRID] Grid keys: from the build (this device has not been given its own)");
        return true;
    }
#endif
    ESP_LOGW(TAG, "[GRID] Not set up: this device has no grid keys. Give it some with `provision`.");
    return false;
}

esp_err_t lg_gridkeys_provision(const char *base64, bool need_backbone)
{
    if (base64 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[LG_GK_BUNDLE_MAX];
    size_t len = lora_b64_decode(base64, strlen(base64), b, sizeof(b));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lg_gridkeys_t k;
    esp_err_t err = parse(b, len, need_backbone, &k);
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_BUNDLE, b, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    memset(b, 0, sizeof(b));
    memset(&k, 0, sizeof(k));
    return err;
}

esp_err_t lg_gridkeys_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_BUNDLE);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void restart_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));   /* long enough for the reply to leave the UART */
    esp_restart();
}

/* provision | provision <base64 bundle> | provision erase. Never prints a key. */
static int cmd_provision(int argc, char **argv)
{
    if (argc < 2) {
        const lg_gridkeys_t *k = lg_gridkeys();
        printf("LGPROV: %s%s%s\n", !k->ready ? "unset" : k->from_build ? "build" : "set",
               k->has_backbone ? " backbone" : "", k->has_release ? " release" : "");
        return 0;
    }
    if (strcmp(argv[1], "erase") == 0) {
        esp_err_t err = lg_gridkeys_erase();
        printf("LGPROV: %s\n", err == ESP_OK ? "erased, restarting" : esp_err_to_name(err));
        if (err == ESP_OK) {
            xTaskCreate(restart_soon, "lgprov", 2048, NULL, 1, NULL);
        }
        return 0;
    }
    esp_err_t err = lg_gridkeys_provision(argv[1], s_need_backbone);
    if (err == ESP_OK) {
        printf("LGPROV: ok, restarting\n");
        xTaskCreate(restart_soon, "lgprov", 2048, NULL, 1, NULL);
    } else {
        printf("LGPROV: refused: %s\n",
               err == ESP_ERR_INVALID_CRC ? "damaged on the way (checksum)"
               : err == ESP_ERR_NOT_SUPPORTED ? (s_need_backbone ? "an access point needs the backbone key"
                                                                 : "a handheld takes the derived keys, never the backbone key")
               : err == ESP_ERR_INVALID_ARG ? "not a setup bundle" : esp_err_to_name(err));
    }
    return 0;
}

void lg_gridkeys_register_console(bool need_backbone)
{
    s_need_backbone = need_backbone;
    const esp_console_cmd_t cmd = {
        .command = "provision",
        .help = "provision | provision <bundle> | provision erase: this device's grid keys (D96); keys are never shown",
        .func = cmd_provision,
    };
    (void)esp_console_cmd_register(&cmd);
}
