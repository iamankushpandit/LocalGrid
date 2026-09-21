# Boards

How LocalGrid supports a board, which boards it supports today, and how to add one. LocalGrid is an offline network (D19). Infrastructure boards are APs (D43), and handhelds are the touchscreen devices people carry.

> **Not open yet.** LocalGrid has no licence, so outside contributions cannot be accepted until the owner chooses one. See [`CONTRIBUTING.md`](../CONTRIBUTING.md). This guide is ready for when they can.

## Supported boards

`components/lg_board/src/lg_board.c` is the source of truth. This table can fall behind it; when they disagree, the code wins. New profiles may be there before they are here: see `lg_board.c` for current profiles.

| Code | Board | Chip | Role | Panel | Touch | Audio | Battery sense | GPS UART | Status |
|---|---|---|---|---|---|---|---|---|---|
| `ELG` | Elegoo ESP32 dev board | ESP32 | AP | none | none | none | none | none | verified |
| `HY3` | Hosyond 3.2in (LCDWIKI E32R32P) | ESP32 | handheld | ST7789P3 240x320 | XPT2046 resistive | internal DAC, listen-only | GPIO34, 2:1 | none | verified (volume ceiling not set by ear) |
| `F4B` | Freenove FNK0104B 2.8in | ESP32-S3 | handheld | ILI9341 240x320 | FT6336U capacitive | ES8311 codec, speaker and microphone | GPIO9, 2:1 | GPIO44 RX, GPIO43 TX | verified |
| `C6L` | Waveshare ESP32-C6-LCD-1.47 | ESP32-C6 | alert unit (D66) | ST7789 172x320, 34-column gap | none; BOOT button (GPIO9) | none | none | none | untested |
| `C6T` | Waveshare ESP32-C6-Touch-LCD-1.47 | ESP32-C6 | alert unit (D66) | JD9853 172x320, 34-column gap | AXS5106L capacitive; BOOT button (GPIO9) | none | GPIO0, 3:1 | none | untested |

**Verified** means the board passed the [bring-up checklist](#bring-up-checklist) on the owner's bench with captured evidence. **Untested** means a profile exists and builds, but nobody has shown the checklist passing. A profile starts untested and becomes verified only with evidence.

## How a board is chosen at runtime

There is one firmware binary per chip, not per board (D9). The board is found at boot from the device ID:

1. Every device carries an ID of the form `LG-<role>-<board>-<10 base32 characters>`, for example `LG-H-HY3-MPT765WSFG`. The middle part is the three-character board code.
2. `tools/flash.py` writes the ID into the `lgid` partition the first time it flashes a board, from the device's `board` field in `tools/bench_devices.json`. Normal flashing never touches that partition (D22).
3. At boot the handheld and `tests/target` read the code and call `lg_board_find(code)` for the profile. Every pin, panel fact, and pixel density comes from that profile.

Consequences:

- A board code is permanent for the IDs minted with it. Changing a board's code means `python tools/flash.py <board> --erase --new-id`. Never reuse a retired code for different hardware.
- A code must be three characters from `A-Z` and `0-9` (`tools/flash.py` checks IDs against `[A-Z0-9]{3}`), unique, and the same in `lg_board.c` and in the `boards` table of `tools/bench_devices.json`.
- The `boards` entry also gives the chip `target` (`esp32`, `esp32s3`, `esp32c6`). `flash.py` refuses to flash a board whose chip does not match.
- AP firmware does not read a profile today. An AP board needs only a `boards` entry with the right target. Its supply sense is set with `CONFIG_LG_NODE_SUPPLY_SENSE_GPIO` (see the `power` skill).

## What a board profile is

A profile is one `lg_board_t` entry in the `BOARDS[]` table in `lg_board.c`. It is data only: no code, no conditionals. Drivers in `lg_bsp` read it, and screens read the screen size at runtime (D9) and style through the theme (D10). Pixel values and colours appear only in board profiles and theme tables.

Two rules apply to every field:

- **Set every pin field.** An unset field is zero, and zero is GPIO0, the boot button. Write `LG_PIN_NONE` for anything the board does not have.
- **Say where each value came from** in a comment beside it: the vendor's schematic or pin table, a datasheet, Braino's measured facts, or your own measurement. Say which values were checked on the board and which were not (see the `HY3` volume ceiling for the tone).

### Where to find each value

| Source | What it is good for |
|---|---|
| Vendor schematic or pin table | Pins, which bus each part is on, divider resistors, amplifier enable lines. |
| Controller datasheet | Panel native size, I2C addresses, register meanings, colour order defaults. |
| Braino (the owner's project, github.com/iamankushpandit/Gume) | Facts measured on the `HY3` and `F4B` boards. Restate facts; never copy code (see [Provenance](#provenance)). |
| Your meter and your eyes | Divider ratios, backlight polarity, colour order, mirroring, and anything the vendor gets wrong. |

### Top level

| Field | Meaning | Where to find it |
|---|---|---|
| `code` | Three-character board code used in device IDs. | You choose it; check it is unused. |
| `name` | Human name, with the vendor's model number. | The vendor's listing. |
| `boot_button` | GPIO of the BOOT button. | Schematic. `0` on the classic ESP32 and S3 boards, `9` on the ESP32-C6. |
| `role` | `LG_ROLE_HANDHELD` (the default, zero) or `LG_ROLE_ALERT_UNIT`: the same handheld firmware with only alerts, SOS, and status (D66). | The owner's decision for the board. |
| `n_buttons`, `buttons[]` | Physical buttons, up to `LG_BOARD_BUTTONS_MAX`: `gpio`, `active_low`, `pull_up`, and the `lg_action_t` bits a short press (`on_press`) and a hold (`on_hold`) drive (SOS, I'm safe, Cancel, Read). Rows past `n_buttons` are never read. A builder wires more buttons by adding rows, not code. | Schematic for the board's own keys; any free GPIO for added ones. |
| `sibling`, `probe` | Two boards sold under one name with different wiring: `sibling` names the other profile, and `probe` gives I2C pins and up to two addresses that only this board answers on. `lg_bsp_board_resolve()` checks at boot and uses the right profile, logging `[BSP] Board probe`. | Schematics of both boards: a part only one of them has. |
| `supply_sense` | ADC1 pin behind a resistor divider, or `LG_PIN_NONE`. ADC2 is unusable while Wi-Fi runs. | Schematic. |
| `supply_divider_milli` | Divider ratio in thousandths (`2000` is 2:1). | Resistor values on the schematic, then confirmed: `power` against a multimeter. |
| `gps_rx`, `gps_tx` | A free UART connector for a GPS module (D65): `gps_rx` takes the module's TXD. `LG_PIN_NONE` if there is nowhere to wire one. A pin here is a connector, not a promise a module is fitted. | Schematic. The pins must not be the serial console's UART. |
| `lora_rx`, `lora_tx`, `lora_reset` | A free UART connector and a spare GPIO for a LoRa module (D71, D76): `lora_rx` takes the module's TXD, `lora_tx` goes to its RXD, and `lora_reset` pulls its RST low (or `LG_PIN_NONE`, and the firmware falls back to `AT+RESET`). `LG_PIN_NONE` where there is nowhere to wire one, which is every board but the FNK0104B. The module needs 3.3 V and never runs without its antenna. | Schematic. Not the console UART, and not the GPS UART: a board with a GPS needs a second free port. |

### `panel` (`lg_panel_profile_t`)

| Field | Meaning | Where to find it |
|---|---|---|
| `kind` | `LG_PANEL_ILI9341`, `LG_PANEL_ST7789`, `LG_PANEL_ST7796`, `LG_PANEL_JD9853`, or `LG_PANEL_NONE`. A controller not listed needs a driver (step 3 below). | The panel's marking or the vendor's listing; confirm by reading the ID register or by what works. |
| `native_width`, `native_height` | The panel in its native orientation. Portrait on every board so far. | Datasheet. |
| `mosi`, `miso`, `sclk`, `cs`, `dc`, `rst` | SPI wires. `miso` matters when a resistive touch controller shares the bus. `rst` may be `LG_PIN_NONE` if tied to the chip's reset. | Schematic. |
| `spi_hz` | SPI clock. `40000000` on both handhelds. | Start at the vendor's example value; lower it if the picture tears or corrupts. |
| `bgr` | Colour element order. | By eye: if red and blue swap, flip it. |
| `invert` | Display inversion on. | By eye: a negative image means flip it. |
| `mirror_x`, `mirror_y` | Mirrors needed for upright portrait. | By eye: text reads backwards or upside down. |
| `backlight`, `backlight_active_high` | Backlight GPIO and polarity. | Schematic; confirm the screen lights. |
| `px_per_10mm` | Pixel density, used to size touch targets in millimetres. | Pixel diagonal divided by the diagonal in cm. 240x320 is 400 px diagonal; at 3.2 in (8.13 cm) that is 49. |
| `x_gap`, `y_gap` | Where the glass starts in the controller's memory: a 172-wide panel on a 240-column controller is 34 columns in. `0` when the panel fills its controller. | Vendor example (its `set_gap` or column offset); a picture shifted sideways with garbage at one edge means it is wrong. |

### `touch` (`lg_touch_profile_t`)

| Field | Meaning | Where to find it |
|---|---|---|
| `kind` | `LG_TOUCH_XPT2046_SPI` (resistive, on the display's SPI bus), `LG_TOUCH_FT6336_I2C` or `LG_TOUCH_AXS5106_I2C` (capacitive), or `LG_TOUCH_NONE`. A controller that does not answer at start leaves the board on its buttons. | Vendor listing, `i2cscan` for an I2C controller. |
| `cs` | XPT2046 chip select. | Schematic. |
| `irq`, `irq_usable` | Touch interrupt pin, and whether it can be trusted. Set `irq_usable = false` on a pin with no pull-up (the `HY3`'s GPIO36): pressure alone then decides a press. | Schematic; input-only pins 34 to 39 have no internal pull-ups. |
| `sda`, `scl`, `rst`, `i2c_addr` | I2C controller wiring and address. The same bus carries an audio codec's control. | Schematic; `i2cscan` confirms the address. |
| `pressure_threshold` | XPT2046 pressure below which nothing is pressed. `350` on the `HY3`. | Start at 350; raise it if ghost touches appear, lower it if light taps are missed. |
| `swap_xy`, `mirror_x`, `mirror_y` | Capacitive only: maps the controller's native frame onto the portrait screen. Resistive panels are mapped by calibration instead. | The `touch` console command: tap each corner and compare raw and screen coordinates. |

### `audio` (`lg_audio_profile_t`)

| Field | Meaning | Where to find it |
|---|---|---|
| `kind` | `LG_AUDIO_DAC` (classic ESP32 internal DAC into an amplifier), `LG_AUDIO_ES8311_I2S` (codec), or `LG_AUDIO_NONE`. The ESP32-S3 has no DAC. | Schematic. |
| `speaker` | DAC pin for a bare DAC path, or `LG_PIN_NONE`. Never set on a codec board. | Schematic. |
| `codec_addr` | Codec's address on the touch I2C bus, `0` if none. | Datasheet; `i2cscan`. |
| `i2s_mclk`, `i2s_bclk`, `i2s_ws`, `i2s_dout`, `i2s_din` | Codec wires. `i2s_din` set means the board can record (push-to-talk sender, D61). `LG_PIN_NONE` on a DAC board. | Schematic. |
| `amp_enable`, `amp_active_low` | The amplifier's enable line and polarity. Easy to miss and silent when missed: the speaker stays mute however well the rest works. Check it is not also declared as an LED. | Schematic. Both current audio boards are active low. |
| `max_volume` | Ceiling set by listening, not from a datasheet. | Your ears, at the loudest volume, without distortion. Say in the comment if it is a starting point. |

On the ESP32-S3 with octal PSRAM, GPIO33 to 37 are PSRAM lines: never assign them.

## Adding a board

Every step has a "done when". Do them in order. The `new-board` skill (`.claude/skills/new-board/SKILL.md`) walks the same path with an AI assistant.

### 1. Gather facts

Collect the schematic, the vendor's pin table, the panel and touch controller part numbers, the audio parts, and clear photos of both sides of the board. Note the chip and module (flash and PSRAM size). For each fact, write down its source.

Done when every field in the tables above has a value or `LG_PIN_NONE`, and a source.

### 2. Add the profile and the code

- Add an `lg_board_t` entry to `BOARDS[]` in `components/lg_board/src/lg_board.c`, with a comment per fact giving its source.
- Add the code to the `boards` table in `tools/bench_devices.json` with a `description` (panel, touch, USB bridge, flash, PSRAM) and the chip `target`.
- If the chip is a target no firmware builds yet, the firmware projects need the target added to their `targets` in `bench_devices.json` and an `sdkconfig.defaults.<target>`. Raise this with the owner first: it changes the build matrix for everyone.
- Add a row to [Supported boards](#supported-boards) with status **untested**.
- Add any fact taken from another project to `THIRD_PARTY.md`.

Done when the profile compiles and every pin field is set.

### 3. Add a driver, only if a part is new

If the panel, touch controller, or codec is one `lg_bsp` already drives, skip this step. Otherwise add it to `components/lg_bsp` following the existing pattern:

- Add a value to the `kind` enum in `lg_board.h` (`lg_panel_kind_t`, `lg_touch_kind_t`, or `lg_audio_kind_t`) and any new profile fields it needs. A new field must default safely to zero or be set in every existing profile.
- Panels: `lg_bsp_display.c` keeps each controller's power and gamma sequence as a `panel_cmd_t` table (`ILI9341_2_INIT`, `ST7789_INIT`) sent through `send_table`, and branches on `kind` only to choose the table and its order around `esp_lcd_panel_init`. Add a table and a branch; keep addressing and pixel transfer on `esp_lcd`.
- Touch: `lg_bsp_touch.c` branches on `kind` in start and in `read_raw_locked`. A new controller returns raw readings in `lg_bsp_touch_raw_t`; capacitive ones are mapped by the profile, resistive ones by calibration.
- Audio: `lg_bsp_audio.c` branches on `kind` in `lg_bsp_audio_start`. Every sound is synthesised; there are no samples.
- Keep the layers (D27): `lg_bsp` is the only UI-side code that includes ESP-IDF drivers, it never includes UI headers, and no screen learns which board it runs on. LVGL is retired (D55).
- Write the driver from the datasheet. Restate facts from other projects; do not copy their code.

Done when the driver needs no board-specific `if` beyond `kind`, and `python tools/check_layers.py` passes.

### 4. Build every target warning-free

```powershell
. C:\esp\v6.1\esp-idf\export.ps1        # or source export.sh on Linux and macOS
python tools/gen_secrets.py             # once; the file is gitignored
python tools/build.py
```

`lg_board` and `lg_bsp` compile into several firmware types and both chips, so build everything, not just your board's target.

Done when the `RESULT` table shows `OK` on every row and the layer check passes before and after.

### 5. Flash and run the bring-up checklist

Add your board to the `devices` list in your copy of `tools/bench_devices.json` (`name`, `port`, `role`, `board`, `firmware`, `device_index` for a handheld, and an empty `id`), then flash:

```powershell
python tools/flash.py <your-board> --firmware tests   # on-board tests first
python tools/flash.py <your-board>                    # then the handheld firmware
```

Work through the [bring-up checklist](#bring-up-checklist).

Done when every applicable item passes and its evidence is saved.

### 6. Submit with evidence

Open a pull request using the template. Attach the evidence the checklist asks for, list what is not tested, and leave the board's status as **untested** unless the owner has seen the evidence. Your `devices` entries describe your bench, not the owner's: do not commit them unless the owner asks.

Done when the pull request checklist is complete.

## Bring-up checklist

You need, at minimum: the new board, one AP (any ESP32 flashed with `firmware/node`), and one known handheld (`HY3` or `F4B`) for messages and push-to-talk. Run console commands with `python tools/console.py <board> <command>`. Opening a port resets most boards, so give a handheld about 30 s to rejoin before judging network items. Capture serial output with `python tools/serial_capture.py --ports COMx --seconds 60`.

Photograph the screen for every item that shows something (D23): serial output is an addition, never the only evidence.

| # | Item | How to prove it | Evidence |
|---|---|---|---|
| 1 | On-board tests pass and show on screen (D23, D24). | `python tools/flash.py <board> --firmware tests` ends with `LG_TESTS_RESULT: PASS`; the results are on the panel. | Serial log; photo of the results screen. |
| 2 | Handheld boots and shows the self test. | Boot log has `[TEST] Self test:` passing and `[UI] Launcher ready: <w>x<h>` at the panel's size. The self test result is reachable from Status. | Boot log; photo of Status. |
| 3 | Colours, orientation, and backlight are right. | Launcher text reads upright and left to right, with no negative image; beside a known handheld on the same screen, the colours match. | Photo. |
| 4 | Touch calibration (resistive) or mapping (capacitive). | Resistive: calibration runs at first boot and logs `[UI] Touch calibration saved`; rerun it from Settings. Then `touch 20` and tap all four corners and the centre: each `raw x,y -> screen x,y` line lands within a few pixels of where you tapped. Capacitive: same `touch` run with no calibration. | The `touch` output with where you tapped. |
| 5 | Every screen fits the runtime size (D9, D10). | Open each: `screen home`, `screen status`, `screen messages`, `screen groups`, `screen settings`, `chat <device>`; `ui kb on` and `ui page letters`, `numbers`, `emoji`, `shift` for the keypad and keyboard (D59); tap the padlock for the lock screen and hold to unlock (D62); receive `send all` and `send urgent` from the other handheld for the alerts. Nothing clipped, overlapping, or off screen; keys are pressable with a finger. | A photo of each screen. |
| 6 | Sound cues. Skip if the board has no audio. | `tone 1000 500`, `cue sent`, `cue received`, `cue urgent`; `volume low` then `volume high` changes loudness; `volume off` silences everything but `urgent`. | Serial replies; a short video or a note of what was heard. |
| 7 | Push-to-talk playback (D61). Skip if no audio. | From the known `F4B`, hold the talk bar in a 1:1 chat with the new board: the new board plays the voice clearly. | Serial logs from both; a note on clarity. |
| 8 | Microphone, if the board can record. | `mic level 5` shows speech well above silence; `mic loop 3` plays back what was recorded; push-to-talk from the new board is heard on the other handheld. | Serial output; a note on what was heard. |
| 9 | Battery badge, if the board has supply sense. | `power` reports `supply <mV>`; measure the same point with a multimeter. Within 100 mV, or correct `supply_divider_milli`. The badge on screen shows a percentage. | `power` output, the meter reading, a photo of the badge. |
| 10 | GPS, if the profile has a GPS UART and a module is fitted (D65). | `gps` reports sentences and, outdoors, a fix. | `gps` output. |
| 11 | Wi-Fi join and registration with an AP. | Log shows `[NET] Session open to node` then `[NET] Online`; `status` shows the link and grid time; `nodes` lists the AP. | Serial log. |
| 12 | A 1:1 message both ways. | `send <device> bring-up test` from each handheld to the other; both screens show it, and the sender logs `[MSG] Delivered to the other handheld`. Use a throwaway text: 1:1 plaintext is never logged by firmware and should not be in your evidence either. | Serial logs; photos of both chats. |
| 13 | Optional: a short chaos run including the board. | On your own bench: `python tools/chaos.py --hours 0.2 --live`. See the `chaos` skill. | The run's report from `chaos-runs/`. |

Attach logs as text files, not screenshots of a terminal. Remove nothing but hardware addresses (D21; the tools already hide them).

## Provenance

Board facts are facts: a pin number or a divider ratio can be restated from a datasheet, a vendor's pin table, or another project, with the source named. Code is different.

- Braino (github.com/iamankushpandit/Gume) is GPLv3. It is the owner's project and the source of the measured `HY3` and `F4B` facts. You may restate facts from it; do not copy its code, which would bring GPLv3 terms with it.
- The same holds for any other project: TFT_eSPI, vendor example sketches, Arduino libraries. Restate facts and write the code against ESP-IDF.
- Record every outside source in `THIRD_PARTY.md`: the project, its licence, which file here uses it, and what was taken. Name the source in a comment beside the facts in `lg_board.c` too.
