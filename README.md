# LocalGrid

**An offline network built from inexpensive ESP32 boards.** Access points find each other and form a
mesh by themselves, and touchscreen handhelds send text, emergencies, and push-to-talk voice across
it. No Internet, no phones, and no cell service, ever.

Think of a group trip where several families spread out across a place with no signal. Everyone
carries a handheld, and a few powered access points give coverage. Nobody configures
anything after the first setup.

> **Status: prototype.** It runs on the bench boards every day and is changing fast. Read
> [the milestone reports](docs/milestones/) before relying on any part of it.

[Project site](https://iamankushpandit.github.io/LocalGrid/) ·
[Try the admin page in your browser](https://iamankushpandit.github.io/LocalGrid/admin/) ·
[Design review](docs/DESIGN_REVIEW.md) · [Decisions](docs/DECISIONS.md) · [Changelog](CHANGELOG.md)

## What it does

- **1:1 messages, end-to-end encrypted** with X25519, HKDF-SHA256, and ChaCha20-Poly1305. Access
  points carry them without being able to read them (D5).
- **Groups and broadcasts**, encrypted in transit, with delivery and read reporting.
- **Emergencies** that break through every screen, including the lock screen (D57, D58).
- **Push-to-talk** to a person or a group, with 100 ms ADPCM frames sealed like text (D61).
- **Self-healing.** A device that restarts gets its state back from the devices that stayed up
  (D48, D53). Grid time survives restarts and can come from a GPS module on the main access point,
  which also shows that access point's location on its admin page (D60, D63, D64).
- **An admin page on every access point** at `http://192.168.4.1/` to name the grid, manage people
  and groups, and set the time (D45).
- **Positions on a map.** A handheld with a GPS shares its position with the grid, held in RAM only, so the admin page can
  show where people are and a handheld can show its distance to MAIN (D65).
- **Watch the grid over BLE without joining it.** Every access point broadcasts a sealed status beacon; `tools/grid_watch.py`
  shows access point health, who is online, batteries and alerts on a laptop. Read-only, never a participant (D68).
- **Bounded by design.** Fixed-size tables, no heap allocation per message, and a defined result when
  anything fills up.

## Hardware

| Board | Chip | Role |
|---|---|---|
| Elegoo ESP32 dev board (or any 4 MB ESP32) | ESP32 | Access point |
| Hosyond 3.2in display (ST7789, resistive touch) | ESP32 | Handheld |
| Freenove FNK0104B 2.8in display (ILI9341, capacitive touch, mic, speaker) | ESP32-S3 | Handheld |
| GPS module, GT-U7 (u-blox 7 class), optional | UART | Grid time and location: on MAIN, and on a handheld (D63, D64, D65) |
| LoRa module, Reyax RYLR998 (SX1262), optional | UART | A second, longer link between access points. Fitted; firmware being written (D71) |

Board facts live in data-only profiles in [`components/lg_board`](components/lg_board). A new
display board is often a profile and a touch check away; see [CONTRIBUTING.md](CONTRIBUTING.md#port-a-board).

The GPS is optional: without one the admin sets the time and there is no location card; nothing else changes. LoRa
modules are now fitted to the access points as a second, longer link between them, and their firmware is being written;
handhelds come later, also optional.

## Build it

LocalGrid uses pure ESP-IDF v6.1 (D1): no Arduino and no PlatformIO.

1. Install [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
   with targets `esp32` and `esp32s3`, and load its environment.
2. Generate your grid's keys. Every device in one grid must be built from the same file, so keep it:
   ```bash
   python tools/gen_secrets.py
   ```
3. Build every firmware type for every target, with warnings refused:
   ```bash
   python tools/build.py
   ```
4. Put your boards in [`tools/bench_devices.json`](tools/bench_devices.json) and flash them. The
   tool checks each board's device ID before it writes anything:
   ```bash
   python tools/flash.py --all
   ```
5. Flash `tests` to any board and look for `LG_TESTS_RESULT: PASS`. It runs the protocol core's unit
   tests, the RFC crypto vectors, and a simulated three-node grid on the chip.

Then join the grid's Wi-Fi from a laptop or phone and open `http://192.168.4.1/` to set it up.

## Repository layout

| Path | What |
|---|---|
| `components/lg_core` | Portable C11 protocol core: envelope, IDs, dedup, routing, handheld logic |
| `components/lg_crypto` | The only crypto interface (PSA Crypto backend) |
| `components/lg_board`, `lg_bsp`, `lg_draw` | Board profiles, drivers, and the handheld renderer |
| `firmware/node` | Access point firmware, including the admin page |
| `firmware/handheld` | Handheld firmware: network service and screens |
| `tests/target` | On-board test app |
| `tools/` | Build, flash, serial capture, chaos testing, and site generation |
| `docs/` | Design review, decisions, milestone reports |

[AGENTS.md](AGENTS.md) has the rules the code follows. Read it before your first change.

## Contributing

Contributions are welcome, especially board ports, testing on your own radios, and the admin page.
Start with [CONTRIBUTING.md](CONTRIBUTING.md). Report security problems privately as described in
[SECURITY.md](SECURITY.md).

## Licence

GNU General Public License v3 or later (`GPL-3.0-or-later`), the same licence as
[Braino](https://github.com/iamankushpandit/Gume), so work can move between the two projects. See
[LICENSE](LICENSE). Third-party material keeps its own licence, listed in
[THIRD_PARTY.md](THIRD_PARTY.md); the reasoning behind the choice is in
[docs/OPEN_SOURCE.md](docs/OPEN_SOURCE.md).
