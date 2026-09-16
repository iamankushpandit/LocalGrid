# Changelog

All notable changes to LocalGrid are recorded here, newest first.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions stay below 1.0 while the project is a prototype.

## [Unreleased]

### Changed (D46: one network, "LocalMesh Access Point")
- Every AP broadcasts the same SSID, **"LocalMesh Access Point"**, and answers as **192.168.4.1/24**, so a phone that moves between APs keeps its gateway and the admin page. `lg_proto_node_ip()` ignores the index and keeps its signature. Handhelds keep static 192.168.4.(100 + device index) on every AP.
- Phone DHCP pools are split per AP so two APs never lease the same address: 12 addresses per AP index from .2 to .97 (MAIN .2 to .13, NORTH .14 to .25, SOUTH .26 to .37). Compile-time checks keep the blocks below the handheld addresses and require one block per possible index (`LG_MAX_NODES`). Twelve is below the 15-station limit, which handhelds share.
- The Wi-Fi vendor IE carries the AP name after the 12-byte discovery payload: a length byte, then up to 15 bytes. Receivers that only read the fixed part are unaffected, so `LG_DISC_VERSION` stays 1. BLE adverts keep the 12-byte payload, well inside the 31-byte legacy limit.
- Handheld service: joins with the shared SSID plus the chosen BSSID, and names each AP from the IE ("AP n" if an AP sends no name), so screens and `status` still say MAIN, NORTH, SOUTH. The static array of 16 Wi-Fi scan records is gone, about 1.3 KB of internal RAM, because candidates now come from the IE alone. Layers ok.
- `config` shows the network, this AP's DHCP block, and the BLE and PMF build switches. The admin page subtitle says "LocalMesh Access Point, served by AP MAIN", and its connection error names the network and 192.168.4.1. The discovery payload's master byte is always 0 now (D45).
- An AP and a handheld from either side of this change only work together on MAIN, whose addresses did not change. Update APs and handhelds together.
- Flashed to MAIN and verified over serial: `[NET] SoftAP "LocalMesh Access Point" (AP 0 MAIN) on channel 6 at 192.168.4.1, phone DHCP .2 to .13, PMF capable`. Both handhelds, still on older firmware, re-joined and registered as .101 and .102. Handheld firmware built for esp32 and esp32s3 with zero warnings; **not flashed**. **Not verified**: anything with more than one AP, and phone roaming. NORTH and SOUTH are unplugged while bench power is fixed.
- `tools/wifi_scan.py` and the `wifi` skill still look for `LG-MAIN`, `LG-NORTH` and `LG-SOUTH`, so they will report every AP missing until they are updated.

### Fixed (a few lost HELLOs no longer drop a backbone link)
- The owner saw `Link lost ... (no HELLO for 6000 ms)`, followed by a relink 30 ms later, in one direction only. HELLOs are ESP-NOW broadcasts with no ACK and no retries, and the classic ESP32 shares its radio between Wi-Fi, ESP-NOW and BLE advertising, so three broadcasts in a row can be lost while the neighbour is fine.
- Once a confirmed link has gone 3 s without a HELLO, the AP sends its HELLO to that neighbour as a unicast, once a second. Unicasts are retried and ACKed at MAC level, and the neighbour handles the probe as a HELLO, which repairs the other direction too. A link is dropped after 6 s with neither a HELLO nor an ACK, or after 20 s with no HELLO even if ACKs continue.
- `nodes` prints per-AP counters that survive link loss: HELLOs, gaps (a HELLO more than 4 s after the previous), longest gap, probes, ACKs, saves (links kept by ACKs), ups, losses. It also prints `tx_fail` and `rx_queue_full`. A kept link logs `[BB] No HELLO from node N for M ms, but it ACKs keepalives; link kept`.
- BLE advertising is no longer stopped and restarted to change its data. `ble_adv_update()` returns when the payload is unchanged and otherwise updates the data in place, which the controller allows while advertising.
- Built with zero warnings and running on MAIN. **Not verified**: the probe path itself, which needs a second AP.

### Added (A/B switches for BLE advertising and PMF)
- `firmware/node/main/Kconfig.projbuild`: `CONFIG_LG_NODE_BLE_ADV` (BLE advertising, D4) and `CONFIG_LG_NODE_PMF_CAPABLE` (SoftAP offers PMF). Both default on in `sdkconfig.defaults`, so normal builds are unchanged. Boot and `config` say when either is off.
- Use them to compare backbone counters with and without BLE on the radio, and handheld reason-2 disassociations with and without PMF. `docs/milestones/P7-ap-mesh.md` has the out-of-tree build and flash commands.

### Verified (D45 on MAIN, alone)
- `tests/target` on MAIN, including `test_grid_state`: `LG_TESTS: 433 checks, 0 failures, min free heap 127568 bytes`, `LG_TESTS_RESULT: PASS`.
- AP firmware flashed back with settings kept. MAIN's version 1 record migrated: `[GRID] Settings version 1 from AP 0 (set up)`, `[WEB] Admin page at http://192.168.4.1/ (configured)`, and `config` shows the grid name, time zone, and a password record with 4000 PBKDF2 iterations.
- `status` on MAIN shows `Restarts since counting began: power-on or reset 16, low supply voltage (brownout) 269`. That is consistent with the USB hub the owner saw brown out all three APs.
- After MAIN restarted, it logged 165 `wifi:no need to send deauth when softap is sending deauth` warnings within about a second, just before both handhelds rejoined. Both handhelds offer PMF. The PMF A/B should show whether PMF causes these warnings.
- **Not verified**: replication between APs, time demotion, split and rejoin. These need NORTH or SOUTH. `docs/milestones/P7-ap-mesh.md` lists the procedure.

### Changed (handheld RAM: three fixes, measured on both boards)
- **One copy instead of many.** Screens share one status snapshot and one message list (`ui/ui_snapshot.c`, drawing task only), and console commands share another pair. Before, there were 17 static status copies and 3 message-list copies, about 46 KB. The Status screen compares a signature of its lists instead of keeping a whole previous snapshot.
- **Screens exist only while shown** (`ui/ui_screen.c`). Status, Settings, the AP chooser, the conversation list and the chat screen with its keyboard are built when opened and freed when another screen replaces them. Their refresh timers stay and check for a live screen first. The top bar no longer keeps a table of bars, so screens can be rebuilt any number of times. The screen saver builds its sixty labels when it starts and frees them when touched. Touch calibration no longer returns to a screen that was freed underneath it; the Settings job builds Settings again afterwards.
- **Wi-Fi and lwIP trimmed for a station** that carries a few small frames a second: 4 static receive buffers, no A-MPDU, 12 management buffers, no SoftAP, WPA3 or enterprise support, 4 sockets.
- Measured with `[MEM]` marks and `mem`, same bench, before and after (KB of internal heap):

  | | Hosyond before | Hosyond after | FNK0104B before | FNK0104B after |
  |---|---|---|---|---|
  | Free at boot | 183 | 216 | 241 | 273 |
  | Wi-Fi driver start | −49 | −43 | −51 | −45 |
  | Screens built at start | −27 | −5 | −29 | −5 |
  | Idle, online | 58 | 123 | 113 | 178 |
  | With Settings open | 48 | 108 | 103 | 164 |
  | With a chat open | 35 | 116 | 91 | 171 |

  Leaving a screen gives its memory back: the Hosyond returns to 124 KB after Settings and a chat. Screens opened and left repeatedly from the console, including Settings, Status and chat, with no crash.
- Both handheld builds have 0 warnings and layers ok. Flashed to both handhelds, which registered through NORTH on the shared SSID.

### Verified on hardware (D45 and D46 across three APs)
- All three APs run the AP firmware. NORTH and SOUTH started from no settings, adopted MAIN's version 1 over the backbone, and kept it in flash: after a restart all three log `[GRID] Settings version 1 from AP 0 (set up)`. NORTH's `config` shows grid name LocalGrid and the shared network.
- Both handhelds joined "LocalMesh Access Point" by BSSID and registered with NORTH, the strongest AP (`Registered with node 1 as device 1` and `device 2`). `nodes` names the APs from the vendor IE.
- Not yet exercised: setting time on one AP and watching the others follow it, a split grid rejoining, phone roaming, and the link keepalive under loss.

### Added (where handheld RAM goes, measured)
- `hh_mem_mark()` logs `[MEM] <stage>: free, change since the last mark, lowest, largest block` at each handheld boot stage, and the `mem` console command prints it on demand. Flashed to both handhelds; builds with zero warnings, layers ok.
- Hosyond (classic ESP32, no PSRAM), heap in KB: 183 free at boot. Console −7, **Wi-Fi driver −49**, joining and the TCP session about −9, **display and LVGL −21**, launcher −4, Status screen −7, Settings screen −7, screen saver −8. That leaves **58 idle**; opening Settings takes another −10 and opening a chat −12, down to **35 free**.
- FNK0104B (ESP32-S3): 241 free at boot, 113 idle, 91 with Settings and a chat open. Its 8 MB PSRAM is not used for any of this.
- Before the heap is counted at all, the handheld app's own static variables take **81.5 KB** of the same internal RAM (`idf.py size-components`). About 26 KB of that is 17 separate static copies of the service status (1740 B each), 20 KB is three static copies of the message list (6720 B each), and 27.8 KB is the service state, including its message ring. LVGL's own static RAM is under 1 KB; its cost is heap for objects and buffers.

### Changed (D45: no master AP; every AP holds the settings and serves the page)
- Every AP serves the admin page at its own address, and every AP keeps a full copy of the admin settings: grid name, time zone, and the password's PBKDF2 hash and salt. Before, only MAIN had them, so when MAIN was down there was no page and no way to set time, and every handheld was held to urgent broadcasts (D6).
- New backbone message `LG_T_GRID_STATE` (0x51), AP to AP only. `lg_core` floods it and hands the body to `io.on_grid_state`, treating it as opaque (1 to `LG_GRID_STATE_MAX` = 256 bytes). The AP firmware defines the 147-byte layout in `firmware/node/main/grid_state.c` and accepts only that exact length.
- Each AP announces its grid state when a backbone link comes up and every 30 s. An AP that was away catches up on everything it missed as soon as it links again, and the same path will carry any setting added later. A copy replaces another only when its (version, AP) pair is higher, so the newest change wins, including after a split grid rejoins.
- Grid time can be set on any AP. Setting it starts a new time generation, which is announced before the time itself. Any AP still holding an older AUTHORITATIVE time steps down to CARRIED and follows the newer one instead of defending its own.
- First-time setup on an AP that has working links waits until it has heard another AP's grid state. Otherwise a fresh AP could accept a new password that would then override the grid's existing one.
- Settings records gain `seq` and `author` (layout version 2). MAIN's existing version 1 record is read once and carried forward as version 1 made on AP 0, so the admin password survives the upgrade.
- Console `config` and `status` show the settings version and time generation; human-readable text says AP (D43). Login sessions belong to the AP that issued them.
- Built with zero warnings (AP firmware and `tests/target`, with a new `test_grid_state`). Not yet run on a board: waiting for the AP capture to finish.

### Added (APs record why they restarted)
- The owner saw MAIN keep forgetting grid time, which lives only in RAM (D6), so every restart of an AP loses it unless a neighbour still holds it. MAIN had restarted twice and NORTH about six times with no tool attached, and MAIN's USB port never dropped, so power loss did not explain it.
- Every AP now counts its restarts by cause in NVS: power-on or reset, software restart, crash (panic), interrupt watchdog, task watchdog, other watchdog, brownout. Boot logs `[GRID] Last restart: <cause>`, and after a crash or software restart it adds how long the previous run lasted, kept in RTC memory that such a restart preserves. `status` prints the counts, so a crash is still on record after a serial tool has restarted the board to look.
- On the classic ESP32 a pulse on EN, which is what opening a serial port does, reports the same cause as a real power-on, so "power-on or reset" covers both.
- Flashed to all three APs. MAIN's first flash lost its serial link partway through the write (`No more data to read from the serial port`) and left the app half-written. A second flash with `--trust-port` restored it, with its admin setup intact.
- `tools/serial_capture.py` writes UTF-8. On Windows with output redirected to a file, a board line the cp1252 code page could not encode killed that port's reader thread partway through a capture.

### Fixed (touch is believed twice before it counts)
- A press now has to be seen on two consecutive polls before it is reported, and so does a release. `indev_read` was the least sceptical reader of the panel in the firmware: the calibration screen in the *same driver* asks for six agreeing samples out of ten before it believes a press and three consecutive misses before a lift, while the path driving every real tap asked for one. A single noisy conversion on the resistive panel therefore became a genuine LVGL click, and one dropped sample mid-press became a released key.
- Two polls is about 30 ms at LVGL's rate — below noticing, and no finger is on the glass for less. Coordinates still move only while a sample says down, so a spurious miss cannot drag the pointer elsewhere.
- This is the best remaining explanation for a notification banner being "tapped" with nobody in the room, which is what sent a false read receipt: the banner is clickable, its handler opens the conversation, and opening the conversation reported the message read. Stated as the likeliest mechanism rather than a diagnosis — the false read no longer reproduces, but it was never proven to be this. Three earlier explanations were checked and discarded: the toast's auto-hide timer (15 s, not the ~4 s observed), the screen saver (a minute of idle, and it had not appeared), and a `status` read that could not answer the question it was asked.

### Changed (D42: one icon per state, and read means seen)
- A message's marker is **one icon showing the furthest state it reached**, each state its own shape: a clock while this handheld still holds it, an up arrow once an AP has taken it, a down arrow once the recipient's handheld confirms, an open eye once that handheld has shown it to its reader.
- Ticks are gone, and the reason is measurable rather than aesthetic: `accent` and `success` are **the same value** in this theme (`0x5FD38D`), so "delivered grey, read green" and "delivered green, read green" were asking one colour to carry two meanings. The pair of ticks was not hard to read, it was impossible. The owner had also been reading the *appearance* of the second tick on delivery as "they have seen it", which is exactly what it did not mean.
- Marker colours come from four new theme roles — `mark_wait`, `mark_node`, `mark_delivered`, `mark_read` — rather than borrowed ones. Read is a violet, deliberately outside the theme's greens, so the two states cannot collapse into each other again at marker size (D10).
- The glyph is drawn at `font_icon` and the words beside it at `font_tiny`, because the emoji font has exactly one size: a clock asked for inside a 10 px line would not have drawn at all. The clock is the emoji subset's existing `U+1F553`, chosen over a true hourglass once the cost was clear — the generator emits one size, so an hourglass meant a second font target.
- Group messages count people instead of naming a state: `3/4` for delivery and an eye with a number for reads, collapsing to "all read" once everybody has, so a number never sits there implying a missing reader when there is none. The denominator comes from the roster's group bitmask and excludes the sender.
- Broadcasts stop at a delivered count. No AP ever reports having finished handing one out — there is no such message in the protocol — and the grid cannot know how many handhelds are switched on, so a count of confirmations is the honest thing to show rather than a completion.
- Group **read** reports now happen at all, which took removing a 1:1 gate in the service and widening the screen's trigger. Broadcasts stay out: a report per handheld on the grid for every announcement is traffic nobody asked for.
- Read means seen, as far as a device without a camera can tell, and two ways of being wrong about that are closed. A **covered panel reports nothing**: the screen saver draws on LVGL's top layer, so the chat screen underneath stayed the active screen and reads were being reported while the panel showed only rain — the sender saw a read marker for a message its reader could not possibly have seen. Nothing is lost by waiting, because the report goes out on the first refresh after somebody clears the saver. And a refusal clears its glyph, so a down arrow can never sit beside the words "that handheld is offline".
- Verified on the boards: a message delivered at 20.9 s and held at `delivered` through polls at 30 s, 44 s and 54 s with nobody touching the receiver. Separately, with the conversation genuinely opened, delivered and read landed 11.5 s apart. **Not verified**: the group counts and group read reports, and whether the clock glyph draws — serial output reports states and counts, never pixels.
- `msgs` prints the delivery and read counts for group and broadcast messages, so a marker that counts people can be checked over serial instead of only by eye (D28).

### Fixed (a keyboard that can be put away)
- Every keyboard page now has a key that dismisses it. Raising the keyboard hides the controls row — and the button that raised it lives in that row — so once the keys were up the keyboard itself was the only thing that could dismiss them. Letters and the keypad had a cross for this; **punctuation and both emoji pages had nothing**, so from those pages there was no way back at all.
- The key is a down chevron rather than a cross: it is already what the controls row's own button turns into while the keys are up, and a cross reads as cancel or delete beside a text field.

### Changed (D43: they are APs, not nodes)
- Everything a person reads now says **AP**: "Which AP", "Look for APs", "No APs heard yet", "APs in range", "auto AP", "Looking for an AP", "on your AP", the chat's "messages wait until this handheld joins an AP", the Status time hint, and the problem lines a handheld shows when it cannot find one ("No LocalGrid AP in range", "The chosen AP is not in range"). Nineteen strings across the four screens and the service.
- The word is apt rather than a relabelling: each one really is a Wi-Fi SoftAP — `LG-MAIN`, `LG-NORTH`, `LG-SOUTH` is what appears in a Wi-Fi list — as well as a routing element, so "AP" names the thing a person actually meets.
- Two categories are deliberately unchanged, because each is an interface rather than prose. The console commands are still `node` and `nodes`: `tools/console.py` and the bench skill invoke them by name, so renaming breaks them where an alias would not. The log lines still say node (`[GRID] Registered with node 1`, `[NET] Heard node 2`): `tools/flash.py` matches them with its `verify_ok` patterns, and renaming one without the other produces a passing flash reported as a failure — which happened once already this prototype.
- Code identifiers keep `node` too (`lg_node.c`, `LG_MAX_NODES`, `node_ssid`, `firmware/node/`), scheduled as its own commit. Landing a rename across the protocol core, both firmwares and the tests on top of uncommitted marker work would make all of it unreviewable. Until then the gap between what the code says and what a person sees is deliberate.
- `tools/wifi_scan.py` and a `wifi` skill check which APs are actually beaconing, from this laptop's radio rather than from the ESP32s' or somebody's phone. It reports UP or MISSING per expected SSID, exits non-zero when one is missing so it can gate a flash, and `--watch` logs appearances and disappearances — which is what separates an AP that is dead from one that comes and goes. It honours D21 (a BSSID is a hardware address, so addresses are substituted out before parsing and signals are aggregated per SSID) and D25 (it never associates, never carries a message, and never stands in for a handheld). One caveat is stated rather than glossed: Windows may answer with an *active* scan, sending probe requests, so it is an observer but not guaranteed to be passive. It needs an administrator terminal — `netsh` blames location permission, but the real error underneath is `WlanQueryInterface` error 5, elevation.

### Fixed (the drawing task's stack, and Settings crashing a handheld)
- Opening Settings crashed a handheld: `***ERROR*** A stack overflow in task lvgl has been detected`, with a corrupted backtrace. It was reproducible on both boards, not one, and it reboots the device, which empties the message list along the way.
- The cause is depth, not a fat frame, and a decoded backtrace names it: `lv_timer_handler` → `lv_display_refr_timer` → `refr_invalid_areas` → `refr_area` → `refr_configured_layer` → `refr_obj_and_children`, then `lv_obj_redraw` and `lv_obj_refr` recursing down the tree, and at the bottom LVGL dispatches the draw **inline on the same stack** — `lv_draw_finalize_task_creation` → `lv_draw_dispatch` → `lv_draw_dispatch_layer` → `lv_draw_sw_label` → `iterate_characters` → `draw_letter_cb` → `lv_draw_sw_blend_color_to_rgb565`. A glyph blend therefore runs about nine frames beneath a tree walk that is already several deep, and Settings is the deepest tree this firmware builds: screen, tabview, content, page, list, row, label.
- Measured rather than guessed. The drawing task's handle is kept now, `lg_display_stack_headroom()` reports its high-water mark, and `status` prints it, so the figure is readable over serial on any board (D28). Peaks: **launcher 4992 bytes** (FNK0104B) and 4768 (Hosyond), **Settings 6544 bytes** (Hosyond). Against the old 6144-byte stack the launcher fitted with 1.1 to 1.4 KB spare and Settings ran about 400 bytes past the end — which is the whole crash, in two numbers.
- `LVGL_TASK_STACK` is 10240 now. Not 8192, which would leave about 1.6 KB over the measured peak, the same thin margin that had just failed; and not the 12288 used while measuring, which the Hosyond pays for in heap. Chat with bubbles on screen is still unmeasured, and the delivery marker adds a nested row and a second label to every bubble, so the deepest screen here may not be the one that has been measured.
- A `screen <settings|status|home>` console command opens a screen from serial, which is what made the deepest tree measurable at all: before it, the one screen whose stack cost mattered most could only be reached by tapping the glass (D23, D25, D28).
- `msgs` prints the delivery and read counts for group and broadcast messages, so a marker that counts people can be checked over serial instead of only by eye.
- Two silent faults found on the way, both in the read-report path: `hh_service_mark_read()` was gated on 1:1, so widening the screens to report group reads would have done nothing while looking like a feature; and the same function walked `s.ring[i]` directly where every other reader goes through `s.ring_head`, which scans the wrong slots once the ring wraps and loses a report rather than failing loudly.
- Not yet verified on hardware: the markers themselves (D42). Group counts, group read reports and whether the clock glyph draws are all unproven, because grid time is unset on the APs and D6 restricts a handheld with no grid time to urgent broadcasts. An urgent broadcast does leave the handheld, but with no AP reachable it stayed at `sending, delivered 0, read 0`, so the counts need a delivery and a delivery needs an AP.

### Added (D24, D29, D30: launcher, settings, and drawing only what changed)
- A launcher is the first screen now, with four tiles: Messages, Status, Settings, Self test. Tiles are measured by dividing the panel that is actually there, header height taken off first, so each is about three times the 8 mm touch minimum.
- A Settings screen, in sections: GRID (which node, reconnect, look for nodes), SCREEN (calibrate touch), THIS HANDHELD (run self test, device number, name, board, identity, memory, restart). Choosing a node is its own sub-screen.
- Touch calibration and the self test are handed to the application task, because the drawing task cannot wait for presses or run for tens of milliseconds.
- Status is the former home screen, reached from its tile. A shared top bar (Home, title, link and clock) sits on every screen.
- `components/lg_selftest` runs the quick check at boot and on demand from the Self test tile: envelope, dedup, bodies, UTF-8, the RFC 8439, RFC 7748 and RFC 5869 vectors, and an end-to-end seal and open. Boot logs `[TEST] Self test: ...` and the tile shows the count.
- Screens update only the part that changed (D29). `lg_ui_set_text()` writes a label only when the text differs; the chat appends new bubbles and rewrites only a delivery note that moved; the launcher, settings and node screens refresh on a signature of the values they show, instead of the service's once-a-second snapshot counter.
- A magnified keycap appears above the finger while a keyboard key is held (D30): one and a half times the touch minimum, the glyph in the largest font, clamped to the screen edges, and suppressed for control keys, where a bubble would only cover the row.

### Added (D41: flashing announcements, emergency takeover, and a saver you can switch off)
- An ordinary broadcast now flashes the screen with the message on it and plays a rising three-note chime, then clears itself after a few seconds. It is meant to be caught out of the corner of an eye from the other side of a tent — "the food is ready" should not wait to be noticed.
- An urgent broadcast takes the whole screen until somebody taps Dismiss, repeats its alert every four seconds while it waits, and sounds at full volume even on a handheld that has been silenced (D40).
- The urgent cue is now two tones alternating, the pattern a phone alert uses, rather than three repeats of one note — a doorbell and a warning should not sound alike.
- 1:1 and group messages are unchanged: banner and bell. The takeover is deliberately reserved for urgent broadcasts, because if every broadcast seized the screen people would learn to dismiss them unread, and then the one that matters is the one nobody reads.
- The flash changes the screen's own colour, not the backlight: a backlight blinking reads as a board fault, a panel changing colour reads as something demanding attention.
- The screen saver can be switched off from Settings (Screen tab) or with `saver [on|off]` over the console, kept in NVS so it stays off across a reboot. It also steps aside for an alert, since the saver and the alert draw on the same top layer and a saver left running would cover the one screen that must not be covered.

### Changed (D40: volume in four steps, and an emergency that ignores it)
- The Sound tab has a Volume row that steps through off, low, medium and high, kept in NVS. An existing mute setting is carried over as off.
- Four steps, not a slider: the cosine generator attenuates in 0, −6, −12 and −18 dB and this DAC path has no gain register, so anything finer would be a pretence. High is full amplitude and there is nothing above it in software.
- Off silences ordinary cues and the console's `tone` alike, but an **urgent broadcast always sounds, at full amplitude**, whatever the volume says. D6 already lets urgent through when nothing else gets out, and a handheld that stays silent for one would be worse than useless.
- The arrival bell is louder: the strike and the note it settles on are both at full amplitude now, and the near-inaudible eighth-amplitude tail is gone. Loudness was bought by spending the decay, which is the only currency this part has.
- `mute [on|off]` is replaced by `volume [off|low|medium|high]` over the serial console (D28).

### Changed (D39: Settings in tabs, and lists that keep your place)
- Settings is four tabs across the top — Grid, Sound, Screen, Device — instead of one long page of sections, so a setting is two taps away rather than a scroll and a hunt.
- Scrolling Settings snapped back to the top, and the cause was a misapplication of D29 by me: the screen rebuilt whenever a value it displayed changed, and free heap was one of those values. Heap moves every second, so the list was destroyed and rebuilt on nearly every refresh tick, taking the scroll offset with it. The node chooser had the identical flaw through signal strength.
- Values that never stop moving are quantised before they can trigger a rebuild: heap in 16 KB steps, signal in about 6 dB steps. A rebuild now follows a real change rather than noise.
- `ui_list_clear()` keeps the reader's place through a rebuild that genuinely has to happen. It deletes the rows one by one instead of calling `lv_obj_clean`, which zeroes the scroll offset, so the offset is never lost. `ui_list_restore_scroll()` is gone: putting the offset back with `lv_obj_scroll_to_y` forced a whole-screen layout pass inside the refresh timer, which is stack depth the drawing task could not spare (it had about 1.2 KB left at its worst on both handhelds).
- The read marker was two ticks in the accent colour again rather than an eye, on the reasoning that the owner's model is the phone convention and that delivered and read had looked identical only because read reports were never arriving. That held until the markers were looked at properly: see D42 below, where ticks are dropped entirely, because `accent` and `success` are the same value in this theme and no pair of ticks differing only in colour could ever have been read.
- Verified on the boards once grid time was set and every device carried `LG_ACK_READ`. The FNK0104B sent to the Hosyond at 20.710 s and the marker reached **delivered** at 21.210 s, half a second later, while the Hosyond had shown only a banner. It then sat at delivered for eleven seconds. Opening that conversation on the Hosyond at 32.385 s produced `[MSG] Read report to device 1 (boot 120 seq 3): sent` twenty milliseconds later, and the sender turned **read** at 32.880 s. Delivered and read are eleven and a half seconds apart in one log, which is the distinction that was missing: the second tick on arrival, the accent pair only once somebody has the conversation open. Driven entirely from the serial console through the real screens (`send`, then `chat`, then `msgs`), because a reset would empty the message list a test needs.

### Changed (D38: muting, and markers that differ)
- Settings has a SOUND section: notification sounds can be muted, and the choice is kept in NVS beside the touch calibration. A board with no speaker says so instead of offering a switch, and `mute [on|off]` asks or sets the same thing over the serial console (D28), which is how it gets tested without tapping a screen.
- That did not persist at first, and the boards said so: `mute on` took effect, but after a reset the setting read back as on. Audio now starts before the console so an early cue is played, which also put it before `nvs_flash_init()`, so its read of the flag failed — silently. NVS is initialised first now, and an unreadable setting is logged rather than passed over. Verified on the board: muted, reset, still muted, then set back to on.
- Muting silences everything, including the console's `tone` and `cue` commands. A mute that still beeps is not a mute.
- Delivered and read no longer share a glyph. Both were two ticks differing only in colour, which reads as the same marker every time on a 240 px panel. Now: a turning arrow while sending, one tick when a node accepts it, two ticks when the recipient's handheld confirms delivery, and an **eye** once that handheld has shown it to its reader.
- Markers and their timestamps use a new `font_tiny` theme role (Montserrat 10, falling back to 12 where it is not compiled in), so a marker no longer crowds the bubble it belongs to. Montserrat 10 is enabled for both the handheld and `tests/target`, since both link `lg_ui`.

### Added (D37: sound on sending and on arrival)
- A message arriving rings a bell: a brief bright strike, the note it settles on, its fall, and a fainter tail. A message leaving plays a short rising pair; an urgent broadcast plays three insistent notes at full amplitude. Cues that mean opposite things differ in direction, not only in pitch.
- Loudness is per segment rather than per cue, which is what makes a bell possible: one level for a whole cue can only produce a beep, and a bell is a strike that fades. The generator offers four levels (full, −6, −12, −18 dB) — coarse, but enough for an attack and a decay. It is not a real bell's inharmonic spectrum, which one sine at a time cannot be; the shape is what makes it read as a bell.
- The bell rings on the notification path, so a message arriving in the conversation already open on screen stays silent.
- Every sound is synthesised at play time from `(frequency, milliseconds)` segments by the classic ESP32's cosine wave generator: no samples, no DMA, no timer of ours, and no audio files in the firmware. The vocabulary costs a few dozen bytes of const data where one second of 16-bit mono would cost 32 KB.
- Cues are queued to their own task at a priority below the drawing task, and a full queue drops the cue rather than making a screen or the network service wait on a speaker.
- That task and its queue are created at the first sound rather than at boot, which is the right shape regardless: nothing is claimed until something asks for a noise.
- Audio starts as soon as the board profile is known, before the serial console, so a cue asked for in the first seconds is played instead of refused. The self test and the display take about five seconds on the Hosyond, and a `cue` arriving in that window was answered "no sound on this board".
- A 20 KB heap regression was suspected of that early start and then ruled out by measurement: moving the call back after the display gave 96 KB against 94 KB, while the *other* board's same reading swung from 147 KB to 169 KB across those builds with no audio change affecting it. The reason became plain once `status` reported live figures: the Hosyond reads 136 KB while its link is still `STOPPED` and about 95 KB once `ONLINE`, so Wi-Fi, lwIP and the node session account for roughly 40 KB and every comparison had been sampling a different moment in the boot.
- `tools/console.py --settled` waits for the board to be online before asking, so two readings are comparable. It asks the board with `status` rather than watching for `[NET] Online` in the log: that line is printed once, so a board already up and quiet never repeats it and the first version of this wait could only time out.
- First figures taken that way, both boards online on the master with the launcher showing: Hosyond **71 KB free, 69 KB lowest**; FNK0104B **126 KB free, 123 KB lowest**. That is the baseline for future comparisons, and it replaces every earlier number in this file that was read from the `Online` log line. Not yet measured: the same reading with the chat screen and keyboard open, which is the real worst case — `console.py` resets the board between invocations, so a screen opened by one command is gone before the next can ask. The `[NET] Online` free-heap line is sampled at a moment that varies with Wi-Fi and peer state, so it cannot support a conclusion that size — the `status` command's settled figures can, and it now reports live values instead of the service snapshot, which read `0 KB` in the seconds before the first snapshot exists.
- Sound is a board capability (D9): `lg_board` now carries an audio profile, including the amplifier's enable line. The Hosyond drives its DAC on GPIO26 (DAC channel 1 on a classic ESP32) with the amplifier enabled by GPIO4 held LOW, both from the vendor's own E32R32P pin table.
- The amplifier is switched on before a cue and dropped only once nothing else is queued, after a 60 ms tail: dropping it as the last segment ends clips the end off every sound. Missing this line entirely is silent rather than noisy, which is the trap it looks like — on a sibling board the same pin was declared as an LED, so switching the LED off switched the speaker off too.
- The FNK0104B's ES8311 wiring is recorded: codec at 0x18 on the touch I2C bus, I2S MCLK 4, BCLK 5, WS 7, DOUT 8, DIN 6, amplifier enable GPIO1 active LOW, volume ceiling 85. `i2cscan` on that board answers `0x18 0x38`, so the codec's presence and address are confirmed on hardware rather than assumed.
- None of the FNK0104B's audio pins touch the octal PSRAM lines (GPIO33 to 37). That board still reports no sound, because its codec backend is not written yet — but its pins are facts now rather than guesses, and the console's refusal says which of the two reasons applies.
- New console commands (D28): `tone [hz] [ms]` plays one tone, `cue <sent|received|urgent>` plays a cue, and `i2cscan` lists the addresses answering on the board's I2C bus, which is how the ES8311 gets confirmed without a datasheet.
- The I2C bus handle the touch driver opens is now shared inside `lg_bsp` through a private header, so the audio driver can probe it without the public interface pulling an ESP-IDF driver across the layer boundary (D27).

### Changed (D36: one icon size everywhere)
- Icons were each inheriting their button's font, so they came out at three different sizes: 28 px on the launcher tiles, 14 to 16 px on Send, and 12 px in the bars and the chat controls.
- The theme now carries `font_icon`, one size for every bar and control icon, and `lg_ui_icon_button()` builds every icon-only control: the glyph centred on a square target no smaller than the 8 mm touch minimum.
- Used by the top bar's Home, the chat's Back, Send, emoji, keyboard, home and urgent controls, and the conversation list's Home.
- Launcher tiles keep the larger 28 px glyph: there the icon is the tile's subject, not one control among several in a row.
- The size is 20 px, which is also the only size the emoji font has, so the drawn face matches the drawn symbols beside it.

### Added (D35: screen saver)
- A handheld untouched for a minute fills its panel with green characters falling down it, and the next touch clears them. That first touch is swallowed, so it cannot also press whatever is underneath.
- It draws on LVGL's top layer: the screen beneath keeps its widgets, its text and its scroll position, and nothing is rebuilt when the saver goes away.
- Column count, character size and line height come from the panel at start (D9) and the colours from the theme (D10): the head of each column is the bright text colour, the trail is the success green. Nothing is allocated per frame; the columns and their text are fixed arrays.
- The backlight is left alone, so this saves the panel, not the battery.

### Added (D34: sent, delivered and read markers)
- Our own messages show a marker instead of a sentence: `↻` while it is going out, one tick when a node accepted it, two ticks when the recipient's handheld confirmed delivery, and two ticks in the accent colour once that handheld showed it to its reader. A refusal still says why in words.
- Read reports are a new `MSG_ACK` status, `LG_ACK_READ = 9`. The 13-byte body and its length are unchanged and the value is appended so no existing status moves (design review answer 24).
- `lg_msg_ack_dec()` validates the status against the highest one it knows, so adding a status also means raising that bound. The first attempt did not, and the on-board suite caught it: every read report was rejected as malformed at the node and at the recipient, which is five failing checks and no read markers. The bound now names `LG_ACK_READ`, and the comment says why a future status has to appear there too.
- Nodes forward a read report the same way they forward a delivery report; the gate that accepted only `DELIVERED` from a recipient now accepts `READ` as well.
- Received messages now carry the identity of what arrived (author, boot, sequence), because a read report names the message it is about. This costs the inbox 8 bytes per entry.
- `lg_client_mark_read()` sends the report and refuses our own messages or a closed session; `hh_service_mark_read()` carries the request to the service task through its queue, so screens still never touch the client (D27), and reports each message once.
- Group and broadcast messages get no read state: one report per member would multiply traffic and say little. Read reports cannot be turned off yet.
- Urgent is an icon now: a warning triangle, amber when on and grey when off.

### Added (D32: keyboard pages and the phone keypad)
- The letters page lost its digit row and its punctuation, so it is four rows instead of five and a key is about 36 px tall instead of 22 px.
- Punctuation and symbols have their own page, six columns wide: `. , ? ! : ;` `' " ( ) - _` `@ # / & + =`.
- A phone keypad page types by multi-tap, as SMS keypads did: three columns of about 78 px, `2 abc` through `9 wxyz`, tapping a key steps through its letters, and a pause of 900 ms ends the run. It is the only layout that makes a key wide on a 240 px panel.
- Shift, ABC, punctuation, keypad, Back, and Send are now icons or two characters wide, so the keys that carry letters get the room.
- The magnified keycap shows the letter a keypad key just typed, not its `2 abc` label.

### Changed (D33: icon tiles, and the self test inside Status)
- The launcher is three tiles instead of four: Messages across the top, Status and Settings beneath it. Each is an icon (envelope, signal, gear) over a small one-line label.
- Tile labels are set with `lg_ui_text`, which does not wrap, and the detail line clips with a dot. At title size "Self test" broke onto two lines and left its tile.
- The self test moved into Status: its last result, what it checks, and a Run button. It is no longer a launcher tile nor a Settings row, so there is one place for it.
- The chat controls, the shared top bar's Home, Back, and Send are icons now: a face for the emoji pages, a keyboard for the keys (a down arrow while they are up), a house for home, an arrow for back, a check for send. Urgent stays a word, because no icon says "off".

### Fixed
- The chat list kept showing "No messages here yet." after messages arrived: the placeholder was only removed on a full rebuild, and the partial-redraw path appends without one. It is tracked now and taken away as soon as a bubble is added.
- The name of the handheld you are talking to was set in the title font, which on a 240 px panel pushed the Back button and the messages aside. It is body size now, and Back is an arrow.
- `lg_selftest` compared a dedup result against `LG_OK`, a constant from another enum. It compares against `LG_DEDUP_NEW` now.
- The boot self test failed one check on both handhelds (`aead ciphertext body`): its expected 16-byte run was not from RFC 8439 and its offset into the ciphertext was arbitrary. The check is gone. The `aead tag` check that remains is Poly1305 over the AAD and all 114 ciphertext bytes, so it proves every byte the removed check claimed to, and the quick check is now 47 checks.

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
