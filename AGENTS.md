# LocalGrid agent guide

LocalGrid is an offline, self-forming ESP32 messaging network: infrastructure nodes (headless ESP32) carry 1:1, group, and broadcast text between handheld touchscreen devices. No Internet at runtime, ever. The project is in the prototype stage.

User-facing text calls the product "an offline network", never a camp or campsite network (decision D19). Camping is one use case, not the product's identity.

## Where things are

| Path | What |
|---|---|
| `docs/DESIGN_REVIEW.md` | Architecture: 47 numbered answers. Cite answers by number. |
| `docs/DECISIONS.md` | Standing owner decisions. Read before changing behavior. |
| `docs/BOARDS.md` | Supported boards, the board profile fields, how to add a board, and the bring-up checklist. |
| `CHANGELOG.md` | Every change, newest first. |
| `components/lg_core` | Portable C11 protocol core: envelope, IDs, dedup, routing, handheld logic. |
| `components/lg_crypto` | The only crypto interface (PSA Crypto backend). |
| `components/lg_identity` | Device ID partition and the serial `id` responder. |
| `components/lg_board` | Board profiles: pins and panel and touch facts, selected by the board code in the device ID. Data only. |
| `components/lg_bsp` | Board support drivers: SPI and I2C buses, display panel, touch controllers. No screens. |
| `components/lg_draw` | The handheld renderer (D55): retained boxes and painted regions, hardware scroll, fonts, touch calibration. No UI library and no drivers. |
| `components/lg_power` | Supply voltage from a board's sense divider and the `power` console command, shared by every firmware. |
| `components/lg_selftest` | The quick boot check (D24): envelope, dedup, bodies, UTF-8, and the RFC crypto vectors. Reachable from Status. |
| `firmware/node` | Infrastructure node firmware. |
| `firmware/handheld` | Handheld firmware: network service in `main/service`, screens in `main/ui` (drawn with `lg_draw`), meeting only in `hh_service.h`. |
| `firmware/common` | Prototype grid config shared by all firmware; secrets are generated here. |
| `tests/target` | On-board test app with a simulated three-node grid. |
| `tools/` | Secrets generator, multi-port serial capture, build and flash, and `grid_watch.py`, the laptop dashboard that reads the grid over BLE (`docs/grid-watch.md`). |
| `site/`, `tools/gen_site.py` | Project site template and generator; every fact comes from the tree (`docs/OPEN_SOURCE.md`). |
| `.github/` | CI (`verify` is the required check), Pages, issue and pull request templates. |

## Skills

- `.claude/skills/bench/SKILL.md` — build, run on-board tests, and read serial logs on the bench boards.
- `.claude/skills/chaos/SKILL.md` — take APs and handhelds out at random for hours and report how the grid recovers. Boards are found by their own device IDs; run it only when the owner asks.
- `.claude/skills/build/SKILL.md` — build every firmware type, chosen types or targets, or what named boards run, without flashing.
- `.claude/skills/flash/SKILL.md` — flash one, several, or all boards with their assigned firmware from `tools/bench_devices.json`.
- `.claude/skills/power/SKILL.md` — supply voltage on one or all boards, now or monitored with min, average, and max.
- `.claude/skills/wifi/SKILL.md` — count the APs broadcasting the grid SSID from this laptop's radio.
- `.claude/skills/grid-watch/SKILL.md` — watch the grid's status from this laptop over BLE (D68) on a local dashboard, without joining its Wi-Fi.
- `.claude/skills/protocol-change/SKILL.md` — add or change a message type or `lg_core` behavior.
- `.claude/skills/new-board/SKILL.md` — add a board: profile, code, any new driver, build, bring-up checklist, evidence.

## Rules

- **Owner decides requirements.** When a requirement looks wrong or a hardware limit blocks it, stop and explain the problem, the limit, the options, and a recommendation. Record the owner's answer in `docs/DECISIONS.md`.
- **Core stays portable.** `lg_core` includes only C standard headers and its own headers. ESP-IDF, sockets, and radios live in firmware glue and reach the core through its `io` callback structs.
- **Everything is bounded.** Fixed-size tables and pools, limits in `lg_types.h`, no heap allocation per message. A full table rejects or evicts with a defined, logged result.
- **One task owns core state.** Wi-Fi, ESP-NOW, and BLE callbacks copy into a queue and return. Console commands post to the core task's queue.
- **Crypto only through `lg_crypto.h`**, with RFC test vectors in `tests/target` for every primitive.
- **Nonces never repeat.** Nonces are built from (author, boot counter, sequence); the boot counter is committed to NVS before any radio transmit. A retransmission reuses the identical plaintext, AAD, and grid time.
- **Prefix public symbols** with `lg_` (core, crypto) or a module prefix such as `lgbb_`. Short names collide with Espressif's closed libraries; `bb_init` already exists in the PHY library.
- **Test on the ESP32 boards only** (D25). The PC builds, flashes, and reads serial logs; it never stands in for a handheld or a node.
- **Layers stay separate** (D27). Infrastructure components never include UI headers. `lg_bsp` is the only UI-side code that includes ESP-IDF drivers, and `lg_draw` reaches hardware only through it. Handheld screens and services exchange events and commands through a queue: screens never touch sockets, and services never draw. LVGL is retired (D55); nothing may bring it back. `tools/check_layers.py` checks this, and `tools/build.py` runs it on every build.
- **Information is sticky** (D48). Anything a device learns that another could use must survive the holder restarting and be shared with neighbours, versioned so newer wins and duplicates merge, and announced on link up and periodically. A record kept on only one device is a design gap.
- **Devices hold bytes, browsers make text** (D49). Store and send state as packed binary records; decode, format, and draw in the browser or PC tool. No long-lived text buffers on a device; new page APIs are binary with a documented layout.
- **Handhelds show results on their screen** (D23). Any firmware flashed to a handheld, including test builds, drives the display with its status and results; serial output is an addition, never the only output.
- **UI code** reads screen size at runtime and styles through the theme (D9, D10). Pixel values and colors appear only in board profiles and theme tables.
- **Secrets** come from `python tools/gen_secrets.py` into gitignored `firmware/common/lg_secrets.h`. Commit only `lg_secrets.example.h`.
- **Log network transitions** with a bracketed tag (`[NET]`, `[BB]`, `[GRID]`, `[TIME]`, `[BLE]`, `[ROAM]`, `[MSG]`). Never log keys, passphrases, or plaintext of 1:1 messages.

## Definition of done

A change is done when:

1. Every affected firmware project and `tests/target` build with zero warnings under ESP-IDF v6.1 defaults.
2. `tests/target` prints `LG_TESTS_RESULT: PASS` on a bench board after any `lg_core` or `lg_crypto` change.
3. Radio or firmware behavior changes are verified on the boards with captured serial output.
4. `CHANGELOG.md` has an entry, and `docs/DECISIONS.md` is updated if a decision changed.
5. Milestones also get a report in `docs/milestones/` with build commands, expected serial output, a test procedure, and known limitations.
