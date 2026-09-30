---
name: new-board
description: Walk a contributor through adding a LocalGrid board — gather facts, write the lg_board profile and board code, add an lg_bsp driver if a part is new, build every target, run the bring-up checklist, and submit with evidence. Use when someone wants to support a new ESP32 handheld or AP board, a new panel, touch controller, or codec.
---

# Add a board

`docs/BOARDS.md` is the guide: the field-by-field profile reference, the six steps, and the bring-up checklist. This skill walks the same path. Read that file first and keep it open.

## Before starting

**Open, under GPL-3.0-or-later** (D76). Board ports are welcome; point a contributor at `CONTRIBUTING.md` first. A board request issue (`.github/ISSUE_TEMPLATE/board_port.yml`) is the place to start.

**Facts, never code.** Pins and register meanings may be restated from datasheets, vendor pin tables, Braino (GPLv3, the owner's project), or other projects, with the source named. Code from any of them is never copied. Every outside source goes in `THIRD_PARTY.md`.

**Never invent a fact.** A pin not found in a source or measured on the board is unknown: ask the contributor, or leave the item marked unverified. Guessing is how a speaker stays silent (the amplifier enable) or a board refuses to boot (GPIO0).

## Steps

1. **Gather facts.** Get the schematic, the vendor's pin table, part numbers for panel, touch, and audio, the chip with its flash and PSRAM, and photos of both sides. Fill every field of `lg_board_t` from `docs/BOARDS.md`, each with its source. Done when every field has a value or `LG_PIN_NONE`, and a source.

2. **Choose the code and write the profile.** Pick an unused three-character code (`A-Z`, `0-9`). Add an entry to `BOARDS[]` in `components/lg_board/src/lg_board.c` with a comment per fact, and the code to the `boards` table of `tools/bench_devices.json` with its chip `target`. Set every pin field; an unset one is GPIO0. On an ESP32-S3 with octal PSRAM, GPIO33 to 37 are off limits. Add the board to the supported-boards table in `docs/BOARDS.md` as **untested**. If the chip is a target no firmware builds yet, stop and ask the owner. Done when the profile compiles and no pin field is left unset.

3. **Add a driver only if a part is new.** Add the `kind` enum value in `lg_board.h`, then extend `lg_bsp` the way it already works: a `panel_cmd_t` init table and a `kind` branch in `lg_bsp_display.c`; a start and `read_raw_locked` branch in `lg_bsp_touch.c`; a `kind` branch in `lg_bsp_audio_start`. No board-specific `if` beyond `kind`. No UI headers in `lg_bsp`, no driver headers outside it (D27), no LVGL (D55). Done when `python tools/check_layers.py` passes.

4. **Build everything.** Use the `build` skill: `python tools/gen_secrets.py` once, then plain `python tools/build.py`, not one target. Done when every `RESULT` row is `OK` with zero warnings.

5. **Flash and bring up.** Add the contributor's device to their own `devices` list, then use the `flash` skill: `--firmware tests` first, then the handheld firmware. Walk the bring-up checklist in `docs/BOARDS.md` item by item, with `python tools/console.py <board> <command>` and `python tools/serial_capture.py`. Each item needs its evidence: text logs, and a photo of the screen for anything shown (D23). Mark items that do not apply (no audio, no microphone, no supply sense, no GPS) as such, with the reason. Done when every applicable item passes and its evidence is saved.

6. **Submit.** Add a `CHANGELOG.md` entry and any `THIRD_PARTY.md` rows. Fill in `.github/pull_request_template.md` with the evidence and a "not tested" list. Leave the status **untested**; the owner marks it verified. Do not commit the contributor's `devices` entries or touch `docs/DECISIONS.md`. Done when the pull request checklist is complete.

## When bring-up fails

| Seen | Likely cause |
|---|---|
| Black screen, backlight off | `backlight` pin or `backlight_active_high` wrong. |
| Backlight on, nothing drawn | Panel `kind`, SPI pins, `dc`, or `spi_hz` too high. |
| Red and blue swapped | Flip `bgr`. |
| Negative image | Flip `invert`. |
| Text mirrored or upside down | `mirror_x`, `mirror_y`. |
| `[UI] Touch controller not responding` | Touch pins, I2C address (`i2cscan`), or `rst`. |
| Touches land in the wrong place | Resistive: recalibrate from Settings. Capacitive: `swap_xy`, `mirror_x`, `mirror_y` from the `touch` output. |
| Ghost touches or missed taps (resistive) | `pressure_threshold`, or `irq_usable` on a pin with no pull-up. |
| `cue` accepted, no sound | `amp_enable` or `amp_active_low`; a codec's volume register reset to silence. |
| `power` far from the meter | `supply_divider_milli`, or the pin is ADC2. |
| No `[NET] Online` | Not a board problem unless others join: check the AP and that both were built from the same `lg_secrets.h`. |
