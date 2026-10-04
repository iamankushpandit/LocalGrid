# 2026-10-04, a run that stalled on a stale beacon, and six memory trials

Two things from one night. A six-hour cabled run meant to catch Fuji's crash stalled after its first
round, for a reason the fix already committed as D92 removes. Then, with the boards free, each memory
idea was tried on one access point, one change at a time.

## 1. The run: `chaos-runs/20261004-013912`, seed 22, stopped by hand at 2 h 10 min

Firmware: the build before D92. All nine boards on USB.

**No crash, no brownout, no restart that was not asked for**, on any access point. Low-water marks
reached 2 KB on Everest and Denali and 3 KB on Fuji.

**Only round #1 ran.** Rounds #2 to #10 were each "not started: grid still not steady", because one
handheld (Channi) had no Wi-Fi session and the harness injects nothing until every handheld is
registered. That is the harness doing its job; the grid really was not steady.

**Why Channi was locked out.** From about twenty minutes in, every scan by every handheld heard all three
access points say "0 free" - for the rest of the run - while they held 14 to 21 KB and five handhelds
between them (1, 2 and 2). Channi scanned 608 times and found nothing usable. The room byte in the beacon
was rewritten only when a handheld joined or left, so a "0 free" taken at a low moment stood for two
hours. D92 rewrites it whenever the room goes to or from zero.

**Everest held one handheld.** 17 of the run's 18 "No heap to serve another handheld" refusals were
Everest's, at 10 to 14 KB free. It rested at 15 KB with one attached, advertised "1 free", and refused
the second. The balancer made it worse: twice, handhelds left Fuji for the emptier-looking Everest and
were turned away.

**Free heap moves in steps that nothing logged explains.** With the same handhelds attached:

| Access point | Step | When |
|---|---|---|
| Denali | 21 KB to 13 KB, back to 21 KB | down at about 14 min, up at about 58 min |
| Fuji | 21 KB to 16 KB, stayed | at about 25 min |
| Everest | 14 KB to 18 KB | at about 90 min |

While Denali sat at 13 KB it restarted its Bluetooth advertising about seven times a minute: 13 KB is
the threshold at which the admin link is offered (`LINK_MIN_HEAP`), and the heap was crossing it
constantly. Whether the advertising mode is a cause of the plateau or only a symptom is **not known**.

## 2. The trials

Method: one access point restarted on the image under test, the other two held in reset so all six
handhelds have only it to join, five minutes, its console read once a second. Every trial image is the
current source with exactly one thing changed, built in its own folder.

### On Fuji (no GPS)

| Image | Free at rest, nobody attached | Handhelds it settled at | Free with them | Low-water | Refusals |
|---|---|---|---|---|---|
| Current build (D92) | 27 KB | 4 | 16 KB | 7 KB | 2 |
| **NimBLE pools halved** | **36 KB** | **6** | 19 KB | 4 KB | 1 |
| AMPDU off | 27 KB | 4 | 16 KB | 6 KB | 1 |
| Wi-Fi dynamic buffers 16 to 8 | 27 KB | 4 | 16 KB | 6 KB | 3 |
| **One Bluetooth controller connection, not three** | **29 KB** | 5 | 15 KB | 4 KB | 5 |

- **NimBLE pools** (`BT_NIMBLE_MSYS_2_BLOCK_COUNT` 24 to 12, `BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` 24 to
  12, `BT_NIMBLE_TRANSPORT_EVT_COUNT` 30 to 16): **9 KB back**, the largest single gain found. The access
  point served all six handhelds. Not yet checked: that the admin link still logs in and carries a fault
  command with the smaller pools.
- **One controller connection** (`BTDM_CTRL_BLE_MAX_CONN` 3 to 1): **2 KB back.**
- **AMPDU off** and **smaller Wi-Fi buffers**: no change at rest and none in the low-water mark. Dropped.
- **TCP windows** were not tried: they are already 2,880 bytes against a floor of 2,440.

D92 itself, seen on a board for the first time in the baseline row: 2 refusals in five minutes where the
same situation produced 67 in six, and four handhelds attached and stable.

### On Everest (the GPS)

| Image | Free at rest, nobody attached | Handhelds it settled at | Free with them | Low-water |
|---|---|---|---|---|
| Current build | 22 KB | 2 | 16 KB | 9 KB |
| **GPS read from the core loop** (`LG_NODE_GPS_POLLED`) | **26 KB** | **3** | 17 KB | 6 KB |

**4 KB back**, as predicted from the code. Grid time was taken from the GPS at the same moment after boot
on both (2.5 s). On the experiment image, after six minutes: fix held, 10 satellites, 2,866 sentences, 4
bad checksums. The same count on the current build has not been taken yet, so "no more bad sentences
than before" is not shown.

## 3. What is and is not in the firmware

- `LG_NODE_GPS_POLLED` is in the source as a switch, **off**: nothing changes until it is turned on.
- **None of the kept settings is in the build configuration.** The change to
  `firmware/node/sdkconfig.defaults` was refused by the session's permission check, so it waits for the
  owner. The four lines are the three NimBLE pool sizes and `CONFIG_BTDM_CTRL_BLE_MAX_CONN=1`.
- With more free heap the unchanged room formula admits more handhelds by itself: six on Fuji, at a 4 KB
  low-water mark. Whether six at 4 KB is wanted, or the formula should hold some of the gain back as
  headroom, is the owner's call.
