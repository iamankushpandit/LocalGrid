---
name: grid-watch
description: Watch the LocalGrid network's status from this laptop over BLE, without joining its Wi-Fi — APs heard, their health, which handhelds are online and where, batteries, and any SOS or all clear — on a local web dashboard. Use when asked how the grid is doing, whether an alert is active, which AP a handheld is on, or to check the D68 status beacon after flashing APs.
---

# Watch the grid over BLE

Every AP puts a sealed status frame in its BLE scan response (D68, `docs/ble-status.md`): AP health, handhelds online and where, the newest urgent alert, batteries, and chosen names, rotating every 500 ms. `tools/grid_watch.py` listens for them next to each AP's discovery advert, verifies each frame against this grid's key, and serves a dashboard at `http://127.0.0.1:8768/`.

**Decision D25 holds.** The laptop is a witness, never a participant: it never joins the grid's Wi-Fi, never carries a message, and never speaks the LocalGrid protocol. Being exact about the radio: Windows scans actively so scan responses arrive, so the laptop sends generic BLE scan requests; the grid does not act on them. **Decision D21 holds too**: BLE addresses are never printed, logged, or served; APs are keyed by the AP index in their advert.

| Goal | Command |
|---|---|
| Watch the grid, dashboard in the browser | `python tools/grid_watch.py` |
| Headless, or the browser is opened by hand | `python tools/grid_watch.py --no-browser` |
| Which APs answer, then exit | `python tools/grid_watch.py --no-browser --seconds 20` |
| Keep a record of events | `python tools/grid_watch.py --log grid_events.jsonl` |
| Another grid's secrets | `python tools/grid_watch.py --secrets path/to/lg_secrets.h` |
| Check the decoder with no hardware | `python tools/grid_watch.py --self-check` |

## Steps

1. **Check the decoder.** `python tools/grid_watch.py --self-check` seals frames the way the spec says an AP does, with a test key, and runs them through the decoder: every frame type decodes, a bad tag, a changed header, or another grid's key is rejected, replays are dropped, another grid's discriminator is ignored, and the page serves on loopback with no address in it. Run it after any change to the tool or to `docs/ble-status.md`. Done when it prints `SELF-CHECK: PASS`.

2. **Listen.** Bluetooth must be on. The tool reads the backbone key and discriminator from `firmware/common/lg_secrets.h` and the company ID from `firmware/common/lg_proto_config.h`; AP names come from `tools/bench_devices.json` until an AP announces its own. It prints events (APs heard and lost, restarts, alerts, time source changes) to the terminal as they happen. Done when each expected AP prints `heard`.

3. **Read the dashboard.** An alert banner (red `SOS from <name> near <AP>, N min ago, read by N`, green `All clear: <name> is safe`), one card per AP, a handhelds table with batteries, and an event log. Tick "beep on alert" for a sound when a new SOS arrives. Done when you have the answer you came for, or know which AP to look at on its serial console.

4. **Stop it** with Ctrl+C. It prints the APs it heard and how many frames were verified, failed the check, or were dropped as replays. Done when the prompt returns.

## Reading the result

| Dashboard says | Means |
|---|---|
| AP card with health, `heard` | The AP is up and its status verifies under this grid's key. |
| `none yet (discovery advert only)` | The AP advertises but sends no status: firmware from before D68, or the beacon failed to start. Check its serial log for `[BLE]`. |
| `cannot be verified ... wrong lg_secrets.h?` | Frames arrive and fail the check: this laptop's `lg_secrets.h` is not the grid's, or the AP's frame format differs from the spec. |
| `not heard` | Silent for 10 s: out of BLE range (tens of metres), off, or crashed. Compare with `tools/wifi_scan.py` and the AP's `nodes`. |
| A handheld `offline` | No AP in range of this laptop lists it online. BLE shows only the APs the laptop can hear, and each AP's own view. |
| `replays dropped` above 0 | Frames with an old counter were received. Nothing forged can pass the tag, but repeated replays mean someone is re-sending recorded frames. |

## Invariants

- The laptop listens; it never transmits into the grid (D25). A task that needs a sender runs on an ESP32.
- No BLE address reaches the terminal, the page, or the log (D21).
- The dashboard binds to 127.0.0.1 only and loads nothing from the Internet: the laptop may be offline.
- A frame is shown only after its 4-byte tag verifies and its counter is newer than the last from that AP and boot. After an AP's NVS is erased its boot counter starts again lower; restart the tool.
- The frame layout lives in `docs/ble-status.md`. Change the spec, the AP firmware, and this tool's self-check together.
