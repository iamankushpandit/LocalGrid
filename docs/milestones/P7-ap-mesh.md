# Prototype milestone 7: APs as one mesh network

Status: **built with zero warnings; MAIN verified alone on hardware.** NORTH and SOUTH are unplugged while the owner sorts out bench power (the USB hub browned out all three APs; MAIN's `status` has recorded 269 brownout restarts), so nothing that needs two APs has run on the boards yet. The handheld service change is built for both chips but not flashed.

This milestone covers three owner decisions and one robustness change:

- **D45, no master AP.** Every AP serves the admin page and holds a replicated copy of the admin settings. Grid time can be set on any AP.
- **D46, one network.** Every AP broadcasts the SSID "LocalMesh Access Point" and answers as 192.168.4.1. Handhelds choose an AP by BSSID and label it from the vendor IE.
- **Backbone link robustness.** A few lost HELLO broadcasts no longer drop a link, and `nodes` prints counters that show how often HELLOs go missing.
- **A/B build switches** for BLE advertising and PMF.

## What changed

| Area | Change |
|---|---|
| `lg_core` | `LG_T_GRID_STATE` (0x51), AP to AP only, flooded, body opaque to the core, 1 to `LG_GRID_STATE_MAX` bytes. |
| `firmware/node/main/grid_state.c` | Replicated settings with (seq, author) newest-wins. Time generations: an AP holding an older AUTHORITATIVE time is demoted to CARRIED. First-time setup waits until the AP has heard another AP's grid state, if it has links. |
| `settings.c` | Record layout version 2 (`seq`, `author`). A version 1 record is read as version 1 made on AP 0, so MAIN's admin password survives. |
| `firmware/common/lg_proto_config.h` | `LG_PROTO_SSID` "LocalMesh Access Point". `lg_proto_node_ip()` always gives 192.168.4.1. Handhelds keep 192.168.4.(100 + device). Phone DHCP blocks of 12 addresses per AP index from .2 to .97, with compile-time checks. The vendor IE gains a name tail: u8 length, then up to 15 bytes. |
| `node_main.c` | Shared SSID, per-AP DHCP block, name in the Wi-Fi vendor IE (BLE keeps the 12-byte payload), `config` shows the network, DHCP block, BLE and PMF state. |
| `backbone.c` | Unicast keepalive probes with MAC-level ACK. Per-AP counters that survive link loss. |
| `ble_adv.c` | Advertising data is updated in place, and only when it changed. It used to stop and restart advertising on every update. |
| `main/Kconfig.projbuild` | `CONFIG_LG_NODE_BLE_ADV` and `CONFIG_LG_NODE_PMF_CAPABLE`, both default `y`. |
| `firmware/handheld/main/service/hh_service.c` | Joins with the shared SSID and the chosen BSSID. The candidate's shown name comes from the IE tail, or "AP n" from an AP without one. The 16-entry static scan record array is gone (about 1.3 KB of internal RAM), because candidates come from the IE alone. |

### Address plan (D46)

| What | Address |
|---|---|
| Every AP | 192.168.4.1/24 |
| Handheld with device index d | 192.168.4.(100 + d), static, the same on every AP |
| Phones on AP 0 (MAIN) | .2 to .13 |
| Phones on AP 1 (NORTH) | .14 to .25 |
| Phones on AP 2 (SOUTH) | .26 to .37 |
| Phones on AP n | .(2 + 12n) to .(13 + 12n), up to AP 7 (.86 to .97) |

Twelve phones per AP is below the 15-station SoftAP limit. The limit is shared with handhelds, and handhelds use static addresses, so the gap is deliberate. A phone that roams asks the new AP for a lease and gets an address from that AP's block. Its old lease stays reserved on the old AP until it expires.

### Backbone link rule

- HELLOs are broadcast every 2 s, with no ACK and no retries.
- Once a confirmed link has gone 3 s without a HELLO, this AP sends its HELLO to that neighbour as a unicast, once a second.
- A unicast is retried and ACKed by the neighbour's radio. The neighbour processes it as a HELLO, which also repairs the other direction.
- The link is dropped only after 6 s with neither a HELLO nor an ACK, or after 20 s with no HELLO at all, because an ACK proves the radio but not the firmware.
- Unconfirmed (ONE-WAY) links keep the plain 6 s timeout.

## Build

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
python tools/build.py --firmware node
python tools/build.py --firmware tests --target esp32
python tools/build.py --firmware handheld        # both esp32 and esp32s3
python tools/check_layers.py
```

## Flash

```powershell
python tools/flash.py node-main --firmware tests   # on-board tests; then put the AP firmware back:
python tools/flash.py --role N                     # APs one at a time, 10 s apart, settings kept
python tools/flash.py --role H                     # handhelds
```

APs and handhelds must be updated together. A handheld built before D46 can join an updated MAIN, because AP 0's addresses did not change. It cannot use an updated NORTH or SOUTH, because it still expects 192.168.5.1 or 192.168.6.1 there. A handheld built after D46 cannot use an older NORTH or SOUTH, for the same reason in reverse.

## Expected serial output

Boot of any AP (MAIN shown, captured 2026-09-16):

```
[GRID] LocalGrid node 0 MAIN starting, boot 364
[GRID] Last restart: power-on or reset
[NET] SoftAP "LocalMesh Access Point" (AP 0 MAIN) on channel 6 at 192.168.4.1, phone DHCP .2 to .13, PMF capable
[GRID] Settings version 1 from AP 0 (set up)
[BB] ESP-NOW v2 on SoftAP interface, node 0 boot 364
[NET] Listening for handhelds on TCP 7300
[WEB] Admin page at http://192.168.4.1/ (configured)
[BLE] Advertising LocalGrid discovery every 500 ms
```

`config`:

```
AP configuration
  id: LG-N-ELG-1ME3HN5EDP
  AP: index 0, name MAIN (every AP is equal; there is no master, D45)
  network: "LocalMesh Access Point" on every AP (D46), channel 6, address 192.168.4.1 on every AP, up to 15 stations
  DHCP for phones on this AP: 192.168.4.2 to 192.168.4.13; handhelds are static at .100 plus device index
  BLE advertising: on; PMF: capable
  grid name: LocalGrid
  ...
  settings version 1, made on AP 0; copied to every AP over the backbone
```

`nodes`, one row per AP ever heard since boot:

```
Backbone links (AP 0):
  AP    STATE     RSSI  AGE_MS  HELLOS  GAPS  MAX_GAP  PROBES  ACKS  SAVES  UPS  LOSSES
  gap = HELLO more than 4000 ms after the previous; probe = unicast keepalive; save = link kept by ACKs
  tx 13 tx_fail 0 dropped 0 nomem 0 | rx 0 auth_fail 0 replay 0 rx_queue_full 0 | queued 0
```

Link events: `[BB] No HELLO from node N for M ms, but it ACKs keepalives; link kept`, and `[BB] Link lost to node N (no HELLO for M ms, no keepalive ACK either)`.

## Test procedure

### Done on MAIN alone

1. `tests/target` on MAIN: `LG_TESTS: 433 checks, 0 failures`, `LG_TESTS_RESULT: PASS`. The suite includes `test_grid_state`.
2. AP firmware flashed over it with settings kept: the version 1 record was read (`Settings version 1 from AP 0 (set up)`), the page reports `(configured)`, and `config` shows the grid name, time zone and password record carried over.
3. The shared SSID, 192.168.4.1, the .2 to .13 DHCP block, and the new `nodes` table appear as shown above. Both handhelds, still on pre-D46 firmware, re-joined MAIN and registered as 192.168.4.101 and .102.

### Needs NORTH and SOUTH (once power is fixed)

Capture all three at once, for example `python tools/serial_capture.py --ports COMa COMb COMc --seconds 120 --reset ...`, and check each item below on every AP involved.

1. **Replication on link-up.** Unplug NORTH, change the time zone on MAIN's page, then plug NORTH in. Expect `[GRID] Adopted settings version 2 (made on AP 0) via AP 0` on NORTH, and `config` on NORTH shows the new time zone.
2. **Time set on NORTH demotes MAIN.** With time set on MAIN (AUTHORITATIVE), run `time set <unix>` on NORTH. Expect `[TIME] Time was set more recently on AP 1; following it` on MAIN and SOUTH, then `time` on MAIN shows CARRIED.
3. **Split and rejoin, newest wins.** Separate MAIN from NORTH and SOUTH, for example by unplugging MAIN. Change a setting on NORTH, then change a different setting on MAIN while it is alone (it serves its page alone, with no links). Rejoin. The higher (version, AP) pair must end up on all three `config` outputs, and the loser logs `Adopted settings`.
4. **Admin page on every AP.** From the owner's phone, join "LocalMesh Access Point" near each AP in turn and open http://192.168.4.1/. The subtitle names the serving AP. Log in with the existing password.
5. **Phone roaming keeps 192.168.4.1.** Walk the phone from MAIN's coverage to NORTH's until it re-associates. The page still loads at the same address. The phone's address moves from MAIN's block (.2 to .13) to NORTH's (.14 to .25).
6. **Handhelds** (after the main session flashes them): Settings, Which AP lists MAIN, NORTH and SOUTH by name, not by SSID. `status` on the handheld shows `on MAIN (node 0)`. `[NET] Joining MAIN (node 0, ...)` in its log.
7. **Link robustness.** Leave all three running for at least 30 minutes, then run `nodes` on each. Compare GAPS, MAX_GAP, PROBES, SAVES and LOSSES. A save instead of a loss is the case the owner saw ("Link lost ... no HELLO for 6000 ms", relinked within 30 ms).

### A/B tests

Build a variant into its own folder so the normal build stays untouched.

1. Create `firmware/node/build-ab/sdkconfig.ab` (the `build-*` folders are gitignored) holding the single line to change, for example `CONFIG_LG_NODE_BLE_ADV=n` or `CONFIG_LG_NODE_PMF_CAPABLE=n`.
2. Build from the repository root with absolute paths:

   ```powershell
   $n = (Resolve-Path firmware/node).Path -replace '\\', '/'
   idf.py -C $n -B "$n/build-ab" "-DSDKCONFIG=$n/build-ab/sdkconfig" "-DSDKCONFIG_DEFAULTS=$n/sdkconfig.defaults;$n/build-ab/sdkconfig.ab" build
   ```

   Check that the build has zero warnings.
3. Flash every AP with the variant, one at a time, about 10 s apart: `idf.py -C $n -B "$n/build-ab" -p COMx app-flash`. This writes only the app, so settings, the boot counter, and the device ID stay.
4. `config` must print `BLE advertising: off (A/B build)` or `PMF: off (A/B build)`.
5. Run for the same length of time as the baseline, with the same boards in the same places.

**BLE coexistence A/B.** Compare `nodes` counters (GAPS, MAX_GAP, PROBES, SAVES, LOSSES, tx_fail, rx_queue_full) per hour between the normal build and `CONFIG_LG_NODE_BLE_ADV=n`. Handhelds do not use BLE yet (D4 observer is not built), so nothing else changes.

**PMF A/B.** Compare handheld `[NET] Wi-Fi link to the node lost (reason 2)` counts, and AP `[NET] Station left (aid N, reason 2)` lines, between the normal build and `CONFIG_LG_NODE_PMF_CAPABLE=n`. The handheld still offers PMF, so with the AP not capable the pair falls back to no PMF. Watch also for `wifi:no need to send deauth when softap is sending deauth` bursts after an AP restart. MAIN printed 165 of them in about a second, just before both handhelds (which offer PMF) rejoined it after a reset.

Afterwards reflash with `python tools/flash.py --role N` to return to the normal build.

## Known limitations

- **Not run with more than one AP.** Replication, time demotion, split and rejoin, roaming, and the keepalive probes are built and unit-tested in the simulator only (`test_grid_state`). They have not been checked on two boards.
- **Handheld change not on the boards.** The IE name tail and shared-SSID join are built for esp32 and esp32s3 with zero warnings but not flashed.
- **No Layer 2 bridge.** Every AP is 192.168.4.1 on its own island. A phone on NORTH cannot reach a phone on MAIN by IP, and a TCP connection does not survive roaming, only the address does.
- **No roaming steering for phones.** A phone may hold a weak AP until it drops (D46).
- **A Wi-Fi list shows one network.** Which AP a phone is on shows only on the admin page subtitle and in `status`.
- **12 phone leases per AP.** A 13th phone on one AP gets no address, even though the SoftAP would accept the station.
- **Keepalives cost airtime.** Probes are sent only while HELLOs are missing, at most one a second per neighbour, each a HELLO-sized frame.
- **ACK is not proof of firmware.** A neighbour whose radio ACKs but whose firmware has stopped keeps its link for up to 20 s.
- **`tools/wifi_scan.py` and the `wifi` skill still expect `LG-MAIN`, `LG-NORTH` and `LG-SOUTH`**, and will report them MISSING. They need updating to count radios under "LocalMesh Access Point".
