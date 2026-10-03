# 2026-10-03, overnight: what was fixed, what is on each device, what is still wrong

Written for the morning, so nothing has to be reconstructed from a terminal. Everything below was
verified on the bench, not inferred, except where it says otherwise.

## What is on each device

| Device | Firmware | Verified by |
|---|---|---|
| Chase, Marshall, Bluey, Channi, Jolly (e28, e28b, hosyond, fnk0104b, fnk0104b_c) | handheld, voice + tones + animated saver labels (03:xx build) | `flash.py` id ok, registered |
| **Big Pepa (e40)** | same handheld build | `flash.py e40` via the new no-stub path: id ok, registered as device 6 |
| **Fuji (COM29), Denali (COM41)** | node with D90 `OP_FAULT`, D91 on-demand admin page | `Admin page off until asked` on boot; `fault lora 20` and `web 3` acknowledged over BLE |
| **Everest** | **old firmware** - alive on the air (answers BLE, beacons), **not reachable over USB** | COM16's CP210x enumerates; the ESP32 behind it never sends a byte at any rate |

## What was wrong and is fixed

1. **Capacity: one handheld per access point.** With the admin page resident an AP had ~14 KB free and
   `node_station_room()` offered room for one; six handhelds cycled through join / accepted / closed. D91:
   the page starts only in setup mode or on request (`web on`, `OP_WEB`) and stops itself. After the
   reflash: Denali 4 attached, Fuji 2 attached, every handheld home.
2. **Chaos needed a special AP build.** D90: one BLE opcode asks an AP for a fault it already knew how to
   perform; `chaos.py` plans and drives them itself (`--admin-password`). T1, T2, T5 superseded.
3. **Big Pepa could not be flashed.** Not the port, not power, not the rate: the stub loader fails on this
   board, and an aborted write leaves its bootloader corrupt. `flash_no_stub` writes the application and
   then the boot record, ROM loader only, with retries - and then **verifies by booting**, never by reading
   flash back: a read-back matched byte for byte while the ROM still looped on `invalid header`, because a
   reset resets the ESP32 and not the flash chip, and a ROM-loader read leaves the chip in a mode the ROM
   does not expect. A write leaves it normal, so the last thing the tool does is write, and the board's own
   ID line within 30 s is the proof; otherwise the boot record is written again. Proved end to end.
4. **Screen saver** names this handheld and its AP - `LoRa to <AP>` when that is where the frames go - each
   letter lit and fading like a rain glyph, never falling.
5. **Chase went blank** after a write: held in reset by the port close, released by opening the port
   properly. Not a reflash.
6. **Big Pepa restarted itself mid-message** during the verification run below: `spi_intr spi_master.c:1022
   (host->cur_cs == DEV_NUM_MAX)`. Its XPT2046 shares the panel's SPI host and polled it while esp_lcd had
   a DMA transaction in flight. The poll now holds the bus (`lg_bsp_touch.c`). Flashed to the four XPT
   boards; soak below.

## The verification run (`chaos-runs/20261003-030303`)

Ten minutes, handhelds on USB, access points deliberately **excluded** from USB so the harness took the
no-cable branch. It did: found Everest, Fuji and Denali over BLE, planned `ble_bb_off` rounds against them
with the same seed machinery, and asked for each fault over BLE. Denali acknowledged every fault. Everest
never did - it runs firmware without `OP_FAULT` - and the report said so, as it should. Two harness
mistakes came out and were fixed before the morning: a finding that said "acknowledged" about a fault that
was refused, and a beacon-based confirmation for backbone-off that could never fire, because D74 declares a
link down only after three missed keepalives (~3 min) and the windows were 18-29 s. A restart is still
confirmed from the boot counter in the beacons; a backbone-off is confirmed by the acknowledgement and
measured by what the probes see.

## What is still wrong

- **Everest's USB.** The bridge works and the chip does not answer it, on two ports. The board runs
  (it beacons and takes BLE sessions) on the firmware it had before tonight - the plain build with the
  resident admin page, so it serves one handheld and refuses `OP_FAULT`. Two ways to update it: a different
  USB cable (if the cable's data lines are the fault, most likely), or over the air via D81 from a laptop
  joined to its Wi-Fi. The second was deliberately not attempted at 03:00 on the one AP with no serial
  recovery.
- **Two `Session receive error` lines** on Big Pepa and Marshall before they registered, after the fix. Both
  then registered and stayed. Tomorrow's run watches both ends; the AP's own close reason
  (`sessions.c`: peer closed / receive error / no registration / idle timeout) will say which.
- **Group voice** has still not been exercised on hardware; 1:1 and announcement have. The `voice` round
  covers it in the next run.

## Tomorrow's run

```bash
python tools/chaos.py --live --lora --until-aps-die --hours 24 --admin-password <pw>
```

APs on battery, handhelds on USB. The harness finds the APs over BLE and asks each for its faults; an AP
on old firmware (Everest, until it is flashed) will show as "not acknowledged" in the report, which is
the truth rather than a bug.
