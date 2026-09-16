/*
 * lg_proto_config.h - hardcoded prototype grid settings shared by node and handheld firmware.
 *
 * Phase 1 replaces all of this with master-issued configuration and pairing.
 * Nothing here is secret; keys live in the gitignored lg_secrets.h.
 * Node index and name come from the board's identity partition (lg_identity.h),
 * written by tools/flash.py, never from hardware addresses in source.
 */
#pragma once

#include <stdint.h>

#define LG_PROTO_CHANNEL        6       /* grid-wide Wi-Fi and ESP-NOW channel */
#define LG_PROTO_TCP_PORT       7300    /* handheld control sessions */
#define LG_PROTO_SSID_PREFIX    "LG-"
#define LG_PROTO_MAX_STATIONS   15      /* ESP32 SoftAP maximum with ESP-NOW encryption disabled */
#define LG_PROTO_UNKNOWN_NODE   7       /* index used by a node board that was never provisioned */

/* Discovery payload carried in beacons (vendor IE) and BLE adverts. */
#define LG_DISC_MAGIC0          'L'
#define LG_DISC_MAGIC1          'G'
#define LG_DISC_VERSION         1
#define LG_DISC_LEN             12
#define LG_DISC_FLAG_BACKBONE   0x01    /* node has at least one working backbone link */
#define LG_VENDOR_OUI           { 0x4C, 0x47, 0x00 }   /* prototype placeholder, not a registered OUI */
#define LG_VENDOR_OUI_TYPE      0x01
#define LG_BLE_COMPANY_ID       0xFFFF  /* Bluetooth SIG "testing" company ID */

/* Deterministic addressing: node n owns 192.168.(4 + n).0/24 and sits at .1. */
static inline void lg_proto_node_ip(uint16_t index, uint8_t out[4])
{
    out[0] = 192;
    out[1] = 168;
    out[2] = (uint8_t)(4u + index);
    out[3] = 1;
}

/* Handhelds use static addresses on their node's subnet: .100 + device index (answer 10).
 * Node DHCP serves .2 to .99 only, for phones. */
#define LG_PROTO_HANDHELD_HOST_BASE 100u
static inline void lg_proto_handheld_ip(uint16_t node, uint32_t device, uint8_t out[4])
{
    lg_proto_node_ip(node, out);
    out[3] = (uint8_t)(LG_PROTO_HANDHELD_HOST_BASE + device);
}
