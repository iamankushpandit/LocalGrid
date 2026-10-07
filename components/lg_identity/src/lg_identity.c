#include "lg_identity.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lg_crypto.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#define PARTITION  "lgid"
#define NAMESPACE  "lgid"

/* Not "LGID": that prefix marks the identity line the bench tools read, and a log line wearing
 * it was parsed as a device ID (bench, 2026-09-20), so every board looked like the same one. */
static const char *TAG = "IDENTITY";

/* Must match tools/flash.py mint_id() exactly, byte for byte. */
#define ID_LABEL   "LocalGrid device id v1"
static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static lg_identity_check_t s_check = LG_IDENTITY_UNCHECKED;

lg_identity_check_t lg_identity_verified(void)
{
    return s_check;
}

const char *lg_identity_check_text(lg_identity_check_t check)
{
    switch (check) {
    case LG_IDENTITY_OK:       return "this board's own";
    case LG_IDENTITY_MISMATCH: return "another board's";
    default:                   return "not checked";
    }
}

static bool is_lower_hex(const char *s)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

esp_err_t lg_identity_compute_id(const char *role, const char *board, const uint8_t mac[6], uint32_t created,
                                 const char *nonce_hex, char *out, size_t out_len)
{
    if (role == NULL || board == NULL || mac == NULL || out == NULL || out_len < LG_IDENTITY_ID_MAX ||
        role[0] == '\0' || board[0] == '\0' || !is_lower_hex(nonce_hex)) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * The minted string, exactly as the provisioning tool builds it. The MAC lives in this buffer
     * and in the caller's array only, and is wiped below: decision D21 keeps hardware addresses
     * out of every log, screen, and stored record.
     */
    char text[128];
    int n = snprintf(text, sizeof(text), "%s|%s%s|%02x:%02x:%02x:%02x:%02x:%02x|%" PRIu32 "|%s",
                     ID_LABEL, role, board, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], created, nonce_hex);
    if (n <= 0 || (size_t)n >= sizeof(text)) {
        lg_secure_zero(text, sizeof(text));
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t digest[32];
    /* Idempotent, and needed here because the identity is read before firmware starts crypto. */
    (void)lg_crypto_init();
    int rc = lg_sha256((const uint8_t *)text, (size_t)n, digest);
    lg_secure_zero(text, sizeof(text));
    if (rc != 0) {
        return ESP_FAIL;
    }
    uint64_t value = 0;
    for (int i = 0; i < 7; i++) {
        value = (value << 8) | digest[i];
    }
    value >>= 6;                                     /* 50 bits, ten base32 characters */
    char tail[11];
    for (int i = 0; i < 10; i++) {
        tail[i] = CROCKFORD[(value >> (5 * (9 - i))) & 31u];
    }
    tail[10] = '\0';
    if ((size_t)snprintf(out, out_len, "LG-%s-%s-%s", role, board, tail) >= out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

lg_identity_check_t lg_identity_check(const lg_identity_t *id, const uint8_t mac[6])
{
    if (id == NULL || !id->present || mac == NULL || id->nonce[0] == '\0' || id->role[0] == '\0' ||
        id->board[0] == '\0') {
        return LG_IDENTITY_UNCHECKED;   /* provisioned before the nonce was stored: nothing to check */
    }
    char expect[LG_IDENTITY_ID_MAX];
    if (lg_identity_compute_id(id->role, id->board, mac, id->created, id->nonce, expect, sizeof(expect)) != ESP_OK) {
        return LG_IDENTITY_UNCHECKED;
    }
    return strcmp(expect, id->id) == 0 ? LG_IDENTITY_OK : LG_IDENTITY_MISMATCH;
}

/* Reads this board's factory MAC, checks the identity against it, and wipes it again (D21). */
static lg_identity_check_t check_against_this_board(const lg_identity_t *id)
{
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return LG_IDENTITY_UNCHECKED;
    }
    lg_identity_check_t check = lg_identity_check(id, mac);
    lg_secure_zero(mac, sizeof(mac));
    return check;
}

esp_err_t lg_identity_load(lg_identity_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return ESP_OK;   /* no identity partition, or it is blank */
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(out->id);
    if (nvs_get_str(h, "id", out->id, &len) == ESP_OK && strncmp(out->id, "LG-", 3) == 0) {
        out->present = true;
        len = sizeof(out->role);
        (void)nvs_get_str(h, "role", out->role, &len);
        len = sizeof(out->board);
        (void)nvs_get_str(h, "board", out->board, &len);
        (void)nvs_get_u32(h, "created", &out->created);
        if (nvs_get_u8(h, "node_idx", &out->node_index) == ESP_OK) {
            out->has_node = true;
            len = sizeof(out->node_name);
            if (nvs_get_str(h, "node_name", out->node_name, &len) != ESP_OK) {
                snprintf(out->node_name, sizeof(out->node_name), "NODE%u", out->node_index);
            }
        }
        if (nvs_get_u32(h, "device_idx", &out->device_index) == ESP_OK && out->device_index != 0) {
            out->has_device = true;
        }
        len = sizeof(out->nonce);
        if (nvs_get_str(h, "nonce", out->nonce, &len) != ESP_OK) {
            out->nonce[0] = '\0';
        }
    } else {
        memset(out, 0, sizeof(*out));
    }
    nvs_close(h);

    out->check = check_against_this_board(out);
    s_check = out->check;
    if (out->check == LG_IDENTITY_MISMATCH) {
        ESP_LOGE(TAG, "[GRID] This board is running another board's identity, %s. Two devices sharing an "
                      "identity break delivery, presence, and addressing, so this one will not join the grid.",
                 out->id);
        ESP_LOGE(TAG, "[GRID] Give this board its own identity: python tools/flash.py <name> --new-id");
    } else if (out->present && out->nonce[0] == '\0') {
        ESP_LOGI(TAG, "[GRID] Identity %s was provisioned before mint nonces were stored; it cannot be checked "
                      "against this board.", out->id);
    }
    return ESP_OK;
}

esp_err_t lg_identity_mint(const char *role, const char *board, uint32_t created, uint32_t index,
                           const char *name, char *id_out, size_t id_len)
{
    bool node = role != NULL && strcmp(role, "N") == 0;
    if (role == NULL || (!node && strcmp(role, "H") != 0) || board == NULL || strlen(board) != 3 ||
        created == 0 || id_out == NULL || id_len < LG_IDENTITY_ID_MAX ||
        (node ? index > 15u : (index < 1u || index > 32u))) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 3; i++) {
        char c = board[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (node && (name == NULL || name[0] == '\0' || strlen(name) >= LG_IDENTITY_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[16];
    char nonce[LG_IDENTITY_NONCE_MAX];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(nonce + i * 2u, 3, "%02x", raw[i]);
    }
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lg_identity_compute_id(role, board, mac, created, nonce, id_out, id_len);
    lg_secure_zero(mac, sizeof(mac));
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_flash_init_partition(PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase_partition(PARTITION);   /* blank, or laid out by an older tool */
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(PARTITION);
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t floor = 0;
    (void)nvs_get_u32(h, "boot", &floor);   /* D108: a new identity must not take the counter backwards */
    (void)nvs_erase_all(h);
    if (floor != 0) {
        (void)nvs_set_u32(h, "boot", floor);
    }
    err = nvs_set_str(h, "id", id_out);
    err = err == ESP_OK ? nvs_set_str(h, "role", role) : err;
    err = err == ESP_OK ? nvs_set_str(h, "board", board) : err;
    err = err == ESP_OK ? nvs_set_u32(h, "created", created) : err;
    err = err == ESP_OK ? nvs_set_str(h, "nonce", nonce) : err;
    if (node) {
        err = err == ESP_OK ? nvs_set_u8(h, "node_idx", (uint8_t)index) : err;
        err = err == ESP_OK ? nvs_set_str(h, "node_name", name) : err;
    } else {
        err = err == ESP_OK ? nvs_set_u32(h, "device_idx", index) : err;
    }
    err = err == ESP_OK ? nvs_commit(h) : err;
    nvs_close(h);
    return err;
}

uint32_t lg_identity_boot_floor(void)
{
    uint32_t floor = 0;
    if (nvs_flash_init_partition(PARTITION) != ESP_OK) {
        return 0;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, "boot", &floor);
        nvs_close(h);
    }
    return floor;
}

esp_err_t lg_identity_set_boot_floor(uint32_t boot)
{
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, "boot", boot);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static const char *s_console_role = "H";

static void mint_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static int cmd_identity(int argc, char **argv)
{
    if (argc < 5 || strcmp(argv[1], "new") != 0) {
        printf("LGIDNEW: usage: identity new <board> <unix seconds> <index> [name] [replace]\n");
        return 0;
    }
    bool replace = strcmp(argv[argc - 1], "replace") == 0;
    lg_identity_t have;
    if (lg_identity_load(&have) == ESP_OK && have.present && !replace) {
        printf("LGIDNEW: refused: this board is already %s; add the word replace to give it a new identity\n", have.id);
        return 0;
    }
    const char *name = (argc >= 6 && !(argc == 6 && replace)) ? argv[5] : "";
    char id[LG_IDENTITY_ID_MAX];
    esp_err_t err = lg_identity_mint(s_console_role, argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                                     (uint32_t)strtoul(argv[4], NULL, 10), name, id, sizeof(id));
    if (err != ESP_OK) {
        printf("LGIDNEW: refused: %s\n", err == ESP_ERR_INVALID_ARG
               ? "a three-character board code, the time in Unix seconds, and an index in range are needed"
               : esp_err_to_name(err));
        return 0;
    }
    printf("LGIDNEW: %s, restarting\n", id);
    xTaskCreate(mint_restart, "lgidnew", 2048, NULL, 1, NULL);
    return 0;
}

void lg_identity_register_console(const char *role)
{
    s_console_role = role != NULL && strcmp(role, "N") == 0 ? "N" : "H";
    const esp_console_cmd_t cmd = {
        .command = "identity",
        .help = "identity new <board> <unix seconds> <index> [name] [replace]: give this board its identity (D96)",
        .func = cmd_identity,
    };
    (void)esp_console_cmd_register(&cmd);
}

void lg_identity_print(const lg_identity_t *id)
{
    if (id == NULL || !id->present) {
        printf("LGID: NONE\n");
        return;
    }
    /* Nothing is added for an identity that checks out or cannot be checked: the line boards have
     * always printed is what tools/flash.py and the bench scripts read. */
    const char *warn = id->check == LG_IDENTITY_MISMATCH ? " identity=MISMATCH" : "";
    if (id->has_node) {
        printf("LGID: %s role=%s board=%s node=%u %s%s\n", id->id, id->role, id->board, id->node_index,
               id->node_name, warn);
    } else if (id->has_device) {
        printf("LGID: %s role=%s board=%s device=%" PRIu32 "%s\n", id->id, id->role, id->board, id->device_index,
               warn);
    } else {
        printf("LGID: %s role=%s board=%s%s\n", id->id, id->role, id->board, warn);
    }
    if (id->check == LG_IDENTITY_MISMATCH) {
        printf("This board is running another board's identity, so it will not join the grid.\n"
               "Give it its own: python tools/flash.py <name> --new-id\n");
    }
    fflush(stdout);
}

static void responder_task(void *arg)
{
    const lg_identity_t *id = arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = '\0';
        }
        if (strcmp(line, "id") == 0) {
            lg_identity_print(id);
        }
    }
}

esp_err_t lg_identity_start_responder(const lg_identity_t *id)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
#elif CONFIG_ESP_CONSOLE_UART
    esp_err_t err = uart_driver_install((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
    setvbuf(stdin, NULL, _IONBF, 0);
    return xTaskCreate(responder_task, "lg_id", 3072, (void *)id, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
