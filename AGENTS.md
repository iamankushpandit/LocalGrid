# LocalGrid agent guide

LocalGrid is an offline, self-forming ESP32 messaging network: infrastructure nodes (headless ESP32) carry 1:1, group, and broadcast text between handheld touchscreen devices. No Internet at runtime, ever. The project is in the prototype stage.

User-facing text calls the product "an offline network", never a camp or campsite network (decision D19). Camping is one use case, not the product's identity.

## Where things are

| Path | What |
|---|---|
| `docs/DESIGN_REVIEW.md` | Architecture: 47 numbered answers. Cite answers by number. |
| `docs/DECISIONS.md` | Standing owner decisions. Read before changing behavior. |
| `CHANGELOG.md` | Every change, newest first. |
| `components/lg_core` | Portable C11 protocol core: envelope, IDs, dedup, routing, handheld logic. |
| `components/lg_crypto` | The only crypto interface (PSA Crypto backend). |
| `components/lg_identity` | Device ID partition and the serial `id` responder. |
| `components/lg_board` | Board profiles: pins and panel and touch facts, selected by the board code in the device ID. Data only. |
| `components/lg_bsp` | Board support drivers: SPI and I2C buses, display panel, touch controllers. No LVGL. |
| `components/lg_ui` | LVGL display glue, theme, pointer input, and the calibration screen. No drivers. |
| `firmware/node` | Infrastructure node firmware. |
| `firmware/handheld` | Handheld firmware: network service in `main/service`, screens in `main/ui`, meeting only in `hh_service.h`. |
| `firmware/common` | Prototype grid config shared by all firmware; secrets are generated here. |
| `tests/target` | On-board test app with a simulated three-node grid. |
| `tools/` | Secrets generator and multi-port serial capture. |

## Skills

- `.claude/skills/bench/SKILL.md` — build, run on-board tests, and read serial logs on the bench boards.
- `.claude/skills/build/SKILL.md` — build every firmware type, chosen types or targets, or what named boards run, without flashing.
- `.claude/skills/flash/SKILL.md` — flash one, several, or all boards with their assigned firmware from `tools/bench_devices.json`.
- `.claude/skills/protocol-change/SKILL.md` — add or change a message type or `lg_core` behavior.

## Rules

- **Owner decides requirements.** When a requirement looks wrong or a hardware limit blocks it, stop and explain the problem, the limit, the options, and a recommendation. Record the owner's answer in `docs/DECISIONS.md`.
- **Core stays portable.** `lg_core` includes only C standard headers and its own headers. ESP-IDF, sockets, and radios live in firmware glue and reach the core through its `io` callback structs.
- **Everything is bounded.** Fixed-size tables and pools, limits in `lg_types.h`, no heap allocation per message. A full table rejects or evicts with a defined, logged result.
- **One task owns core state.** Wi-Fi, ESP-NOW, and BLE callbacks copy into a queue and return. Console commands post to the core task's queue.
- **Crypto only through `lg_crypto.h`**, with RFC test vectors in `tests/target` for every primitive.
- **Nonces never repeat.** Nonces are built from (author, boot counter, sequence); the boot counter is committed to NVS before any radio transmit. A retransmission reuses the identical plaintext, AAD, and grid time.
- **Prefix public symbols** with `lg_` (core, crypto) or a module prefix such as `lgbb_`. Short names collide with Espressif's closed libraries; `bb_init` already exists in the PHY library.
- **Test on the ESP32 boards only** (D25). The PC builds, flashes, and reads serial logs; it never stands in for a handheld or a node.
- **Layers stay separate** (D27). Infrastructure components never include UI or LVGL headers. `lg_bsp` is the only UI-side code that includes ESP-IDF drivers, and `lg_ui` reaches hardware only through it. Handheld screens and services exchange events and commands through a queue: screens never touch sockets, and services never touch LVGL. `tools/check_layers.py` checks this, and `tools/build.py` runs it on every build.
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
