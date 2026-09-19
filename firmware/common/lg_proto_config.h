/*
 * lg_proto_config.h - hardcoded prototype grid settings shared by node and handheld firmware.
 *
 * Phase 1 replaces all of this with admin-issued configuration and pairing.
 * Nothing here is secret; keys live in the gitignored lg_secrets.h.
 * Node index and name come from the board's identity partition (lg_identity.h),
 * written by tools/flash.py, never from hardware addresses in source.
 */
#pragma once

#include <stdint.h>

#define LG_PROTO_CHANNEL        6       /* grid-wide Wi-Fi and ESP-NOW channel */
#define LG_PROTO_TCP_PORT       7300    /* handheld control sessions */
/* D46: one network. Every AP broadcasts this SSID; handhelds pick an AP by BSSID and the vendor IE. */
#define LG_PROTO_SSID           "LocalGrid Access Point"
#define LG_PROTO_MAX_STATIONS   15      /* ESP32 SoftAP maximum with ESP-NOW encryption disabled */
#define LG_PROTO_UNKNOWN_NODE   7       /* index used by a node board that was never provisioned */

/* Discovery payload carried in beacons (vendor IE) and BLE adverts.
 * Fixed part, LG_DISC_LEN bytes: magic 'L' 'G', version, 4-byte grid discriminator, AP index,
 * reserved (was the master flag; always 0 since D45), free station slots, flags, attached handhelds.
 * Wi-Fi only (D46): a trailing u8 name length (0..LG_DISC_NAME_MAX) and that many bytes of the
 * AP name, because every AP now shares one SSID. Receivers that only know the fixed part ignore
 * the tail, so the version stays 1. BLE adverts carry the fixed part only (31-byte legacy limit). */
#define LG_DISC_MAGIC0          'L'
#define LG_DISC_MAGIC1          'G'
#define LG_DISC_VERSION         1
#define LG_DISC_LEN             12
#define LG_DISC_NAME_MAX        15
#define LG_DISC_WIFI_MAX        (LG_DISC_LEN + 1 + LG_DISC_NAME_MAX)
#define LG_DISC_FLAG_BACKBONE   0x01    /* node has at least one working backbone link */
#define LG_DISC_FLAG_TIME       0x02    /* the AP holds grid time; handhelds prefer such an AP (D6) */
#define LG_VENDOR_OUI           { 0x4C, 0x47, 0x00 }   /* prototype placeholder, not a registered OUI */
#define LG_VENDOR_OUI_TYPE      0x01
#define LG_BLE_COMPANY_ID       0xFFFF  /* Bluetooth SIG "testing" company ID */

/* D46: every AP shares 192.168.4.0/24 and answers as 192.168.4.1, so a phone that moves between
 * APs keeps a working gateway and admin page. The index is kept in the signature for callers and
 * ignored. ESP32 SoftAPs cannot bridge one Layer 2 network; identical addressing stands in for it. */
static inline void lg_proto_node_ip(uint16_t index, uint8_t out[4])
{
    (void)index;
    out[0] = 192;
    out[1] = 168;
    out[2] = 4;
    out[3] = 1;
}

/* Handhelds use static addresses: 192.168.4.(100 + device index) (answer 10), the same on every AP. */
#define LG_PROTO_HANDHELD_HOST_BASE 100u
static inline void lg_proto_handheld_ip(uint16_t node, uint32_t device, uint8_t out[4])
{
    lg_proto_node_ip(node, out);
    out[3] = (uint8_t)(LG_PROTO_HANDHELD_HOST_BASE + device);
}

/* Phone DHCP pools (D46). All APs share one subnet, so each AP leases only from its own block
 * and two APs never hand out the same address. Blocks cover .2 to .97, below the handheld
 * addresses, one block per possible AP index (0 .. LG_MAX_NODES - 1, which includes
 * LG_PROTO_UNKNOWN_NODE). 96 addresses over 8 indices gives 12 each: fewer than
 * LG_PROTO_MAX_STATIONS, which is shared with handhelds, whose addresses are static. */
#define LG_PROTO_DHCP_FIRST_HOST    2u
#define LG_PROTO_DHCP_BLOCK         12u
#define LG_PROTO_DHCP_INDICES       8u      /* must equal LG_MAX_NODES; checked where lg_types.h is visible */
_Static_assert(LG_PROTO_DHCP_FIRST_HOST + LG_PROTO_DHCP_INDICES * LG_PROTO_DHCP_BLOCK - 1u < 98u,
               "AP DHCP blocks must stay within .2 to .97");
_Static_assert(LG_PROTO_UNKNOWN_NODE < LG_PROTO_DHCP_INDICES, "the unprovisioned AP index needs a DHCP block");

/* First and last host of AP index's phone pool; out-of-range indices share the last block. */
static inline void lg_proto_dhcp_block(uint16_t index, uint8_t *first, uint8_t *last)
{
    uint16_t i = index < LG_PROTO_DHCP_INDICES ? index : (uint16_t)(LG_PROTO_DHCP_INDICES - 1u);
    *first = (uint8_t)(LG_PROTO_DHCP_FIRST_HOST + i * LG_PROTO_DHCP_BLOCK);
    *last = (uint8_t)(*first + LG_PROTO_DHCP_BLOCK - 1u);
}
