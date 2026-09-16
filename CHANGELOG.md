# Changelog

All notable changes to LocalGrid are recorded here, newest first.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions stay below 1.0 while the project is a prototype.

## [Unreleased]

### Added (build tooling)
- `tools/build.py` builds firmware without opening any serial port. It can build every firmware type for every target, chosen types (`--firmware`) or chips (`--target`), or what named boards or a role run. Options: `--clean` starts from scratch, and `--list` shows the build matrix with each last result (size and free app space).
- It shares `tools/flash.py`'s build step, which refuses warnings, and its run lock.
- Build skill `.claude/skills/build`. The bench skill now points to it instead of spelling out `idf.py` commands.

### Added (P5: handheld joins the grid)
- `firmware/handheld` is the first handheld firmware, for the Hosyond (ESP32) and the FNK0104B (ESP32-S3).
- Its network service task owns Wi-Fi, the node session, and `lg_client`:
  - Single-channel active scan. Nodes are recognised by the beacon element's grid discriminator.
  - Node selection by signal, stickiness, load, and backbone health (design review answer 10).
  - Static address 192.168.(4+n).(100+device), TCP to .1:7300, then REGISTER.
  - PING every 10 s. A Wi-Fi drop, 25 s without PONG, or a socket error starts a new search, backing off from 1 s to 10 s.
  - The boot counter is committed to NVS before any frame is sent. The X25519 key is created once, after Wi-Fi starts, and kept in NVS namespace `lghh`.
- The home screen shows:
  - this handheld's roster name and ID;
  - connection state, node, signal, and address;
  - grid time, or the D6 restriction when it is not set;
  - the handhelds it has heard about, with no placeholders;
  - nodes in range: tap one to use only that node, or Automatic.
- The screen and the service meet only in `hh_service.h`, a status snapshot plus a command queue (D27).
- `lg_client_ping()` and `last_pong_ms` in `lg_core`, because nodes close sessions silent for 30 s. New simulator test `test_ping_pong`.
- Handheld device index in the identity partition (`device_idx`), shown in `LGID:` output.
  - `tools/flash.py --update-identity` rewrites identity from the device map without erasing settings.
  - Bench map: FNK0104B is device 1 ("Handheld 1") and Hosyond is device 2. Both are assigned the new `handheld` firmware type.
- `LG_SECRET_DISCRIMINATOR` in `lg_secrets.h`, added by `python tools/gen_secrets.py --update` without changing keys. Handheld firmware never references the backbone key, and `tools/check_layers.py` fails if it does.
- `lg_ui_widgets` in `lg_ui`: column, row, label, text, card, and button helpers.
- `lg_proto_handheld_ip()` in `firmware/common/lg_proto_config.h`.

### Fixed (P5)
- A handheld that rebooted and rejoined the same node lost its first `REGISTER_ACK`. The node's `sess_send()` wrote to the first session slot for that device, which was the previous, still-open session. The handheld waited 5 s and registered on the retry. Replies now go to the newest session for the device.
- `tools/serial_capture.py --reset-at` did not reset the ESP32-S3 once its port was open. Windows' `usbser.sys` driver sends RTS changes only when DTR is written too, so both resets now rewrite DTR after RTS, as esptool does.

### Fixed (P6: chat screen room)
- Messages were invisible while the keyboard was up. Mock-ups at the true 240x320 size showed the reason: a header, a four-button controls row, the entry row, and a 144 px keyboard left the message list about 38 px, less than one bubble. Now the keyboard takes 40% of the panel (52% for the taller emoji pages), the controls row hides while typing, and the message list has a one-bubble floor so the column can never overflow. With the keyboard down the list gets the whole panel.
- The emoji page had 48 keys in 144 px, about 14 px each, against a 39 px fingertip. It is now 24 per page in rows of six, with a More key for the second page.
- The chat screen handles its own keys, because LVGL's handler would have typed "More" into the message. It keeps the built-in behaviour for letters, capitals, symbols, backspace, cursor keys, send, and hide.
- Mock-ups of every handheld screen, drawn at 240x320 with the firmware's palette, fonts, padding, and touch sizes, and annotated with the pixel height each region really gets.

### Added (P6: emoji, notifications, and honest rejection reasons)
- Emoji: `tools/build_emoji_font.py` downloads Noto Emoji (monochrome, SIL Open Font License 1.1), pins weight 400, subsets it to 48 curated emoji, and converts it to an LVGL font with `lv_font_conv`. The generated font, the licence text, and a matching `lg_emoji.h` are committed, so an ordinary build needs neither Node nor a download.
  - The theme's body and small fonts fall back to the emoji font, so one label can carry letters and emoji.
  - The chat keyboard has an Emoji page (decision D8); its ABC key returns to letters.
  - A colour emoji font was rejected: LVGL draws single-colour glyphs, and the colour builds are 3 to 10 MB.
- Notifications: a message that arrives while another screen is up shows a banner on LVGL's top layer naming the sender with a preview; tapping it opens that conversation. Unread counts appear on the Messages button and on each conversation row, and clear when the conversation is opened. No sound yet: the speakers are not driven until the audio milestone.
- Message states now say why: a rejection from the grid is reported as offline recipient, not a group member, rate limited, clocks disagreeing, unknown target, or refused body, in one place used by both the screens and the console.

### Added (P6: chat screens)
- A Messages button on the home screen opens the conversation list: Everyone (broadcast), each group this handheld belongs to, and each handheld it has heard about, with online state. A non-member never sees a group it cannot use.
- The chat screen shows the conversation's messages oldest first: received ones on the left, ours on the right with their state (sending, sent, delivered, rejected, or the reason it was refused), each with a clock reading and the sender's name. Urgent messages are outlined in the warning colour.
- Writing: a one-line text area capped at 240 bytes, a Send button, and LVGL's keyboard in the lower half of the screen (decision D8) with letters, capitals, numbers, and symbols pages. Emoji need a font this build does not carry.
- The Everyone conversation has an Urgent toggle, the one thing that can be sent while grid time is unset (D6). Hints explain when nothing can be sent: not on the grid, or grid time unset.
- Screens read the message list and status snapshot and send only through `hh_service.h` (D27).

### Added (P6: messaging on handhelds, service layer)
- The handheld service sends and receives text: 1:1 (end-to-end encrypted), group, and broadcast, through `lg_client`.
  - Sending goes through a small queue drained by the network task, which stays the only owner of `lg_client`.
  - A 24-message ring holds what was sent and received, with each of our messages tracked from sending through accepted, delivered, or rejected, and refusals explained (grid time unset, outbox full, or not allowed).
  - The snapshot now carries the roster's groups and whether this handheld belongs to each.
- Handheld console commands for messaging, so it can be exercised before the screens exist: `send <device|group|all|urgent> <text>`, `msgs [count]`, and `groups`.
- Verified on hardware, with grid time still unset:
  - An urgent broadcast from Handheld 1 reached Handheld 2 in about 250 ms. The sender shows it accepted; the receiver shows it received.
  - A 1:1 message and a group message were both refused, each explaining that grid time is unset so only urgent broadcasts go out (decision D6).
  - `groups` reports the roster correctly: Handheld 1 belongs to FAMILY and LEADERS, not KIDS.
- Device IDs are accepted only when whitespace follows the ID, which proves it arrived whole. A serial read can end mid-line, and a split ID was reported as a mismatch against the device map, making a correct board look like the wrong one. An intermediate version of this fix required a line end and matched nothing, because the firmware's line continues with `role=` after the ID; it was caught by `--identify` returning no answer on all five boards.
- `tools/console.py` waits for a board to announce itself before typing, and retries writes. The FNK0104B's native USB port refuses writes while it re-enumerates after the reset that opening the port causes, which had failed every command after the first.

### Added (D28: serial console on every device)
- Handhelds now have a full console, not just `id`: `status` (device, link, signal, address, grid time, node choice, memory, problem), `nodes` (nodes in range with signal, load, and backbone health), `people` (handhelds heard about), `node <index>|auto`, `scan`, `reconnect`, `time`, and `reboot`. It runs over the Hosyond's UART and the FNK0104B's native USB.
- Node consoles gain read-only `config`: device ID, node index and name, SSID, channel, address, DHCP range, grid name, time zone, whether an admin password is set with its iteration count, and grid time quality. No secrets are printed, and grid settings still change only on the master's admin page.
- `tools/console.py <board|all|N|H> <command>` asks any bench board a console command by name, checking its device ID first, masking MAC addresses, and sharing the flash run lock.
  - Opening a port resets the board, so the tool waits for the `grid>` prompt before typing and retries once. Keystrokes sent while a board is still booting are lost.
  - Replies hide ESP-IDF log lines; `--raw` keeps them.
- Verified on hardware: `config` and `status` answered on LG-MAIN over its UART, and `status`, `nodes`, and `people` answered on both handhelds, including the FNK0104B's native USB port. The node reported grid name, time zone, and that an admin password is set with 4,000 iterations, with no secrets in the output.

### Fixed (bench tooling and privacy)
- The node logged a joining station's MAC address, against decision D21. It now logs the association id and the disconnect reason only.
- `tools/serial_capture.py` releases every port it already opened when a later port cannot be opened. A five-port capture had failed on a port that disappeared when a board was replugged, and the skipped cleanup left the boards it had opened held in reset. A node stuck that way looks like "no LocalGrid node in range" on a handheld.
- The Elegoo boards' CP2102 chips carry no unique serial number, so Windows renumbers their COM ports after a replug. The device map's port hints for MAIN and NORTH were corrected; `flash.py --identify` reports which board answers on each port.

### Verified on hardware (P5)
- Core tests on the FNK0104B: 373 checks, 0 failures, including `test_ping_pong`.
- Both handhelds join, register, and show each other across nodes: the Hosyond on LG-MAIN and the FNK0104B on LG-SOUTH. Boot to Online takes 2.8 s on the Hosyond and 2.9 s on the FNK0104B.
- After a reboot on the same node, the FNK0104B's registration succeeded on the first attempt, and LG-SOUTH logged `Replacing stale session ... for device 1`, confirming the `sess_send()` fix.
- After a reboot the Hosyond rejoined on another node (LG-NORTH), registering on the first attempt 2.2 s after boot. The other handheld saw it move, and the old node closed the stale session without marking it offline.
- Heap while online: Hosyond 155 KB free, 147 KB lowest; FNK0104B 208 KB free, 206 KB lowest. Node heap is unchanged.

### Changed (P5)
- Node DHCP serves .2 to .99 only. The default pool ran to .101 and could give a phone a handheld's static address.
- `tools/check_layers.py` also checks the handheld's service and UI folders.

### Changed (layering)
- Decision D27: the UI layer stays separate from the infrastructure. Display and touch drivers moved out of `lg_ui` into a new board-support component, `components/lg_bsp` (`lg_bsp_display`, `lg_bsp_touch`).
- `lg_ui` now holds only LVGL glue, the theme, the pointer device (`lg_ui_input`), and the calibration screen (`lg_ui_calibrate`). It has no ESP-IDF driver dependencies.
- Node firmware no longer builds the handheld UI stack. `firmware/node/CMakeLists.txt` had listed the whole `components/` folder, so the node build compiled `lg_board`, `lg_bsp`, `lg_ui`, and a downloaded copy of LVGL.
  - LVGL was compiled but not linked: the binary went from 950 KB to 948 KB.
  - The node now lists only `lg_core`, `lg_crypto`, and `lg_identity`, and sets `COMPONENTS main`. A clean build compiles exactly those three LocalGrid components and downloads nothing.
- `tools/check_layers.py` enforces D27, and `tools/build.py` runs it before and after every build. It checks:
  - UI or board-support includes or requirements in infrastructure code;
  - LVGL in `lg_bsp`;
  - driver includes in `lg_ui`;
  - UI components in a node build folder.
- Decision D26: voice is tested one way on the bench (FNK0104B talks, Hosyond listens) until a second FNK0104B arrives.

### Changed (testing policy)
- Decision D25: all testing runs on the ESP32 boards. The planned laptop test client (`lgctl.py`), its emulated handhelds, and the laptop load scripts are dropped from the design review, decisions, and `AGENTS.md`.
- On hardware, group membership is checked with the two handhelds in two passes. Four-user and load cases stay in the on-board simulator until more ESP32 boards are added.

### Added (handheld display)
Handheld displays come up for decision D23; test builds now draw their results on the handheld's own screen.
- `components/lg_board` holds the board profiles for the Elegoo ESP32, Hosyond 3.2in (E32R32P), and Freenove FNK0104B: panel type, SPI pins, colour order, inversion, mirroring, backlight polarity, pixel density, and touch wiring.
  - Profiles are selected at boot from the board code in the device ID, so one binary serves every board with the same chip.
  - Pin facts are credited to the owner's Braino measurements.
- `components/lg_ui` brings up the panel and runs LVGL 9.5:
  - Both panels are driven by ESP-IDF's ST7789 panel object. Controller registers come from TFT_eSPI's ST7789 and ILI9341_2 command sequences.
  - A 24-line DMA draw buffer, an LVGL task, and a recursive lock guard every LVGL call.
  - The screen is cleared before the backlight turns on.
- `lg_theme` holds the Phase 1 "Terminal" theme. Colour roles follow Braino's palette; fonts, padding, gaps, and the 8 mm minimum touch target are sized from the screen at runtime.
- The `tests/target` results screen shows the device ID, board name, the three test groups (waiting, running, then passed or failed counts), and a large PASS or FAIL with the check count and lowest free memory. Layout uses only percentages, content sizing, and theme values.
- The test app prints free heap and largest block before the tests.
- Touch in `lg_ui` (milestone P4), started by the display code. Both controllers feed an LVGL pointer device.
  - FNK0104B: FT6336U over I2C, mapped by the board profile.
  - Hosyond: XPT2046 on the display's shared SPI bus. A press is decided by pressure (IRQ 36 is unusable) and positions are averaged.
  - Resistive panels get a three-target calibration wizard. The affine fit is saved in NVS namespace `lgui`, which normal flashing keeps.
- Touch check in the test app: after the results, a tap opens a screen with five numbered targets. Each target turns green when hit, a dot and coordinates follow the finger, and there are Results and Calibrate buttons.
  - Serial logs each hit and `TOUCH_CHECK: PASS` once all five are hit.
  - The Hosyond calibrates first if it has no saved calibration.
- The test app times each test group, on screen and over serial.
- Decision D24: product handheld firmware keeps the self test and its results screen.

### Changed (handheld display)
- LVGL allocates through the C heap (`CONFIG_LV_USE_CLIB_MALLOC`) instead of its static 32 KB pool, and LVGL examples are no longer compiled.
- The simulated grid reserves its 136 KB of memory once at boot (`sim_reserve()`), before the display starts. On the Hosyond, starting the display first left a largest free block of 128 KB: every `sim_create` failed and the messaging tests could not run (118 checks, 12 failures).

### Verified on hardware (handheld display)
- With the display running, both handhelds pass 360 checks with 0 failures:

  | Board | Chip | Minimum free heap |
  |---|---|---|
  | FNK0104B (`LG-H-F4B-9DTCV8M0R1`) | ESP32-S3 | 192 KB |
  | Hosyond (`LG-H-HY3-MPT765WSFG`) | ESP32 | 104 KB |

- The test app is 648 KB on ESP32-S3 and 636 KB on ESP32, with 58–59% of the app partition free.
- Serial shows `[UI] Hosyond 3.2in (E32R32P): 240x320, backlight on` about 0.8 s after boot.
- Owner confirmed both screens show the results screen ending in PASS.
- Both touch controllers answer at boot: `[UI] Touch FT6336U ready`, and `[UI] Touch XPT2046 ready, needs calibration`.
- Owner confirmed touch works on both handhelds: every touch-check target is hit on the FNK0104B, and on the Hosyond after its calibration wizard.
- Test group times, which set what a boot-time check can include (D24):

  | Group | FNK0104B (ESP32-S3) | Hosyond (ESP32) |
  |---|---|---|
  | Core protocol, 71 checks | 0 ms | 1 ms |
  | Encryption | 36.6 s | 87.8 s: the RFC 7914 80,000-iteration PBKDF2 vector takes 79.2 s and the login-cost measurement 4.0 s, leaving about 4.6 s |
  | Messaging, simulated grid | 26.5 s | 32.3 s |

- Hosyond heap before the tests, with the simulation reserved and the display and touch running: 106.5 KB free, all of it one block.

### Fixed (handheld display)
- The test suites logged task watchdog warnings every 5 s on both handhelds. The test task kept CPU 0 busy for minutes at a priority above the idle task. The suites keep their normal priority, and the task watchdog stops watching idle tasks only while they run.
- Verified on both handhelds: 360 checks and 0 failures with no watchdog warnings. The Hosyond runs faster than before, because the warnings' backtrace printing had been taking CPU time: encryption 72.9 s (was 87.8 s), messaging 31.9 s, and PBKDF2 at 406 ms per 1,000 iterations.
- A first fix lowered the suites to idle priority. It stopped the warnings but halved their speed on the Hosyond (encryption 88 s to 173 s, messaging 32 s to 66 s), skewed the PBKDF2 timing measurement, and missed `flash.py`'s 200 s verify limit.

### Added
- Node firmware (`firmware/node`) for the three Elegoo ESP32 boards:
  - SoftAP on channel 6 with deterministic addressing (node n at 192.168.(4+n).1).
  - TCP control sessions for handhelds on port 7300.
  - ESP-NOW backbone sealed with ChaCha20-Poly1305, HELLO every 2 s, two-way link confirmation, one frame in flight at a time.
  - NimBLE broadcaster advertising the discovery payload, which is also carried in a Wi-Fi beacon vendor element.
  - Serial console with `status`, `nodes`, `devices`, `ping`, and `time` commands.
- Diagnostic echo (`LG_T_DIAG_ECHO`) that floods once and reports hop count at every node.
- Grid time announcement over the backbone; nodes adopt announced time and push it to their handhelds.
- Shared prototype configuration (`firmware/common/lg_proto_config.h`) mapping bench MAC addresses to node indexes.
- `tools/gen_secrets.py` to generate the gitignored `firmware/common/lg_secrets.h`, plus a committed template.
- `.gitattributes` enforcing LF line endings.
- This change log.
- Agent guidance: `AGENTS.md` (imported by `CLAUDE.md`), `docs/DECISIONS.md` as the single decisions list, and project skills `bench` and `protocol-change` in `.claude/skills`.
- `tools/serial_capture.py`: capture several boards at once, type console commands and pulse resets on a schedule.
- Decisions D10 (themeable UI through a theme table) and the themes section in design review answer 37.
- `tools/serial_capture.py --reset-at` to pulse board resets on a schedule.
- Milestone report `docs/milestones/P2-node-firmware.md`.
- Brand icon `assets/brand/localgrid-icon.svg` and single-color `localgrid-icon-mono.svg` (decision D18).
- Decision D17: the configuration web page is the next milestone.
- Admin web page on the master node at `http://192.168.4.1/` (plain HTTP, decision D11):
  - First visit runs setup: network name, admin password (12–64 characters), and time and time zone taken from the browser.
  - Login with a 128-bit session cookie (`HttpOnly`, `SameSite=Strict`), 30-minute idle expiry, at most 2 sessions, CSRF header plus Origin check on every change, and lockout that grows from 30 s to 5 min after 5 failed logins.
  - Dashboard with grid time and a "Use this device's time" button, manual time entry, master node stats, infrastructure links with signal, and people's presence.
  - Styling through CSS theme variables (decision D10) and the LocalGrid icon as favicon.
- `lg_pbkdf2_sha256` (RFC 8018) and `lg_ct_equal` in `lg_crypto`, with RFC 7914 test vectors and a timing measurement in `tests/target`.
- Master settings (network name, time zone, password hash) persisted in NVS namespace `lgcfg`.
- `lgbb_links()` accessor so the web page can show backbone links.
- Flashing skill `.claude/skills/flash` with `tools/flash.py` and the device map `tools/bench_devices.json`: flash one board, several, a role, or all with each board's assigned firmware type (`node` or `tests`); builds with warnings are refused; nodes are flashed one at a time to keep grid time; each board is verified over serial. Builds go to per-target folders (`build-esp32`, `build-esp32s3`).
- Device IDs (decisions D20–D22): `LG-<role>-<board>-<10 base32>` minted at first flash from MAC, time, a label, and random bytes, stored in a new `lgid` partition, and answered by the serial `id` command on node and test firmware (`components/lg_identity`). `--decode` prints the device type; `--identify` asks boards for their IDs; `--erase` factory-resets while keeping the ID; `--new-id` replaces it.
- `tools/flash.py` and `tools/serial_capture.py` set a serial write timeout; a native-USB board that is not reading its console previously blocked the tool indefinitely.
- Shared 4 MB partition table `firmware/common/partitions_4mb.csv` (factory app 1.5 MB plus `lgid`) used by node and test firmware; the ESP32-S3 test build uses the USB-Serial/JTAG console.

### Removed
- MAC addresses from `firmware/common/lg_proto_config.h`, the device map, and the node console's `nodes` output. Nodes take their index and name from the identity partition.

### Changed (MAC exposure, D21)
- Node firmware lowers the `wifi` and `BTDM_INIT` log tags to WARN; Espressif's Wi-Fi driver and Bluetooth controller otherwise print the SoftAP and Bluetooth MAC addresses at boot.
- `tools/serial_capture.py` masks any MAC-like string as `xx:xx:xx:xx:xx:xx` before printing.
- Run lock verified: a second `tools/flash.py` run is refused while a live run holds `tools/.flash.lock`; a lock from a dead process is cleared, and the lock is removed on exit.
- Holding DTR and RTS inactive before opening a port does not stop the Elegoo boards' auto-reset; any serial read of a node restarts it.

### Verified on hardware (device IDs)
- All five bench boards provisioned and answering `id`: nodes `LG-N-ELG-1ME3HN5EDP` (MAIN), `LG-N-ELG-81NNZEH78W` (NORTH), `LG-N-ELG-276YMMKRDB` (SOUTH); handhelds `LG-H-F4B-9DTCV8M0R1` (FNK0104B) and `LG-H-HY3-MPT765WSFG` (Hosyond). Both handhelds pass 360 checks with 0 failures on the new partition layout; the master kept its admin setup through reflashing.

### Known issues
- Two `tools/flash.py` runs at once collide: a leftover run from an ended session kept building while a new run started, and the new build failed with "file is being used by another process". No board was harmed. Fixed: `tools/flash.py` now takes a run lock (`tools/.flash.lock`) and refuses to start while another live run holds it; a lock left by a dead run is cleared automatically.
- Handheld screens stay blank: no firmware drives the displays yet, which conflicts with decision D23 until display bring-up lands.
- Nodes "losing links" was a bench tool fault, not firmware. Closing a serial port the plain way holds the Elegoo boards in reset through their auto-reset circuit, so a node whose port a tool had just closed stopped sending HELLOs. Proof, with NORTH's port held open as a witness: a plain close of MAIN's port made NORTH log `Link lost to node 0` within about 5 s. Setting DTR and RTS to False before closing restarts MAIN once, and it stays running (relinked in about 2.5 s). A 5-minute capture holding all ports open never failed. Fixed: `tools/flash.py` (`release_and_close()` in `query_id()` and `verify()`) and `tools/serial_capture.py` release both lines before closing. Verified: after the patched tool closed MAIN's and SOUTH's ports, NORTH opened alone linked to both within 1.7 s and 2.6 s, with tx fail, dropped, NO_MEM, and replay all 0.

### Changed (admin page and naming)
- Admin page lists only handhelds the grid has actually seen, under "Handhelds", with an empty state until one connects. Previously it listed every prototype roster entry as "NOT SEEN".
- Prototype roster names changed from fictional people (Dad, Emma, Alex, Ranger) to placeholders "Handheld 1" to "Handheld 4" until pairing lets the admin name devices.
- Decision D19: the product is "an offline network", not a camp network. Admin page subtitle, `AGENTS.md`, and the GitHub repo description updated.
- Hosted admin page preview: `tools/build_web_preview.py` combines the firmware page with `web/preview/mock-master.js`, a simulated master that follows the same setup, login, lockout, CSRF, and time rules. The firmware page gains a one-line hook that is inactive on the device.

### Changed
- On-board test app grows to 352 checks covering the diagnostic echo and time announcement.
- Backbone module symbols use the `lgbb_` prefix; `bb_init` collided with a symbol in Espressif's PHY library.

### Verified on hardware
- Three Elegoo nodes reset together form a full mesh; every link confirmed two-way within about 1.3 s of boot.
- `time set` on MAIN is adopted by NORTH and SOUTH within one announcement.
- `ping` from MAIN is reported exactly once by each other node.
- Staggered resets and a mid-run reboot: a rebooted node relinks within about 2.4–3.3 s and re-adopts grid time on link-up; tx fail, auth fail, replay, and NO_MEM counters all 0.
- PBKDF2 matches RFC 7914 vectors on classic ESP32; 228 ms per 1,000 iterations through PSA, so the admin password uses 4,000 iterations (about 0.9 s per login). Tests: 360 checks, 0 failures.
- Owner completed setup on the real admin page over `LG-MAIN`; NORTH adopted the browser-provided grid time from the master, within about 2 s of the PC clock.
- Master starts the admin page in setup mode about 1.1 s after boot; heap with the web server idle is 63.3 KB free, 56.8 KB minimum. Node binary 971 KB. Browser test is pending on the owner's phone (`docs/milestones/P3-admin-web.md`).
- Node heap with SoftAP, ESP-NOW, NimBLE advertising, and TCP server running: 81.5 KB free, 74.4 KB minimum, 73.7 KB largest block. Binary 919 KB, 40% of the 1.5 MB app partition free.

## [0.1.0] - 2026-09-15

Initial commit `3f26639`, pushed to the private GitHub repository.

### Added
- Phase 0 design review (`docs/DESIGN_REVIEW.md`) answering the 47 architecture questions, with owner decisions recorded:
  - ESP-NOW backbone between SoftAP-only nodes.
  - BLE kept in Phase 1.
  - End-to-end encrypted 1:1 messages.
  - Admin re-enters time after a full power-off; handhelds with wrong time may only receive and send urgent broadcasts.
  - One user per handheld; portrait, resolution-independent UI.
  - Plain HTTP admin page.
- `lg_core`: portable C protocol core with a 32-byte envelope, message IDs, duplicate windows, roster and groups, node routing for direct, group, and broadcast messages, handheld outbox and inbox, and key pinning.
- `lg_crypto`: X25519, HKDF-SHA256, and ChaCha20-Poly1305 over PSA Crypto, verified against RFC 7748, RFC 5869, and RFC 8439 test vectors.
- `tests/target`: on-board test app with a simulated three-node grid; 313 checks passed on all five bench boards (ESP32 and ESP32-S3).
- Prototype milestone report `docs/milestones/P1-messaging-core.md`.
