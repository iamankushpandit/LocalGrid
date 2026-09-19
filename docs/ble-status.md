# BLE status beacon (D68)

Every AP tells a nearby laptop how the grid is doing, over BLE, without anyone joining the grid's
Wi-Fi. The laptop only listens (`tools/grid_watch.py`); it never transmits into the grid (D25).
The status is sealed with a key derived from the grid's secrets, so only a laptop holding this
grid's `lg_secrets.h` can read it, and nobody can forge an "all fine".

## Where it travels

Each AP already advertises the discovery payload (D4) as manufacturer data under
`LG_BLE_COMPANY_ID`. The status goes in the **scan response**, also manufacturer data under
`LG_BLE_COMPANY_ID`, so the discovery advert is unchanged. A scanner in active mode (the default
for Windows / bleak) receives both. The AP replaces the scan response with the next frame of the
rotation every advertising interval (500 ms).

## Key

    K = HKDF-SHA256(salt = "LG-BLE-STATUS-1", ikm = LG_SECRET_BACKBONE_KEY (32 bytes),
                    info = "ble status", L = 32)

## Frame (scan-response manufacturer payload after the 2-byte company ID, at most 27 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | magic `0x53` ('S') |
| 1 | u8 | low nibble: AP index; high nibble: frame type |
| 2 | u32 LE | AP boot counter |
| 6 | u24 LE | frame counter, per AP boot, +1 every frame of any type |
| 9 | n (<= 14) | ciphertext |
| 9 + n | 4 | the first 4 bytes of the Poly1305 tag |

Sealed with ChaCha20-Poly1305 (`lg_aead_seal`), then the 16-byte tag is cut to its first 4 bytes.

- **Nonce (12 bytes):** `ap (u8), 0x00, boot (u32 LE), counter (u24 LE), 0x00 0x00 0x00`. The
  counter never repeats within a boot, and a new boot is a new nonce space; 2^24 frames at 2 a
  second last 97 days of uptime. At the wrap the AP stops sending status until it restarts,
  rather than reusing a nonce.
- **AAD:** bytes 0 to 8 of the frame, the 9-byte header.
- **The receiver** decrypts with ChaCha20 (counter 1) and checks the tag by sealing the recovered
  plaintext again with the same nonce and AAD and comparing the first 4 bytes. A mismatch drops
  the frame. It also drops a counter at or below the last one accepted from that AP and boot
  (replay).

## Frame types (plaintext, little-endian)

**1: AP health** (10 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | uptime, minutes (saturates) |
| 2 | u8 | backbone links up: bit n = link to AP n |
| 3 | u8 | handhelds registered here |
| 4 | u8 | time: bits 0-1 quality (0 unset, 1 carried, 2 authoritative), bit 2 from GPS, bits 3-7 stratum (31 = unknown or more) |
| 5 | u8 | lowest free heap since boot, KB (saturates at 255) |
| 6 | u8 | last reset reason (`esp_reset_reason_t`) |
| 7 | u8 | restarts since first boot, low 8 bits |
| 8 | u8 | brownouts since first boot, low 8 bits |
| 9 | u8 | GPS: bit 0 fitted, bit 1 fix, bits 2-7 satellites in use (saturates at 63) |

**2: Handhelds** (14 bytes): who is online and where, from this AP's presence table.

| Offset | Size | Field |
|---|---|---|
| 0 | u32 | online: bit i = roster device i + 1 is online |
| 4 | 10 | the AP index of devices 1 to 20, one nibble each (device 1 in the low nibble of byte 4); 0xF = not online or unknown |

**3: Alert** (10 bytes): the newest urgent broadcast this AP has carried.

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | bit 0 an urgent broadcast is active (carried in the last 15 minutes and not followed by an all clear, `LG_FLAG_ALL_CLEAR`, from the same author); bit 1 the newest urgent is an all clear |
| 1 | u8 | author device (0 none) |
| 2 | u16 | seconds since it was carried (saturates) |
| 4 | u8 | read reports seen for it by this AP (255 unknown) |
| 5 | u8 | reserved 0 |
| 6 | u32 | this AP's grid time, Unix seconds (0 unset) |

**4: Batteries** (up to 14 bytes): the handhelds registered on this AP.

Up to 7 pairs of `device (u8), percent (u8; 255 = no battery sense)`; a laptop merges them across
APs. Handhelds report their battery to their AP (see the firmware notes).

**5: Name** (up to 14 bytes): one handheld's chosen name (D50) per frame, rotating through the
devices this AP knows a name for: `device (u8)`, then up to 13 bytes of UTF-8, cut at a character
boundary. The laptop falls back to "Handheld N" until it has heard one. An AP that knows no
chosen name sends AP health (type 1) in type 5's turn, so a type 5 frame always has a device byte.

## Firmware notes

- The frame is built and sealed on the AP's core task, which owns every source it reads; the
  NimBLE host task only copies the sealed bytes into the scan response (`firmware/node/main/ble_status.c`).
- Batteries: each handheld's keepalive PING carries one byte, its battery in percent (0 to 100,
  255 no battery sense). APs before D68 ignore it; an AP since D68 takes an empty PING from an older
  handheld as 255. The AP keeps the byte in RAM for the handhelds registered on it.
- Read reports: an AP counts, by reader, the `LG_ACK_READ` reports for the newest urgent broadcast
  that pass through it. Reports travel towards the author's AP only, so an AP off that path shows
  fewer (or 0) than the author's AP.
- Restarts are the AP's boot counter minus one; brownouts are its `rr_brownout` NVS count. Both
  reset with an NVS erase.
- A type 4 frame with no handhelds registered carries no pairs (0 bytes of plaintext).

## Rotation

Type 1 every other frame; types 2, 3, 4 and 5 take turns in between, so every type arrives
within 4 s and AP health every second.

## Privacy

Everything above is sealed. A stranger's scanner sees the discovery payload (as today) and a
scan response of random-looking bytes that changes every 500 ms. Positions (D65) are never in the
beacon.
