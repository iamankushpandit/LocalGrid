# Prototype milestone 2: node firmware

Status: **passed on hardware, 2026-09-15.** Three Elegoo ESP32 nodes (COM16, COM17, COM18).

## What exists

`firmware/node` runs on every infrastructure node:

| Module | Purpose |
|---|---|
| `node_main.c` | Identity from the MAC table, boot counter, SoftAP, grid time, discovery payload, core task |
| `backbone.c` | ESP-NOW on the SoftAP interface: sealed frames, HELLO, two-way link confirmation, one frame in flight |
| `sessions.c` | TCP control sessions for handhelds on port 7300 |
| `ble_adv.c` | NimBLE broadcaster with the discovery payload |
| `console_cmds.c` | Serial console |

| Node | Port | SSID | Address |
|---|---|---|---|
| 0 MAIN (master) | COM16 | `LG-MAIN` | 192.168.4.1 |
| 1 NORTH | COM17 | `LG-NORTH` | 192.168.5.1 |
| 2 SOUTH | COM18 | `LG-SOUTH` | 192.168.6.1 |

All three are on channel 6 with the passphrase from `firmware/common/lg_secrets.h`.

## Build and flash

```powershell
python tools/gen_secrets.py
. C:\esp\v6.1\esp-idf\export.ps1
idf.py -C firmware/node set-target esp32
idf.py -C firmware/node -p COM16 flash
idf.py -C firmware/node -p COM17 flash
idf.py -C firmware/node -p COM18 flash
```

## Test procedure

Capture all three nodes, reset them in a staggered order, set time, echo, and reboot one node:

```powershell
python tools/serial_capture.py --ports COM16 COM17 COM18 --seconds 40 `
  --reset-at COM18@0 --reset-at COM16@7 --reset-at COM17@14 `
  --send "COM16@20:time set 1790000000" --send "COM18@23:ping after staggered start" `
  --reset-at COM17@27 --send "COM16@37:nodes"
```

Or interactively with `idf.py -p COM16 monitor` and the console commands `status`, `nodes`, `devices`, `ping [text]`, `time`, `time set <unix>`.

## Expected serial output

```
[COM16] I (1102) BB: [BB] ESP-NOW v2 on SoftAP interface, node 0 boot 3
[COM16] I (1432) BLE: [BLE] Advertising LocalGrid discovery every 500 ms
[COM16] I (1452) BB: [BB] Link up to node 1, RSSI -12, 0 clients there
[COM18] W (8382) BB: [BB] Node 0 rebooted; link down until confirmed
[COM18] I (8382) BB: [BB] Link up to node 0, RSSI -22, 0 clients there
[COM18] I (19962) TIME: [TIME] Adopted grid time 1790000000 from node 0
[COM16] I (15972) BB: [BB] Echo from node 2 after 1 hop(s): after staggered start
[COM17] I (4182) TIME: [TIME] Adopted grid time 1790000011 from node 0
[COM16]   tx 23 fail 0 dropped 0 nomem 0 | rx 39 auth_fail 0 replay 0 queue_full 0 | queued 0
```

## Measured

| Item | Value |
|---|---|
| Boot to first confirmed link | about 1.3–1.9 s |
| Rebooted node relinked to all neighbors | about 2.4–3.3 s after its boot |
| Neighbors reconfirm a rebooted node | on its first HELLO |
| Diagnostic echo deliveries per node | exactly 1 |
| Rebooted node re-adopts grid time | on link-up, about 4 s after boot |
| Backbone counters over 40 s with 3 reboots | tx fail 0, auth fail 0, replay 0, queue full 0, NO_MEM 0 |
| Free heap with SoftAP, ESP-NOW, NimBLE, TCP server | 81.5 KB now, 74.4 KB minimum, 73.7 KB largest block |
| Binary size | 919 KB; 40% of the 1.5 MB app partition free |
| Desk RSSI between nodes | −12 to −25 dBm |

The design review estimated about 137 KB of free heap without BLE. The measured 74 KB with NimBLE running is consistent with BLE's expected cost and stays above the 40 KB floor.

## Known limitations

- **Desk test only.** All nodes hear each other, so a true two-hop chain has not run on real radios yet. The simulator covers chains; a reduced-TX-power chain test is next.
- **BLE interference not yet stressed.** Counters were clean over short runs; the 24-hour soak and message load are still to come.
- **No handhelds yet,** so TCP sessions, presence, and messages between users have not run on radios.
- **Presence does not expire** for users on a node that disappears.
- **Grid time is set from the serial console** until the admin web page exists.
- **Opening a serial port resets the Elegoo boards** through the auto-reset circuit.

## Next

The owner moved the configuration web page forward: the next milestone is the admin web page on the master node, starting with time setting and grid status.
