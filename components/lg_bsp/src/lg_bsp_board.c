/*
 * Board probe (D66). Waveshare's 1.47in ESP32-C6 comes with and without touch, the two are wired
 * differently, and nothing on the outside says which one is on the bench. The Touch board has an
 * IMU and a touch controller on I2C; the plain board has nothing on those pins. A short probe at
 * boot settles it, so a unit minted with the wrong board code still gets the right pins.
 */
#include "lg_bsp_board.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "BSP";

#define PROBE_TIMEOUT_MS 20

static bool has_probe(const lg_board_t *b)
{
    return b != NULL && b->probe.sda >= 0 && b->probe.scl >= 0 && (b->probe.addr[0] != 0 || b->probe.addr[1] != 0);
}

/* True when a device answers at one of the probe's addresses. The bus exists only for the probe. */
static bool probe_answers(const lg_board_t *b, uint8_t *found)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)b->probe.sda,
        .scl_io_num = (gpio_num_t)b->probe.scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        return false;
    }
    bool answered = false;
    for (int i = 0; i < 2 && !answered; i++) {
        uint8_t addr = b->probe.addr[i];
        if (addr != 0 && i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
            answered = true;
            *found = addr;
        }
    }
    i2c_del_master_bus(bus);
    return answered;
}

const lg_board_t *lg_bsp_board_resolve(const lg_board_t *named)
{
    if (named == NULL || named->sibling == NULL) {
        return named;
    }
    const lg_board_t *sib = lg_board_find(named->sibling);
    if (sib == NULL) {
        return named;
    }
    uint8_t found = 0;
    if (has_probe(named)) {
        if (probe_answers(named, &found)) {
            ESP_LOGI(TAG, "[BSP] Board probe: 0x%02X answers on GPIO%d/%d, so this is the %s (%s)", found,
                     named->probe.sda, named->probe.scl, named->name, named->code);
            return named;
        }
        ESP_LOGW(TAG, "[BSP] Board probe: nothing on GPIO%d/%d, so this is not the %s but the %s; using %s. "
                 "Re-mint the device ID as %s so it says what the board is.", named->probe.sda, named->probe.scl,
                 named->name, sib->name, sib->code, sib->code);
        return sib;
    }
    if (has_probe(sib)) {
        if (probe_answers(sib, &found)) {
            ESP_LOGW(TAG, "[BSP] Board probe: 0x%02X answers on GPIO%d/%d, so this is the %s, not the %s; using %s. "
                     "Re-mint the device ID as %s so it says what the board is.", found, sib->probe.sda,
                     sib->probe.scl, sib->name, named->name, sib->code, sib->code);
            return sib;
        }
        ESP_LOGI(TAG, "[BSP] Board probe: nothing on GPIO%d/%d, so this is the %s (%s)", sib->probe.sda,
                 sib->probe.scl, named->name, named->code);
    }
    return named;
}
