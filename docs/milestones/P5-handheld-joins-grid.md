# Prototype milestone 5: handheld joins the grid

Status: **verified on the bench from serial captures.** On-screen checks by the owner (test procedure below) are pending.

## What exists

- `firmware/handheld` for the Hosyond 3.2in (ESP32) and the Freenove FNK0104B (ESP32-S3). One build per chip; the board profile comes from the device ID.
- **Network service** (`main/service/hh_service.c`), one task:
  1. Scans the grid channel and recognises LocalGrid nodes by the beacon element's grid discriminator.
  2. Scores nodes by signal, stickiness, load, and backbone health (design review answer 10).
  3. Joins the chosen node's BSSID with a static address 192.168.(4+n).(100+device), opens TCP to .1:7300, and registers through `lg_client`.
  4. Sends PING every 10 s. A Wi-Fi drop, 25 s without PONG, or a socket error starts a new search, backing off from 1 s to 10 s.
- **Home screen** (`main/ui/ui_home.c`) shows:
  - this handheld's roster name and device ID;
  - connection state, node, signal, and address;
  - grid time, or the D6 restriction when it is not set;
  - the handhelds it has heard about;
  - the nodes in range. Tapping a node uses only that node until the handheld restarts; Automatic returns to the best signal.
- The screen reads a status snapshot and sends commands through `main/service/hh_service.h`, and nothing else (D27). `tools/check_layers.py` enforces this on every build.
- **Supporting changes:**
  - `lg_client_ping()` in the protocol core, with a simulator test;
  - handheld device index in the identity partition, written by `tools/flash.py --update-identity`;
  - `LG_SECRET_DISCRIMINATOR`, so handhelds never carry the backbone key;
  - node DHCP limited to .2–.99.

## Build and flash

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
python tools/gen_secrets.py --update        # once: adds LG_SECRET_DISCRIMINATOR, keeps keys
python tools/build.py --firmware handheld
python tools/flash.py --role N              # nodes, one at a time (DHCP range change)
python tools/flash.py --role H --update-identity   # handheld firmware plus device index
```

The device map assigns FNK0104B device 1 ("Handheld 1") and Hosyond device 2 ("Handheld 2"). The test app still runs on a handheld with `--firmware tests`.

## Test procedure (on the handheld screens)

All checks are read from the handhelds' own screens (D23); nothing is tested from a PC (D25).

1. **Join.** Power the three nodes, then both handhelds. On each home screen the status label moves from Searching to Joining to Online, and Connection reads "Online on LG-...". Pass: both Online.
2. **See each other.** Under "Handhelds on the grid", each handheld lists the other as "on your node" or "online, LG-...". Pass: both lists show the other, and no other entries.
3. **Two nodes.** Under "Nodes in range", tap a different node on each handheld, for example LG-NORTH on the FNK0104B and LG-SOUTH on the Hosyond. Pass: each reconnects to the node it was given, and each shows the other as "online, LG-<other node>".
4. **Node loss.** Unplug the node the Hosyond is using while it is set to Automatic. Pass:
   - the Hosyond shows the problem line, then joins another node;
   - the FNK0104B shows it offline, then online again.
5. **Automatic.** Tap Automatic on both. Pass: the Automatic row is highlighted and both stay Online.
6. **Grid time.** Before the admin sets time, Grid time reads "Not set" with the urgent-only notice. Set time on the admin page. Pass: both screens show the UTC time within a few seconds.

## Expected serial output (handheld)

```
LGID: LG-H-F4B-9DTCV8M0R1 role=H board=F4B device=1
NET: [NET] Handheld service started: device 1 (Handheld 1), boot N
UI: [UI] Home screen ready
NET: [NET] Joining LG-MAIN (node 0, -45 dBm) as 192.168.4.101
NET: [NET] Session open to node 0; registering
GRID: [GRID] Registered with node 0 as device 1 (Handheld 1)
GRID: [GRID] Device 2 (Handheld 2) online on node 1
```

On the node: `[GRID] Registered device 1 from 192.168.4.101`.

## Measured

Bench run, 2026-09-15. Captures came from all five boards at once, with every board reset at the start of each capture. Grid time was unset.

| Check | Result |
|---|---|
| Core tests with `test_ping_pong` (FNK0104B) | 373 checks, 0 failures |
| Handheld image size | Hosyond 1,142 KB (26% of app partition free); FNK0104B 1,125 KB (27% free) |
| Boot to Online, Hosyond | 2.8 s (join started 1.25 s; session 2.5 s; registered 2.8 s) |
| Boot to Online, FNK0104B | 2.9 s |
| Wi-Fi join to session open | 1.3 s (Hosyond), 0.7 s (FNK0104B), including the 0.3 s settle before TCP |
| Cross-node presence | Hosyond on LG-MAIN and FNK0104B on LG-SOUTH: each logged the other online on the other node within 0.2 s of registering |
| Handheld reboot with the grid running | Hosyond rejoined on LG-NORTH and registered on the first attempt 2.2 s after boot. The FNK0104B saw it move to node 1. MAIN closed the old session 23 s later (TCP keepalive) and did not mark the Hosyond offline, because presence already named node 1. |
| Heap while online | Hosyond 155 KB free, 147 KB lowest; FNK0104B 208 KB free, 206 KB lowest |
| Node heap with one handheld attached | MAIN 60 KB free, 52 KB lowest; NORTH 71 KB, 65 KB; SOUTH 72 KB, 62 KB. Unchanged from P3. |
| Grid time unset | Both handhelds logged `[TIME] Grid time is not set: only receiving and urgent broadcasts (D6)` |

Found and fixed during the run: a handheld that rebooted and rejoined the same node lost its first `REGISTER_ACK`. The node sent it to the handheld's previous, still-open session. The ACK timed out after 5 s, and the retry registered. `sess_send()` now uses the newest session for a device.

Verified after the fix: the FNK0104B was rebooted while LG-SOUTH still held its session. It rejoined LG-SOUTH, the node logged `[NET] Replacing stale session 192.168.6.101 for device 1`, and the handheld registered 20 ms after opening its session, on the first attempt.

## Known limitations

- **BLE** discovery on handhelds (decision D4) is not in this milestone; discovery is Wi-Fi only.
- **Roaming:** a handheld changes node only when its link fails or it is given a node. Proactive roaming (answer 11) comes later.
- **Messaging:** no messaging screens yet; they are P6.
- **Self test (D24):** the quick boot self-test is not in the handheld firmware yet.
- **Time zone:** grid time is shown in UTC, because the admin's time zone is not sent to handhelds yet.
- **Node choice:** a tapped node lasts until the handheld restarts.
- **Roster:** fixed prototype roster (Handheld 1–4). The device index comes from the bench device map, not from pairing.
- **Secrets:** the X25519 private key is in NVS without flash encryption, and the Wi-Fi passphrase is compiled in.
- **Presence flap on a quick reboot:** when a handheld reboots and reconnects to the same node, its new connection resets the old one. The node logs the old session closed, marks the handheld offline, then online again in the same millisecond. The other handheld receives both updates; the screen refreshes every 500 ms, so it normally never shows the offline state.
- **Flash space:** the ESP32-S3 image is 1,125 KB, leaving 27% of the 1.5 MB app partition free. Emoji fonts in P6 will need watching.
