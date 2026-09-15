#include "lg_identity.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    } else {
        memset(out, 0, sizeof(*out));
    }
    nvs_close(h);
    return ESP_OK;
}

void lg_identity_print(const lg_identity_t *id)
{
    if (id == NULL || !id->present) {
        printf("LGID: NONE\n");
        return;
    }
    if (id->has_node) {
        printf("LGID: %s role=%s board=%s node=%u %s\n", id->id, id->role, id->board, id->node_index, id->node_name);
    } else {
        printf("LGID: %s role=%s board=%s\n", id->id, id->role, id->board);
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
