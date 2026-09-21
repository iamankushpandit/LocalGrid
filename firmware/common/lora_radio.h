/*
 * lora_radio.h - the RYLR998's settings and the address plan, in one place (D71).
 *
 * An AP and a handheld must agree on every number here or they never hear each other, so both
 * firmwares read them from this file rather than each keeping a copy. What differs between them -
 * pins, buffers, timings, what is worth airtime - stays in their own drivers.
 */
#pragma once

#include <stdint.h>

/*
 * Band. 868.5 MHz is the European licence-free LoRa band; a 915 MHz region must change this one
 * number (and only this one) before flashing.
 */
#define LORA_BAND_HZ        868500000u
#define LORA_UART_BAUD      115200        /* the RYLR998 leaves the factory at this rate */
/* AT+PARAMETER: spreading factor 9, bandwidth 125 kHz (code 7), coding rate 4/5 (code 1),
 * preamble 12. docs/lora.md names SF9/BW125; preamble 12 is the module's own default and is
 * required when the network ID is left at 18. */
#define LORA_SF             9
#define LORA_BW_CODE        7
#define LORA_CR_CODE        1
#define LORA_PREAMBLE       12
#define LORA_POWER_DBM      22            /* AT+CRFOP, 0..22 */

/*
 * The address plan (docs/lora.md, "Ready for handhelds later"): 1-16 are APs, by AP index, and
 * 100 + device number are handhelds. The RYLR998's AT+SEND to address 0 reaches every address on
 * the network ID, so it is the broadcast address and nothing else: an AP is 1 + its index.
 * Measured on the bench, 2026-09-20: with MAIN sitting on address 0 as well, NORTH and SOUTH
 * linked to each other but MAIN's link kept timing out, because a module cannot be both a
 * broadcast target and a private one. Keeping 0 free fixed it.
 */
#define LORA_ADDR_BROADCAST 0u
#define LORA_ADDR_AP(i)     ((uint16_t)((i) + 1u))
#define LORA_ADDR_HANDHELD(d) ((uint16_t)(100u + (d)))

/*
 * AT+NETWORKID takes 3..15 (18 is its default). One byte of the grid's discriminator picks one, so
 * another LocalGrid built from other secrets is ignored by the module itself, before either
 * firmware looks at a byte. Handhelds hold the discriminator (never the backbone key), so both
 * sides work it out the same way.
 */
#define LORA_NETWORK_ID(disc0)  ((uint8_t)(3u + ((disc0) % 13u)))
