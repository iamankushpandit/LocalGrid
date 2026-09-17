/*
 * lg_power.h - what a board can say about its power supply, over serial (D28).
 *
 * A board with a supply sense pin (a resistor divider into an ADC1 input) reports its supply
 * in millivolts. The Hosyond reads GPIO34 and the FNK0104B GPIO9, each behind a 2:1 divider
 * (pins and ratios from Braino's measured board profiles). Without a battery fitted that is
 * the charger output, which follows USB, so it shows a sagging hub. The Elegoo AP boards have
 * no such pin: they say so and report why they last restarted and how often they browned out.
 * Two resistors from 5 V to an ADC1 pin would make them measurable.
 *
 * Console command, the same on every firmware:
 *     power                     one reading (16 ADC samples averaged)
 *     power -m <s> [-i <ms>]    monitor for s seconds, one reading every ms (default 1000),
 *                               list each reading, then min, average, and max
 *     power -m <s> -q           monitor, summary only
 * Lines meant for tools start with "POWER:", "POWER_SAMPLE", or "POWER_RESULT".
 *
 * The divider ratio is the vendor's, not a meter's: check a reading against a multimeter
 * before trusting it to tenths of a volt.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* gpio < 0 means the board has no sense pin. divider_milli: 2000 for a 2:1 divider. */
esp_err_t lg_power_init(int gpio, uint32_t divider_milli);

bool lg_power_available(void);

/* One averaged reading. ESP_ERR_NOT_SUPPORTED when the board cannot measure. */
esp_err_t lg_power_read_mv(uint32_t *supply_mv, uint32_t *pin_mv);

/* Optional: extra lines printed after every `power` reply, such as an AP's restart counts. */
void lg_power_set_report_hook(void (*hook)(void));

/* Registers `power` with esp_console. Call after the console exists. */
esp_err_t lg_power_register_command(void);
