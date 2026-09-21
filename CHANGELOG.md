# Changelog

All notable changes to LocalGrid are recorded here, newest first.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions stay below 1.0 while the project is a prototype.

## [Unreleased]

### Changed
- **D79: a self-running demo, and voice without a microphone.** `demo showcase` and
  `demo resilience` on a handheld work through a 1:1 message, a group message, a voice talk, an
  announcement, an urgent call and its all clear, and positions, showing each step on the screen.
  The script is built when the run starts from who is present and what their devices report they
  can do, so nothing is hard-coded to the boards on one bench. `hh_voice_tone_start` feeds a
  generated warble into the real push-to-talk path, so the four handhelds with a speaker but no
  microphone can send voice; its capture task is created on demand, so a handheld that never runs
  a demo pays nothing. Verified on the boards: Marshall (no microphone) sent 31 voice frames in
  3098 ms with none dropped, and Channi heard them a backbone hop away, plus the text, the
  announcement and the urgent call with its all clear.
  Both runs verified on the boards; the resilience run moved Marshall from Fuji to Everest and back
  on the grid in 1 s. Two bugs found and fixed while doing it: the roam step declared success in 0 s
  without the handheld ever leaving its AP (the reconnect is asynchronous, so it now waits for the
  link to drop before timing its return), and the demo task overflowed its stack and restarted the
  board, because `hh_status_t` carries the people, users and groups tables and two of them shared one
  call chain - those structures now live outside the task stack.
- **Devices say what they can do, and the grid passes it on.** Design review answer 19 reserved two
  bytes of capability bits in the presence table and nothing ever filled them. A handheld now reports
  its own `LG_CAP_*` bits (speaker, microphone, generated voice, screen, touch, GPS, LoRa, battery)
  in `REGISTER`, read from its board profile rather than any list of boards, and its AP shares them
  with the grid in `PRESENCE_UPDATE` (D48). Any device or observer can now ask what is on the network
  and what each one is able to do, instead of being told in advance - so a board added later takes
  part with no code change. `PRESENCE_UPDATE` grew by 2 bytes; the decoder accepts the older layout
  too and reports no capabilities for it, so a grid whose APs are flashed one at a time keeps its
  presence working. Verified on a board: 6125 checks, 0 failures.
- **D78: `tools/serial_log.py`, a console logger so reading a board costs no reset.** A daemon
  attaches once to every connected board and writes timestamped console lines to `logs/serial/`;
  `--status`, `--who` and `--find REGEX` answer from those files and open no port. Only flashing
  opens a port: `flash.py` and `console.py` take a hold, the daemon yields it and reattaches after.
  Logs rotate at 8 MB and rotated files are purged after 30 days. A port that will not open is
  recorded as `#### unreachable`, and every read prints how long ago the board last spoke, so a
  stale log cannot be mistaken for a healthy one. New skill: `.claude/skills/serial-log/`.
- **D77: ask the hardware, never recall it.** A rule in `AGENTS.md` and a decision in
  `docs/DECISIONS.md`: identity comes from `tools/flash.py --identify` and live state from
  `tools/grid_watch.py`, never from `tools/bench_devices.json` alone and never from recollection.
  Reports now say whether an answer came from a board or from the map, and say so when a board
  could not be asked. `flash.py` already asked before writing; this extends it to reports and
  decisions.
- `tools/flash.py --identify` no longer resets boards it was not asked about: its sweep for
  unmapped ports now skips every port in the device map, not only the selected boards', so
  identifying two handhelds cannot reset an AP in the middle of a measurement.
- **The APs are named Everest, Fuji and Denali** (AP 0, 1 and 2; owner, 2026-09-20), written into each board's identity partition so the name travels with the board. The bench map's own labels stay `node-main`, `node-north` and `node-south`, because the skills and scripts address boards by those. New handhelds on the bench: device 8 **Marshall** (the second E28, after its cloned identity was undone) and device 9 **Jolly** (a third Freenove).
- **Noted for the watchers** in `docs/grid-watch.md`: a name arrives one per beacon rotation, so with eight handhelds it can take minutes, and "Handheld 6" is shown both for a handheld that was never named and for one whose name has not been heard yet. On the bench this looked like handhelds losing their names, when the grid was right all along. A logged-in watcher could take every name from the AP's status reply at once instead. To settle when the observers are next worked on.

### Added (D75: a board proves its identity is its own)
- **A device checks its stored identity against its own MAC at boot.** The mint value is now kept beside the ID (a `nonce` key in the identity partition, `id_nonce` in the bench map), so `lg_identity` can recompute the ID from this board's factory MAC plus the stored role, board and creation time, and compare. `lg_crypto` gained `lg_sha256` so the recompute goes through the one crypto interface; the MAC is read in memory only and is never printed, logged or stored (D21).
- **A board running another board's identity refuses to join the grid.** An AP starts no SoftAP, backbone, BLE, sessions or admin page and offers only `id` on its console; a handheld does not register and says so on its screen in plain words. Both log it once with the fix. A board provisioned before this change cannot be checked and behaves exactly as before — every bench board is in that state until re-provisioned.
- **The roster runs to handheld 12.** Adding a handheld no longer needs a firmware change, and a device number with no roster entry cannot join at all.

### Fixed
- **Two boards shared one identity on the bench.** Both answered `LG-H-E28-XRSRKB0XR4` and registered as device 4 on different APs with the same address, because the same map entry had been flashed onto both. One has a fresh identity and its own device number. `tools/flash.py --identify` now scans every serial port on the machine, not only the ones in the map (where this hid), and says plainly when two ports answer the same ID; a board whose ID it cannot decode no longer crashes the listing.
- **A handheld crashed while reporting that it could not start.** Giving a board a device number the roster did not know was detected correctly and then panicked in a boot loop, because the message asks for a name from a roster that does not exist yet, so the explanation never reached anyone. The roster lookups now tolerate a table that was never filled in. The identity refusal above uses the same path and would have crashed the same way.
- **A log line was read as a device ID.** The identity check logged under the tag `LGID`, which is also the marker the bench tools parse, so every board appeared to answer with the same identity. The tag is now `IDENTITY`, and the tools match the ID's shape rather than whatever follows the marker.

### Changed (D74: one radio at a time, and a quieter grid)
- **LoRa no longer sends what Wi-Fi is already carrying.** ESP-NOW carries everything between APs while it works; LoRa carries a frame only when ESP-NOW is not reaching that peer. Urgent broadcasts, SOS and all clear still go on **both** radios: they are rare, and learning that a Wi-Fi link had died only after losing an alert is the failure this radio exists to prevent. This revises D71's "always in parallel", which the bench disproved.
- **Housekeeping gives way on LoRa.** The shared state and handheld names are re-announced only to heal a missed update, so on a link carrying about one message every three seconds they must not queue in front of what someone is waiting for. They now travel only while fewer than half the send slots are busy, and the console counts how often they stood aside. Messages, presence, positions, time and acknowledgements are never held back. Measured: Wi-Fi was moving about 35 messages a minute with nobody using the grid, so without this a Wi-Fi failure would have moved a flood onto a radio that drowns in it.
- **The LoRa keepalive is once a minute**, not twice. On a quiet grid it was most of what the radio carried (about 18 of MAIN's 35 frames in nine minutes). A link is still called down after three missed, about three minutes.
- **The grid re-announces its shared state every two minutes** rather than every 30 s. A change is still announced at once and on link up, so this only sets how quickly a missed one heals; three APs at 30 s were the largest single source of traffic and of MAIN's full send queue (owner agreed).

### Fixed
- **MAIN was 364 bytes from a stack overflow.** The BLE admin link task formats the admin page's whole JSON, and at 3584 bytes of stack it had 364 left. Raised to 5120, which leaves about 1.9 KB spare at the same workload. The watcher reports this headroom, so it can be watched rather than guessed at.

### Fixed (the watchers could never log in, and three reasons why)
- **The AP and the watchers used different keys.** The AP derived the admin link's key from the backbone key; both watchers derive it from K, the key the pairing code carries. Every request failed to unseal and the AP hung up after a second, which looked like "the AP did not answer in time". The AP now derives from K as `docs/ble-link.md` always said — and it has to: a paired phone is given K alone and never holds the backbone key, so no phone could ever have connected. Each side had passed its own tests because each mocked the other with its own assumption.
- **A second conversation on the same connection was refused.** Windows keeps a BLE connection alive after a watcher believes it has disconnected, so the next watcher subscribes again and counts its messages from zero while the AP is still counting from the last conversation. The AP called that "out of step", hung up, and then refused everyone: the dashboard worked at most once per AP reboot. Subscribing now starts a fresh session, fresh counters and a fresh login; failed logins are deliberately not forgiven, so resubscribing cannot wipe the rate limit.
- **The link was never offered at all.** Both `ble_link.c` and `ble_adv.c` held their own copy of a 24 KB free-heap floor, and an AP carrying a LoRa module runs at 17-25 KB: the AP either refused every watcher or sat on the line and tore its advert down and rebuilt it every second, so no connection could complete. One floor now (13 KB), shared, with hysteresis.
- **"Wrong admin password" said "too many failed attempts".** The AP answers every refusal with a wait, two seconds on the first, and the tool read any wait as a lockout. It also left the login box on "Checking the password with an AP..." for ever when the pull failed for a reason of its own; it now says what happened and keeps the password when the trouble was reaching the AP.

### Fixed (the map)
- **Devices at the same spot hid each other.** A handheld two metres from MAIN is two pixels away, and consumer GPS scatters them further than they really are, so one marker covered the other and the grid looked short of a device. A few together now fan out side by side with a hairline back to the spot; more than three become one marker with a count that opens when tapped.
- **A tile server that refuses by serving a picture.** OpenStreetMap blocks some networks with an "Access blocked" image under a success code, which filled the map with them. The tool notices the same picture arriving for every square and falls back to the drawn plan.

### Added
- **The project mark on both watchers.** The dashboard serves the same icon the AP's admin page does (`/favicon.svg`), and the Android launcher icon is now the LocalGrid mark rather than a placeholder.
- **LoRa on the Android watcher** (0.3.0): the Traffic tab shows each AP's module, its peers with signal strength and when they were last heard, and the counters; 100 unit tests pass.
- **D73: the radios come first for memory.** Wi-Fi, BLE and LoRa keep their buffers; the GPS becomes a periodic reader on an interval the admin sets, released in between. Measured: MAIN, carrying GPS, the master admin page, LoRa and BLE, ran at 14.7 KB free with a 3.6 KB low-water mark.
- **`tools/flash.py` tolerates a garbled device ID** when the port is named with `--trust-port`. MAIN's console mangles its own ID often enough that the flasher kept refusing to flash the right board.

### Added (D71: a LoRa second backbone between APs)
- **LoRa on all three APs** (Reyax RYLR998, SX1262, AT commands over UART1, 868.5 MHz, SF9/BW125). New `lora.c` (the module, its task, detection, configuration, wedge recovery, per-peer links, the send policy) and `lora_wire.c` (base64, the part header, reassembly, the bounded send queue, the AT parser, airtime), with a fourth `tests/target` suite: **2163 checks, 0 failures** on NORTH. Frames are sealed by the existing backbone code, so both radios share one frame counter and one replay window and a nonce is never reused across them; a frame arriving by LoRa takes exactly the path an ESP-NOW frame takes. Urgent broadcasts, SOS and all-clear go on both radios; everything else only when Wi-Fi is not carrying it; live voice never. One part is about 940 ms on the air, so a full frame takes roughly 3 s.
- **A module is optional everywhere.** An AP with none logs one line, takes no buffers, transmits nothing and boots in the same time as before; a module plugged in later is picked up without a restart. The pins differ by AP as built: MAIN on GPIO 32/33/25 beside its GPS, NORTH and SOUTH on GPIO 4/5/13.
- **Ready for what comes next without a breaking change:** payloads up to 6240 bytes (D72's voice notes), an alert taking the radio from a long transfer between parts so it waits at most one part, an oversized payload refused with a logged reason rather than truncated, and an address plan with room for handhelds. Cut-through relaying was considered and rejected: an AP cannot authenticate a payload before all of it has arrived, so relaying is store-and-forward and the ceiling is a memory question. MAIN, with about 24 KB free, skips the send buffer and says so; it can still receive.
- **Seeing it work:** the admin page has a LoRa card, `/api/status` a `lora` object, the D70 traffic record a LoRa section (appended, so existing watchers are unaffected), and the console `lora` and `bb` commands, including the self-restoring `lora off <s>` / `bb off <s>` hooks that let `tools/chaos.py` break one radio and prove the other carries the grid. The number that leads: frames that arrived over LoRa and that Wi-Fi had not already delivered.

### Fixed
- **LoRa's long-payload buffers nearly starved the APs.** Reserving the 6 KB receive and send slots for D72's voice notes took an AP from 22-28 KB free to 9-12 KB with a 1 KB low-water mark once handhelds joined, and MAIN reached 0. They are now off (`LORA_LONG_PAYLOAD 0`) until a board has the memory: a long payload is refused at its first part, logged and counted, and the wire format still carries 48 parts, so enabling them later changes nothing on the air. Measured afterwards with LoRa running on all three APs: NORTH and SOUTH 22 KB free and 13-15 KB low-water, MAIN about 17 KB. **Voice notes cannot ship on these APs as they stand** - that is a memory problem to solve before D72 is built.
- **An AP could not share the broadcast address.** MAIN was addressed as 0, which is also the address that reaches every module, and its LoRa link kept timing out while NORTH and SOUTH linked to each other happily. An AP is now 1 + its index and 0 stays free for broadcasts; all three then linked (MAIN at -27 dBm, NORTH and SOUTH at -15 dBm, SNR 8-10). Found on the bench, not in review.
- **The Android watcher blamed the APs for its own fault** (0.2.3). It kept scanning while connecting, so the phone settled on the 23-byte default packet size, and the AP closes a link it has no room to answer on; a dropped link then never woke the code waiting for a reply, which sat out its timeout and guessed "the AP may be busy with another watcher". The scan now stands aside for the connection, the packet size is checked against the 64 the AP needs and reported in numbers, a drop raises the real reason at once, and every failure says what happened with its status code. "Busy" now comes only from the AP saying so. The app also tries every AP it can hear, loudest first, and keeps a connection log you can read on the phone. 86 unit tests.
- **The laptop watcher dropped handhelds from its list too eagerly.** A handheld's presence reaches it only through an AP's rotating beacon, so the window is now 90 s rather than 15 s, matching the phone.

### Added (D70: a watcher sees everything the admin page shows, and can still only watch)
- **A read-only BLE admin link on every AP.** Each AP serves a sealed GATT service (`4c470001-6c67-4772-6964-42544c453031`) so a paired watcher can ask for everything the admin page shows, without joining the grid's Wi-Fi. The service UUID is not advertised and the D68 advert and scan response are unchanged. One watcher at a time, dropped after 60 s idle, five failed logins, or any sealing failure, and not offered at all below 24 KB of free heap. Messages are sealed chunk by chunk with ChaCha20-Poly1305 and a full tag under `K_link = HKDF(backbone key, "LG-BLE-LINK-1")`, the key the pairing code already carries. Logging in is an HMAC over a fresh challenge against the stored PBKDF2 hash, compared in constant time, so the password never crosses the link. `GET_STATUS` and `GET_HISTORY` return the admin page's own bytes: `web_admin` now has one emitter writing to a sink, so the page and the watcher cannot drift apart. Costs 2.2 KB of static RAM and about 4.4 KB of heap; the AP reports its own heap in the traffic reply, so the bench run measures the rest. Layout and UUIDs: `docs/ble-link.md`.
- **Traffic and performance** (`GET_TRAFFIC`, packed binary, D49): messages by class both ways and relayed, voice frames dropped, drops and faults, per-backbone-link frames and bytes, the handheld side, and each AP's heap, queues, stack headroom, loop timing and radio errors, with a ten-bucket ring for one- and five-minute rates. One add on the hot path, no allocation. New `lg_crypto` primitive `lg_hmac_sha256` with its RFC 4231 vectors; new tests for the login proof, one sealed chunk end to end, and the traffic counters.
- **The laptop dashboard gained the admin page's tabs** — Overview, Map, Network, Handhelds, Traffic — in the same theme and wording, each pulled section saying which AP answered, by name, and when. The Map draws OpenStreetMap tiles when the laptop has Internet (fetched by the tool with a proper User-Agent, only the squares shown, nothing kept on disk) and falls back to a drawn plan with distances and bearings from MAIN, coordinates as text and a copyable link when it does not; no coordinate appears in any URL, query string or referrer, and `--no-map-tiles` switches tiles off. The admin password is typed into the page, kept in memory only, and never written to disk, logged or sent. `--self-check` grew from 26 to 67 checks against a mock AP.
- **LocalGrid Watch 0.2.1 for Android** gained the same tabs over the same link, with the password asked for when a protected tab is opened and kept in memory unless "remember on this phone" is switched on (then sealed by the Keystore). The link opens only while the app is on screen or on Refresh; the background service still does beacon-only watching and SOS notifications. The app now asks for the INTERNET permission, used for map tiles only. JVM unit tests 21 to 73, including replaying a whole conversation recorded from the laptop tool byte for byte, and a guard that no coordinate can appear in any URL the app builds. The APK installs over 0.1.0 with the same signing key.
- **A watcher monitors and does nothing else**, by construction (owner: "should not be able to read conversation or listen to ptt or partake in communication in any way but should be able to do monitoriting very very well"). No request writes anything; no reply carries a 1:1 message, a group message, an announcement's text or any audio; the watcher has no device ID, never registers with an AP and never joins the grid's Wi-Fi. Positions stay out of the beacon and cross the link only after an admin login, so a lost paired phone shows status but no coordinates. BLE addresses are never shown or logged (D21).

### Added (D69: LocalGrid Watch for Android)
- **An Android app that watches the grid over BLE** (`android/grid-watch`, native Kotlin and Jetpack Compose, installed by sideloading the APK). It shows what `tools/grid_watch.py` shows, in the same dark theme: the alert banner, each AP's health, the handhelds heard with their AP and battery, and an event log. A foreground service keeps watching with the screen off: low-latency scanning while the app is open, balanced in the background, filtered to the grid's company ID. It keeps an ongoing "Watching LocalGrid: N APs heard" notification, raises a loud "SOS alerts" notification (alarm sound and vibration, heard even with the ringer silent) with who and near which AP, and "X is safe" on the all clear. It only scans (D25), never shows BLE addresses (D21), and asks for no location, no Internet and no `BLUETOOTH_CONNECT` (minSdk 31, "never for location" scanning). ChaCha20-Poly1305 with the 4-byte tag is plain Kotlin, checked against the RFC 8439 vectors and against frames sealed by `grid_watch.py`; 21 JVM unit tests pass, and a smoke test on the emulator passed. Release APK 0.1.0, 2.1 MB, signed with a local keystore that is not in git.
- **Pairing by QR code.** `python tools/grid_watch.py --pair` shows the `LGW1:` code (docs/ble-status.md) as a QR code on a page served to this laptop only, and prints it as text for pasting. It holds the grid ID and the derived status key only, never the backbone secret, so a phone can read status but cannot join, send, or forge anything. The app keeps it encrypted with an Android Keystore key, excluded from backups; "Forget this grid" deletes it. The QR code needs `pip install segno`.

### Added (D68: watch the grid from a laptop over BLE, without joining its Wi-Fi)
- **Sealed BLE status beacon (AP side).** Every AP now puts a status frame in its BLE scan response, as manufacturer data under the LocalGrid company ID, and replaces it every 500 ms. AP health goes every other frame; handhelds online and where, the newest urgent alert, handheld batteries and chosen names take turns in between (`docs/ble-status.md`). Frames are sealed with ChaCha20-Poly1305 under a key derived by HKDF from the backbone secret, with the tag cut to 4 bytes. The nonce is AP index, boot counter and a per-boot 24-bit frame counter; the beacon stops at the counter's wrap rather than reuse a nonce. The core task builds and seals each frame (`firmware/node/main/ble_status.c`) and the NimBLE host task only applies it; the discovery advert is unchanged. Positions are never in the beacon.
- **lg_core:** a handheld's keepalive PING now carries its battery percent (one optional byte; older APs ignore it, and new APs take an empty PING as unknown), kept per handheld on its AP in RAM. Each AP also keeps the newest urgent broadcast it carried, whether it is still active (15 minutes, not stood down by the same author's all clear), and the distinct read reports for it that passed through. New tests `test_ping_battery` and `test_urgent_record`; `tests/target` passes 1332 checks with 0 failures on Pepa Pig (ESP32-S3).
- **`tools/grid_watch.py` (laptop side).** Listens passively for each AP's discovery advert and sealed status, checks the 4-byte tag, drops replays and older boots, ignores other grids, and serves a local dashboard on 127.0.0.1 only (one self-contained page, no CDN). The dashboard shows an SOS / all-clear banner, AP cards (heard and RSSI, uptime, links, handhelds, time source and GPS, lowest heap, restarts, brownouts, reset reason), handhelds with batteries, and an event log, with an optional beep on SOS. Options: `--port`, `--no-browser`, `--secrets`, `--log`, `--seconds`, and `--self-check` (26 checks, no hardware). On Windows, bleak merges the advert and scan response under the same company ID, so the tool reads both raw events itself. BLE addresses are never printed, logged or served (D21); the laptop never takes part in the grid (D25). New skill `.claude/skills/grid-watch`, and a user guide in `docs/grid-watch.md` (what you need, how to run it, security, and what the page is telling you).
- **Verified live:** after flashing the three APs, the laptop verified every status frame (0 failed the check, 0 replays) and showed all three APs with their links and handhelds. MAIN showed authoritative time from its GPS (11 satellites); NORTH and SOUTH showed time carried from GPS at stratum 1. Pepa Pig's battery (97 %) arrived through SOUTH. The E40 would not take a flash (serial noise at 460800 and 115200 baud) and still runs the previous handheld build, so its battery shows as unknown.

### Changed (the network is "LocalGrid Access Point"; the admin page names handhelds)
- Every AP now broadcasts **"LocalGrid Access Point"** instead of "LocalMesh Access Point" (owner: "It should be LocalGrid Access point"; D46 updated). Handhelds join by that name, so every AP and handheld must run the same build; phones and laptops join the new name once. The admin page, console help, `tools/wifi_scan.py` and the wifi skill say the new name.
- The admin page listed handhelds by their roster placeholders ("Handheld 1", ...): the Handhelds card, group members and announcers now show the name each handheld chose (D50), up to 23 bytes, and device names are JSON-escaped like group names (a chosen name is typed by a person).
- Checked while looking into the owner's "only SOUTH": all three APs had both backbone links up and grid time from MAIN's GPS; the page had been loaded before the other APs' first status after the flash.

### Added (D67: the alert unit is a watch; local time and where it came from on every handheld)
- The alert unit's idle screen is a watch face: the LocalGrid mark with the unit's name and battery, local time in large drawn digits with a blinking colon, the date, and a small GPS mark beside the time (green when a GPS keeps grid time, a faint outline when it was set by hand). The AP line, notices and SOS button stay; only the digits, colon and mark that change are repainted.
- Handhelds show local time. The admin page's browser turns its time zone into a POSIX TZ string (from its own zone data, rules checked eight years ahead, a fixed offset for zones without DST) and saves it with the time; APs keep it in the replicated settings (grid state layout 5, settings version 3, migrated) and send it to handhelds as the new `TIME_ZONE` (0x54) at registration and on change. Handhelds keep it in flash, so a restart without an AP still shows local time. The launcher, chat times, Status and the console use it; the admin page says which zone handhelds use.
- `TIME_SYNC` to handhelds gains a flags byte (9 bytes; 8 and 5 still decode) with `LG_TIME_FROM_GPS`, set by every AP from the replicated time generation; APs keep flooding the 8-byte form to each other. A registering handheld now gets `TIME_SYNC` right after `REGISTER_ACK`. Tests: `test_bodies` (9-byte form, zone strings), `test_time_source_and_zone`.
- All APs and handhelds must be reflashed. Re-save the time zone once on the admin page.

### Fixed (D66 on the bench: "I'm safe" and the unit's wording)
- "I'm safe" did not take down the SOS (owner): handhelds show one alert at a time, and it arrived as another urgent alert, flashing red with the alarm. It now carries a new envelope flag, `LG_FLAG_ALL_CLEAR` (0x0020), on an urgent broadcast (so it still goes out without grid time); APs pass it through and the client keeps it (`hh_message_t.all_clear`, `hh_service_send_all_clear`). Receivers replace the author's SOS alert with a calm **ALL CLEAR** alert (the accent colour, no flashing, the ordinary chime) without reporting the SOS read; an emergency from anyone else stays up.
- After "I'm safe" the unit said "I'm safe: seen by 2", which read as two people having seen the person (owner). It now says "You're marked safe. Everyone was told." ("Telling everyone you're safe..." while sending, "Could not tell everyone you're safe" on failure), with no reader count.
- The unit's AP status and notice lines wrap onto two lines at 172 px instead of running off the edge, and connection problems say "AP", never "node" (D43), on every handheld.
- The alert unit is named Recty on the bench (device 7, C6L: the plain ESP32-C6-LCD-1.47, confirmed by its boot probe). Protocol tests: 1063 checks, 0 failures on the ESP32-C6 and on the FNK0104B.

### Added (D66: the alert and distress unit on Waveshare's 1.47in ESP32-C6)
- The handheld firmware and the on-board tests now build for esp32c6. Two board profiles, C6L (ESP32-C6-LCD-1.47: ST7789, no touch) and C6T (ESP32-C6-Touch-LCD-1.47: JD9853 with AXS5106L touch, battery on GPIO0), both in the new alert-unit role. The two boards are wired differently under one name, so a boot-time I2C probe (`lg_bsp_board_resolve`, logged `[BSP] Board probe`) picks the right profile. Pins from Waveshare's documentation and factory example (THIRD_PARTY.md).
- `lg_bsp` gained panel column and row offsets, a JD9853 init sequence, an AXS5106L touch driver, and debounced buttons from a table in the board profile (pin, level, pull-up, and the action a press and a hold drive), so a builder adds a button without code (owner: "add touch but also map to physical buttons"). Console `ui button <n> press|hold`.
- An alert unit shows only its name, AP, battery and grid clock, incoming alerts (Read by button or touch), and the SOS: hold 3 s, a 5 s countdown with Cancel, then an urgent broadcast "SOS from <name> near <AP>" with the AP's coordinates when known. It repeats as a new "SOS (repeat n)" every 60 s until a handheld reads it, shows "Seen by N" with names, keeps to the APs' 2 s urgent rate limit, and ends with an urgent "<name> is safe". Urgent needs no grid time, so SOS works with the clock unset (D6).
- Device 7 joins the prototype roster (version 4), so APs must be reflashed to accept it. Push-to-talk's buffers are allocated only on boards with audio (about 8 KB of RAM freed on the others). `flash.py` accepts the ESP32-C6.

### Changed (the admin page has tabs)
- The admin page was one long column of eleven cards. It now has a tab bar under the header: **Overview** (grid time, this AP), **Map** (the map, or a note that no positions are known yet), **Network** (APs, availability, incidents), **Handhelds** (handhelds, groups, announcements) and **Settings** (admin password). The open tab is kept in the address (`#map`, `#network`, ...), so a reload or a bookmark lands on it; on a phone the tab bar scrolls sideways. The map is drawn again when its tab opens, since a hidden map has no size.
- The setup page's name placeholder was "Smith Family Camping", which breaks D19 (never a camp network) and the multi-family examples rule; it is now "Lakeside Trip".
- Checked in the browser against the preview mock (`web/preview/mock-master.js`): setup, every tab, and a phone-width viewport.

### Added (change the admin password)
- The admin page has an **Admin password** card: the current password (a wrong one counts towards the login lockout), then the new one twice under the setup rules (12 to 64 characters, not the same as the old). The new hash replaces the old in the grid's settings, so every AP takes it with the next settings version (D45); every other session on this AP is logged out.
- The password was already stored only as a salted PBKDF2-HMAC-SHA256 hash, never as text. Its work factor is raised from 4,000 to 8,000 iterations, so each guess against a copied hash costs twice as much (a login takes about 1.8 s on an AP instead of 0.9 s); a hash made with fewer iterations is upgraded at the next successful login. The login button says "Checking…" meanwhile.

### Added (three more handheld boards: E28, CYD, E40)
- LocalGrid now runs on the LCDWIKI E32R28T-1 (E28, 2.8in ILI9341), the dual-USB ESP32-2432S028 "CYD" with the inverting panel, and the LCDWIKI E32R40T (E40, 4.0in ST7796, 320x480). All facts are restated from Braino's measured profiles, and anything a board inherited from a sibling is marked as a guess in `lg_board.c` (THIRD_PARTY.md).
- `lg_bsp` gains an ST7796 driver (TFT_eSPI's power and gamma sequence on esp_lcd) and XPT2046 touch on a bus of its own (SPI3) for boards that don't share the display bus. The screens needed no change at 320x480: everything is sized at run time (D9, D10).
- The prototype roster gains Handheld 5 and 6 (roster version 3); the three boards are devices 4 to 6 in `tools/bench_devices.json`, and `flash.py`, `power.py` and `console.py` skip devices with no port yet. Every AP and handheld must be flashed: old firmware turns devices 5 and 6 away.

### Added (contributor kit: prepared, not open)
- `docs/BOARDS.md`, the hardware guide: supported boards, every `lg_board_t` field and where its value comes from, how board codes in device IDs select a profile, the six steps to add a board, and a bring-up checklist with the command and evidence for each item. A draft `CONTRIBUTING.md`, a new-board issue template, a pull request template, the `new-board` skill, and a CI workflow (`.github/workflows/build.yml`) that builds every firmware type and target with ESP-IDF v6.1 and throwaway secrets and checks layering (D27).
- LocalGrid has no licence yet (owner: "prepare, but keep it closed for now"), so every one of these says outside contributions cannot be accepted, with TODOs for the licence and a contributor sign-off. The workflow has not been run.

### Fixed (1:1 messages refused after an AP restart: the night chaos run)
- The 10-hour chaos run of 2026-09-18 recorded 20 "lost" 1:1 messages between registered handhelds on different APs, in clusters after an AP restart. None was lost in transit: the sender's AP had never heard of the receiver and refused it as offline. Presence was flooded only when a handheld registered and when a link came up, and a restarted AP dropped that one flood because the peer confirmed the link first and data was accepted only over links confirmed on this side. Found from the logs alone by a parallel agent; every cluster matched.
- `backbone.c`: an authenticated data frame from a peer's known boot now confirms the link on this side too (a peer sends data only over links it has confirmed), instead of being dropped.
- `lg_node_announce_presence()` re-floods every handheld registered on an AP every 60 s (D48: announced on link up and periodically), and receiving APs pass a presence update on to their handhelds only when it changed. New test `test_presence_announced_again`.
- A handheld now logs an AP's refusal (`[MSG] Refused by the AP (offline): 1:1 to D boot B seq S`), and `tools/chaos.py` reports it as "refused by the AP" rather than lost; still a finding while both ends are registered.
- The same run found no unplanned restarts: all 62 boots were injected (no brownouts, panics or watchdogs), 45 faults and 2 full blackouts all recovered, and grid time came back from MAIN's GPS after each blackout.

### Changed (chaos runs with GPS on the grid)
- `tools/chaos.py` no longer sets grid time from the PC when the grid already has it: at the start of a run, and after a blackout, it waits for the grid's own sources first. MAIN refuses `time set` while its GPS has a fix (D63), and another AP would have accepted it and hand-set time until MAIN's next fix took it back.
- After a blackout the report says where time came back from, read from the APs' own `[TIME]` lines: the GPS on an AP (D63), a handheld's clock or live fix (D53, D60, D65), or the PC; a blackout with none of them is a finding.
- Checked on the bench before the night: with MAIN's GPS unplugged and NORTH and SOUTH holding time, a restarted MAIN took grid time back within a second of its links coming up ("AP 0 has no grid time; sent ours"), so the 11 s gap seen earlier came from every AP having been reset at once, not from a fault.

### Fixed (MAIN's free heap back from 11 KB to 29 KB)
- The admin page kept five copies of its status snapshot, three of the positions list and a 6 KB reply buffer (24.6 KB of static RAM). It now keeps one published copy and one for the web task, and sends `/api/status` in chunks from a 1 KB buffer (15.5 KB freed; httpd stack 8 to 6 KB).
- ESP-IDF's lwIP holds a full 1.5 KB buffer for every `send()` until it is acknowledged, so a handheld registering (7 to 11 small frames) took 11 to 17 KB, and handhelds reconnecting together stacked it. An AP now sends the frames going back to a handheld in one write when it finishes handling that handheld's frame; the bytes are unchanged and talk frames are not held. Measured on MAIN with three handhelds re-registering at once: minimum free heap 29 KB (17 KB after the first fix, 11 KB before).

### Added (D65: a GPS on a handheld, and everyone's position)
- **Protocol and APs.** New `POSITION` message (0x24, 18 bytes: subject, lat/lon in microdegrees, fix time, satellites, flags). A handheld sends its own GPS fix with `lg_client_send_position`; APs keep the newest fix for every handheld and AP in RAM, flood newer ones on the backbone, push them to their handhelds, and hand every known position to a handheld when it registers. MAIN shares its own GPS position every 30 s or after moving 20 m. A handheld fix marked LIVE gives an AP with no clock grid time, as registration already does (D53). Positions are never written to flash, a deliberate exception to D48. MAIN's admin page card is now a Map: it pins MAIN and every handheld, fits them all in view, and lists each one's coordinates, distance and direction from MAIN, and fix age. Tests: `test_position_body`, `test_positions`, `test_position_time`; 1042 checks, 0 failures on the FNK0104B.
- **Handheld GPS.** The FNK0104B reads a GT-U7 on its 4-pin UART connector (GPIO44 RX, 43 TX, from new `gps_rx`/`gps_tx` board-profile fields; other boards have none and behave as before). NMEA parsing moved into a portable `firmware/common/lg_nmea.c`. While the GPS has a fix it is the handheld's clock, which REGISTER carries back to an AP (D53); D6's grid-time rule is unchanged. The handheld sends its position at registration, every 30 s, after moving 25 m, and just before an urgent broadcast. Positions are never logged. Console `gps` and `gps raw`, as on MAIN. On the bench Pinky's GT-U7 was heard at once (346 sentences, 0 bad) and was still looking for satellites indoors.
- **Status shows everything a fitted GPS says** (owner: "display all GPS data if one is connected ... show if a lock is achieved"). A GPS section appears only on a handheld with one: Lock ("yes, 3D + WAAS" or "no, looking for satellites"), Satellites (used, in view, best signal), Accuracy (HDOP in words), Position, Altitude and movement, Last fix (UTC and age), and Data (sentences, bad checksums); "no data from the GPS: check its wiring" when a fitted module goes quiet. `lg_nmea` now also reads GSA (2D/3D, PDOP) and GSV (satellites in view and signal), plus GGA's quality, HDOP and altitude and RMC's speed and course; `hh_service_gps_info()` carries it to the screen. The list holds 32 rows.
- **Screens.** Urgent and announcement alerts give the sender's distance and direction from this handheld ("240 m NE of you"), or from MAIN when this handheld has no fix, or the sender's coordinates, and how old the position is past two minutes. The alert layout is worked out in one place for painting and touch, which also fixes a long message running under the Read button. Status is now a scrolling list with GPS, Position and MAIN rows and a Nearby section (other handhelds by distance and direction, freshest first), repainting only rows whose text changed. New portable `ui/ui_geo.c` does distance, bearing, compass and text.

### Fixed (D63: pages said an admin set a GPS time)
- With MAIN's GPS keeping grid time, NORTH's page still said the time was "set by the admin on MAIN": the replicated record said where and when time was set, not how. The top bit of its author field now marks a GPS setting (no layout change; an AP on older firmware follows it as before), the history bytes carry it to the page, and the page says "set by the GPS on MAIN" or "Set by this AP's GPS".

### Added (D64: where MAIN is, on its admin page)
- With a GPS fix, MAIN's admin page has a **Where this AP is** card: coordinates (decimal, and degrees-minutes-seconds) with Copy, links to the phone's map app, Google Maps, Apple Maps and OpenStreetMap, and an OpenStreetMap map with a pin. `/api/status` `gps` gains `pos`, `lat_u`, `lon_u` (microdegrees); the browser does the formatting (D49).
- The map tiles are loaded by the viewing browser, not the AP; without Internet on that device the map area says so and the rest still works. The position lives only in MAIN's RAM: not in flash, not on the backbone, not on handhelds.

### Added (D63: grid time from a GPS on MAIN)
- `firmware/node/main/gps.c`: MAIN reads NMEA from a GPS module (GT-U7, u-blox 7 class) on GPIO16 at 9600 baud, checks checksums, and takes UTC from fixed RMC sentences and the satellite count from GGA. Wiring: GPS VCC to 3V3, GND to GND, TXD to GPIO16; RXD and PPS unconnected.
- Once it has a fix the GPS wins: MAIN becomes the time source (AUTHORITATIVE, stratum 0, a new time generation) and the other APs follow; later fixes slew or step MAIN's clock. While the fix is fresh the admin page (409) and `time set` refuse hand-set time; a time set on another AP is taken back at the next fix. Losing the fix leaves the grid on MAIN's clock and hand-setting works again.
- The GPS is optional: with none fitted the reader idles on a pulled-up pin and nothing else changes. The admin page shows FROM GPS (n satellites) or "looking for satellites" only when a GPS is heard; `/api/status` gains `gps {started, heard, fix, sats}`; console `gps` prints the reader's state. `gps raw` prints the next 600 bytes as they arrive, anything unprintable as hex, to tell a wiring fault from a wrong baud rate. On the bench a loose TXD jumper looked exactly like no GPS (1 byte in 10 s); after reseating, MAIN had a fix with 7 satellites within seconds of boot and the grid took its time from it. The position is used only for MAIN's own admin page (D64).

### Changed (D61 and D62 on the bench)
- **Talk without opening a chat** (owner): in Messages, holding a person or group row for 0.4 s talks to them (the row turns red, letting go ends it); in Groups, holding a group you belong to does the same. A quick tap still opens the chat or the editor. `slist` gains talk rows and `SLIST_HOLD` / `SLIST_HOLD_END`.
- **Talk was too faint and cut short** (owner, and the first bench logs: a 5.3 s talk played 3 s). The talker now levels its voice (automatic gain toward -14 dBFS, at most +18 dB, never clipping, with a gate so pauses are not pumped into hiss), and talk plays one 6 dB step louder than cues (LOW is -6 dB, MEDIUM and HIGH full). Wi-Fi power save held frames at the AP until the next beacon, so talk arrived in bursts: the radio now stays awake while talk flows and 10 s after, the player waits 400 ms before starting and 1.5 s before giving up, and holds 1.2 s of frames. A talk frame that meets a full socket is dropped instead of ending the session, and a handheld no longer scans or moves to another AP (D54, time moves) while talk flows -- one talk was cut when the handheld changed AP mid-sentence. After the fixes a 16.4 s talk played for 16.4 s; group talk reached both members across two APs, the Hosyond through its DAC.
- **Talk on the Hosyond was faint, buzzing and chopped; on the Freenoves clear but faint** (owner, and a phone recording: a 1.4-1.5 kHz buzz in 60 ms bursts between words, only 14 dB under the speech, starting and stopping with the talk). An empty DMA ring replays its old buffers, so every late frame became a burst of stale audio: playback now writes silence through a gap, and 50 ms of silence before the first frame. The Hosyond's DAC now runs at 24 kHz with each 8 kHz sample interpolated to three instead of an 8 kHz staircase that sounded like a radio. The talker gates its microphone between words (-18 dB after 200 ms below about -40 dBFS, faded across a frame) and its automatic gain tops out at +12 dB, so its own noise is not lifted into the pauses. A new **Talk loudness** setting (Settings, Sound: Normal, Loud +6 dB, Louder +12 dB, peaks rounded off above about -4 dBFS) boosts talk playback only; it starts at Loud on the Freenove and Louder on the Hosyond.
- **"Not connected to an AP" about 3 s into a talk** (owner). With all three handhelds on MAIN, the talker's session failed, a listener's was closed by the AP, and the third saw the AP stop answering. The AP's core task, which serves every session and the backbone, waited up to 30 s on any listener whose socket was full, so one listener falling behind during talk froze the AP and the talker's socket filled. A talk frame a listener cannot take at once is now dropped for that listener (counted in `sess_voice_dropped()`), and anything else waits at most 2 s before that one session is closed.
- **The Hosyond still had a disturbance under talk** (owner: "much better but there is disturbance"). Not the link: frames are authenticated, so a damaged one is dropped, never played. It is the 8-bit DAC and its amplifier beside the radio, and the listener's talk boost lifting the talker's gated noise back up between words. The listener now gates too (below about -36 dBFS for more than two frames it fades to true silence, and fades back in), and the DAC path low-passes at about 3.5 kHz, where speech ends and the interpolation's fizz begins.
- **Home from every screen** (owner): every screen but the launcher has a house at the top-left that goes home in one tap.
- **The lock screen and the screen saver took turns** (owner: "they fight each other"). The lock stamped its 12 s idle timer with a clock read after the loop's own, so the unsigned difference looked like a timeout of weeks and the saver came straight back over the lock on every wake. The same wrap made the talk radio flip between awake and power save; both now use one clock.

### Added (D61: push-to-talk)
- **Protocol core.** New `LG_T_VOICE` (0x40) frames carry 100 ms of IMA ADPCM (8 kHz mono, 800 samples in 400 bytes) behind a 10-byte header holding the decoder state, so a lost frame costs only 100 ms. Voice is 1:1 (sealed end to end like text) or group, never broadcast, and is sent once: no outbox, no retry, and no ack when an AP takes it. It follows the time rule for non-urgent text (D6). Voice frames use their own per-boot sequence with `LG_VOICE_SEQ_BIT` set and a newest-only table per author on APs and handhelds, so they never enter the text dedup windows or make text retransmissions look stale. APs refuse bad voice at most once a second per author (`LG_CEV_VOICE_REFUSED`). New `lg_client_send_voice`, optional `io.on_voice`, node stat `voice`. Tests: `test_voice_body`, `_group`, `_direct`, `_duplicates_and_order`, `_refusals`, `_keeps_text_fresh`.
- **Audio.** `lg_bsp_audio` gains 8 kHz mono voice. The FNK0104B records through the ES8311's microphone (+30 dB analogue, +18 dB digital, +4.5 dB ADC volume; `mic gain` adjusts it) and plays over the codec, the I2S port sending and receiving at once at 16 kHz, filtered down or up to 8 kHz. The Hosyond plays voice through its DAC at 8 kHz and is listen-only. Ordinary cues wait while voice or the mic is open; an urgent cue cuts voice off. `service/hh_adpcm.c` is standard IMA ADPCM. Console `mic level` / `mic loop` / `mic gain` test the whole path on one board.
- **Handheld.** `main/voice/hh_voice.c` owns the microphone and speaker (capture and playback tasks) and reaches the network only through `hh_service.h` (`hh_service_voice_send`, `hh_service_set_voice_io`), which sends each frame once. Playback holds the first frames 250 ms against jitter, plays one talker at a time, and ends a talk on its END frame or after 600 ms of silence. A talk stops by itself after a minute, and the reason any talk stops (refused, offline, no time) is shown.
- **Screens.** In a 1:1 or group chat a full-width "Hold to talk" bar sits under the field while the keyboard is down, on boards with a microphone; it turns red while talking and shows who is talking or why a talk stopped. While someone talks in a conversation its chat title names them, and on every screen a mark left of the battery says someone is on air.

### Added (D62: battery badge and screen lock)
- Every screen's top bar, and the lock screen, show the charge on both handhelds: the supply is sampled by the service every 2 s (`service/hh_battery.c`), smoothed over about 40 s, mapped through a LiPo curve, and held in a 2-point deadband (`hh_service_battery_percent()`, -1 = no badge). Without a cell it honestly reads the charger's 4.1 to 4.2 V.
- A padlock in the top bar locks the handheld (`ui/ui_lock.c`): a lock screen with the wordmark and battery, released by holding "Hold to unlock" for 0.9 s (150 ms contact grace), handing back to the screen saver after 12 s untouched; waking the saver returns to the lock. Announcements and urgent alerts still break through and can be Read; other messages chime without a banner. Behaviour and numbers follow Braino (THIRD_PARTY.md).

### Added (bench: a third handheld)
- A second Freenove FNK0104B joined the bench as device 3, ID `LG-H-F4B-4VV3QPWGE8`, bench name `birthdaysuite`, named BirthdaySuite on the grid (D50). Provisioned and flashed with `flash.py`; it registered with MAIN and took grid time at once.
- The Hosyond re-enumerated as COM10 (it has been dropping off USB); the device map follows it.

### Fixed (D60: grid time survives a restart)
- The grid lost its time whenever every AP restarted together, and the handhelds could not put it back, so the owner had to set it by hand. Root cause: the clock was RAM-only on both APs and handhelds, so a restarted handheld had nothing to carry (D53 was working as written -- `lg_node.c:584` needs `reg.client_time != 0`, and a rebooted handheld sends 0).
- `firmware/common/lg_timekeep.c` keeps the clock on the chip's RTC timer, which counts through any reset that does not cut power, with a marker in RTC memory so a restored value is known to be ours and not whatever was in the counter.
- An AP restores it as CARRIED at unknown stratum, so an AP closer to where the admin set it corrects it immediately; a handheld restores it as its own clock, which its next REGISTER carries to an AP that came back without one.
- A power-off still loses it (the RTC stops), the restored value fails a 2023-11-14 floor, and D6 stands.
- An AP that refuses a handheld's offered clock for being too old now logs it. It was silently dropped, which looks exactly like the carry-back never happening.
- `tools/chaos.py` gains `--scenario blackout` (every experiment takes all APs out together) and, after any blackout, waits `--time-recovery` seconds to see whether grid time comes back from a handheld before the PC sets it -- recording which one saved the grid. The script used to set the time itself straight away, which would have hidden this failure indefinitely.

### Added (D59: the phone keypad, and keys that fire where the finger landed)
- The keyboard opens as a phone keypad: four columns, about 60 px a key, letters by tapping a key until the one you want shows (900 ms ends a run, 0 is space, shift as on qwerty). The full qwerty keyboard is one key away (`abc`), the keypad one key back (`123`), and whichever was last used is remembered in NVS.
- A key now fires where the finger landed rather than where it lifted. A fingertip rolls a few pixels leaving the glass, and on a 24 px qwerty key that typed the neighbour -- the owner's "ankush" came out "abjudh", with misses in both directions, which is what ruled out a mapping offset.
- The numbers and emoji pages return to whichever letters page is in use instead of always to qwerty.
- Fixed: the strip between the message field and the keyboard was painted by nothing, so an alert's colour stayed there after the alert was dismissed. `draw_all()` now fills from the field to the keyboard (or to the bottom edge when the keyboard is down).
- `touch [seconds]` on the handheld console prints what the panel reports against the pixel it maps to, so a tap landing wrong can be read rather than guessed at (D23, D28).
- Verified on the boards: both handhelds built and flashed; on the Hosyond `ui type ankush` through the keypad sent exactly `ankush`, and taps at the 2 and 6 keys' coordinates sent `am`. **Not verified by eye**: the keypad's layout on the panels, and whether the Freenove now types what your finger means.

### Added (provenance)
- `THIRD_PARTY.md` records where material in the tree came from, after the owner flagged that Braino is GPLv3: what LocalGrid restates from Braino (the owner's own project, so the owner's to license here), the TFT_eSPI panel init sequences and their FreeBSD attribution, the OFL fonts, and ESP-IDF.
- LocalGrid itself stays unlicensed for now (owner: decide before anything is published).

### Added (D58: Read dismisses an alert, and a broadcast says who read it)
- Alerts are closed with a Read button across the bottom; the corner X is gone (owner). Tapping Read also reports the message read, so dismissing an announcement and acknowledging it are one act.
- Broadcasts now send read receipts, as 1:1 and group messages already did. They had been left out to save one ack per handheld per announcement; knowing who has seen an emergency is worth it.
- Under our own group or broadcast message the chat shows who has read it: "Read by Pinky, Bluey", or "Read by N handhelds" when the names do not fit the bubble. 1:1 is unchanged -- the eye marker already says it.
- `hh_message_t` carries the core's read mask, and `hh_service_reader_names()` turns it into current roster names in one place.
- Verified on the boards: an urgent broadcast from the Hosyond raised the alert on the Freenove; tapping Read closed it, and the Hosyond's `msgs` then read `to everyone URGENT [sent, delivered 1, read 1]`. **Not verified by eye**: the Read button and the readers line on the panels.

### Added (D57: sending an emergency, and a cheaper alert flash)
- The Everyone conversation has an urgent toggle beside the send key: outlined when off, filled red when on, and the send tick turns red with it. Sending asks first ("Urgent broadcast -- this takes over every screen until it is read", Cancel or Send), and urgent never carries over to the next message.
- When urgent is all this handheld may send -- not an announcer (D56), or grid time unset (D6) -- the toggle is locked on and the field reads "Urgent message only". Before this the field was simply dead, which would have blocked an emergency.
- The no-LVGL UI had no way at all to send an urgent message from the screen; only the console's `send urgent <text>` did. That gap is closed.
- Alerts paint once and flash only a 12 px border instead of repainting the panel (owner: "a lot of screen redraw is happening"). An emergency pulses every 500 ms, an announcement keeps 260 ms for its eight flashes. Measured on the Freenove with an emergency up: 5.9 screens of pixels per 10 s, where full repaints cost about 38.
- The emergency tone is 2.3 s instead of 1.1 s (five pairs and a held last note), since it repeats every 4 s and a second of sound in four was easy to miss across a camp.
- Fixed while testing: the confirm panel's buttons sat on top of its second line, because the painter and the layout each guessed the panel's height. One `place_confirm()` now owns the box, and it centres on the whole screen when the keyboard leaves the list too little room.
- Verified on the boards: both handhelds built and flashed warning-free; on the Hosyond the toggle logs `[UI] Urgent on`, Cancel logs `Urgent broadcast cancelled`, and Send logs `[UI] Chat send (urgent): queued` with `[MSG] Sent URGENT to everyone`; the Freenove received it as `URGENT`, raised the alert (`cover 1`) and closed it on the X. **Not verified by eye**: the corrected confirm panel and the border flash (the owner's photo showed the overlapping version).

### Added (D56: the admin page says who may announce)
- Owner: the APs' web page defines who may make announcements. Urgent broadcasts stay open to everyone, and a new grid starts with everyone allowed.
- Protocol: the GROUPS body carries `announcers`, a bit per roster user, in a 13-byte head (was 9). One version (seq, author) covers groups and announcers together, so the list is sticky the way groups are: saved in NVS, announced on link up and periodically, and sent to a handheld when it registers (D48). `LG_GROUP_ANNOUNCERS` is a new GROUP_EDIT op, refused with the new `LG_ACK_REJ_NOT_ALLOWED` when a handheld asks for it.
- Enforced at both ends: `lg_client_send_text` returns the new `LG_ERR_DENIED` before the message leaves, and `lg_node` refuses a broadcast that arrives anyway with `LG_ACK_REJ_NOT_ALLOWED`. Urgent broadcasts skip both checks.
- Admin page: a new Announcements card with "Everyone, including handhelds not seen yet" and a handheld list; `/api/status` reports `announce_all` and `announcers`, and `/api/groups` takes `{"op":"announcers","devices":"all"|"1,2"}`.
- Handheld: the Everyone conversation shows "Urgent only: ask the admin" in the field with the send tick greyed out when this handheld may not announce, and a refused message reads "not sent: the admin page does not let you announce". The AP console's `groups` prints who may announce.
- Fixed while testing: `lg_msg_ack_dec` rejected any status above the highest it knew, so the new refusal never reached the handheld and the message sat pending. The bound now includes it; the comment there already warned about this.
- Verified on the boards: `tests/target` on the Hosyond, 710 checks, 0 failures, including a new scenario where only Ranger may announce (Dad's announcement refused by his own handheld, his urgent broadcast delivered, Ranger's announcement delivered, and a handheld with a forged older table refused by its AP). All five boards flashed; MAIN reports "Announcements (D56): everyone" and kept its grid time through the flash (`time CARRIED`); a broadcast from the Hosyond arrived on the Freenove. **Not verified on hardware**: setting a narrower list from the admin page, which needs a browser on the grid network.

### Changed (D55: LVGL retired; the no-LVGL UI is the handheld UI)
- Owner: make the no-LVGL UI the default and retire LVGL. A normal build and `flash.py` now put it on every handheld; there is no option to go back.
- Deleted: `components/lg_ui` (LVGL glue, theme, widgets, calibration, screen saver), the LVGL screens in `firmware/handheld/main/ui`, `CONFIG_LG_HH_UI_SPIKE`, `sdkconfig.spike`, and the LVGL settings in both `sdkconfig.defaults`. No project downloads LVGL any more.
- The spike code moved to `firmware/handheld/main/ui/` with `ui_` names (`ui_main.c`, `ui_chat.c`, `ui_kb.c`, `ui_list.c`, `ui_screens.c`, `ui_overlay.c`, `ui_theme.h`, `ui_nav.h`).
- Fonts: `components/lg_draw/fonts` holds Montserrat 10 to 28 px (with the Font Awesome icons) and the emoji fonts as `lg_font_t` tables (`lg_font.h`, `LG_SYMBOL_*`). `tools/convert_font.py` rewrites lv_font_conv output into them; `tools/build_emoji_font.py` uses it and now writes into `lg_draw`.
- Touch calibration moved into `lg_draw` (`lg_draw_calibrate`), shared by the handheld and the test app. The handheld calibrates at boot when the panel needs it, before the launcher.
- Status screen: the "Boxes drawn" debug row is gone (owner); a "Self test" row shows the boot self test (D24), in the error colour if anything failed.
- Console: `spike` is now `ui` (`ui tap|scroll|kb|type|page|log`); `screen` takes `home|status|messages|groups|settings`; `chat` opens a conversation on the new UI; `status` reports the UI task's stack headroom; `saver` reads and writes the NVS setting directly.
- Test app: the results screen and touch check are drawn with `lg_draw` instead of LVGL.
- `tools/check_layers.py`: `lg_draw` takes `lg_ui`'s place in the layer rules, and a new rule fails any `#include <lvgl...>` or a downloaded `lvgl` component.
- Verified: handheld and tests build for esp32 and esp32s3 with zero warnings (handheld 937 KB and 926 KB). Both handhelds flashed with `flash.py --role H` and registered on different APs; every screen opened over serial on both with no fault (Hosyond 154 KB internal RAM free, UI task 4.2 KB stack unused; Freenove 187 KB, PSRAM 8122 KB free). `tests/target` on the Hosyond: 666 checks, 0 failures. **Not verified by eye**: the test app's redrawn results screen and the boot calibration path (both handhelds are already calibrated).

### Changed (D54: handhelds spread across the APs)
- Owner: if all APs are in range, not every handheld should join the same one. Before, the choice was signal plus a 3 dB bonus for the last AP, and load only counted above 8 handhelds, so every handheld joined the strongest AP.
- Joining (`pick_node`): APs heard at -70 dBm or better are all good enough; among them the one with the fewest other registered handhelds wins, and signal breaks ties. Backbone first and grid time second still outrank load. A handheld's own registration is not counted against the AP it is on.
- Rebalancing while online (`balance_move_due`): move only to a good AP with fewer handhelds than this one would have without us; wait 60 s online plus 20 s times the device number, rescan first, then move; at most once every 3 minutes; never with a fixed AP chosen, and a move for grid time comes first. Logged as `[ROAM] Balancing: MAIN has 1 other handheld(s), NORTH has 0; moving`.
- A planned move (for balance or for grid time) now closes the TCP session while still associated and waits 150 ms, so the AP drops the handheld from its count at once. Before, the AP counted a departed handheld until its 30 s idle timeout, and the first test showed the second handheld moving away from an AP the first had already left.
- Verified on both handhelds (no-LVGL builds): both pinned to MAIN with `node 0`, then `node auto`. After the graceful-close fix, exactly one move: the Freenove (device 1) balanced to NORTH at 122 s, the Hosyond stayed on MAIN, and nothing moved in the following 150 s. The LVGL handheld firmware also builds for both chips with zero warnings.
- Phones are not balanced: they choose their own AP (D46).


### Added (no-LVGL UI: screen saver, notifications and alerts, and the logo)
- `spike_overlay.c`, matching the LVGL firmware's behaviour:
  - Banner: a 1:1 or group message for a conversation not on screen rings the bell and shows a banner at the top for 15 s; tapping it opens the conversation.
  - Unread counts: kept per conversation and cleared when it is opened. The Messages tile shows "N new", and the conversation list shows "N new" per row in the warning colour.
  - Alerts: a broadcast flashes a full-screen announcement (warning colour, 4 cycles, then still) with its chime; an urgent broadcast flashes the error colour and repeats the siren every 4 s. Both stay until the X is tapped (D41).
  - Screen saver: after a minute untouched, if its setting is on, the green rain falls, drawn one character cell at a time. The first touch only dismisses it.
  - While an alert or the saver covers the panel, screens do not paint; afterwards `spike_redraw_current()` redraws the screen where it was (chat and lists keep their scroll). A screen that repaints under the banner puts the banner back.
- The home screen shows the LocalGrid mark beside the name, drawn from the SVG's shapes with smoothed edges at 26 px (`lg_paint_pixel()` added to `lg_draw`).
- Console: `spike log` also prints the overlay's state (cover, banner, idle time, saver setting, unread total); `spike tap` goes through the overlay first and counts as activity, as a finger does.
- Verified over serial on the Hosyond: the Freenove's 1:1 message gave `Notified: Pinky in Pinky`; its broadcast gave `Announcement alert: dinner is ready`, and a tap on the X gave `Alert closed`; with the saver switched on from Settings > Screen, `Screen saver on` after 60 s idle and `Screen saver off` on a tap. The saver setting had been off on the Hosyond. **Not verified by eye**: the logo, the banner, the alert, and the rain.


### Added (no-LVGL UI: Messages, Groups, and Settings on both handhelds)
- Owner, after the scrolling test: add the Groups and Settings screens. The spike is now a whole handheld UI apart from the screen saver and the self test.
- `spike_kb.c`: the keyboard, shared by chat, group names, and renaming. `spike_list.c`: a list screen (header with home or back and an optional +, optional tabs, and section, fact, action, choice, button, note, checkbox, and text-field rows) that scrolls with the panel's hardware scroll, repaints one row for a press or a typed character, and only rebuilds when what it shows changed. `spike_nav.h`: screens ask to move with `spike_go()`, and the spike task switches once the touch or refresh that asked has returned.
- Messages opens a conversation list: Everyone, the groups this handheld is in, and every handheld heard from. The chat works for all three. Group messages show delivered and read counts ("2/3", then an eye count, then "all read"), and received group or everyone messages name their author. Opening a conversation reports its messages read (D34, D42), as the LVGL chat did.
- Groups: every group with its members ("you" for this handheld; groups this handheld is not in are muted); + makes one. The editor has the name field with the keyboard, a checkbox per known handheld (the maker cannot untick themselves), Save, and Remove with a second tap; it waits for the AP's answer, then returns to Groups or shows the refusal.
- Settings: Grid (Which AP, Reconnect, Look for APs, connected AP, grid time), Sound (volume Off/Low/Med/High, Test sound, Test urgent), Screen (screen saver setting, Calibrate touch), Device (Name, device number, memory and PSRAM, Restart with a second tap). Which AP and Name are their own screens; calibration draws three targets with `lg_bsp_touch_wait_press()`.
- Not carried over yet: the screen saver (its setting is kept, the rain is not drawn), the self test, notifications and alerts outside the chat.
- Console for driving it without a finger: `spike go <home|status|messages|groups|settings>`, `spike tap <x> <y>`.
- Verified over serial on both handhelds with taps: conversations, the Everyone chat and back, Groups, a new group's editor with the keyboard shown and hidden, all four Settings tabs, Which AP and back, home; no crash. Internal RAM free on a list screen: Hosyond 133 KB, Freenove 166 KB. **Not verified by touch.**


### Changed (spike chat: hardware scroll, a full keyboard, delivery icons, a new-message arrow, and the Freenove)
- Owner, after trying the spike: scrolling should be smooth with hardware scroll, the new keyboard is much better and should be on the Freenove too, and it needs capitals, numbers and emoji; delivery icons were missing; a message arriving while scrolled up needs a slowly flashing down arrow that goes when the reader reaches the end.
- `lg_bsp_display_scroll_area()` and `lg_bsp_display_scroll_to()` send VSCRDEF (0x33) and VSCRSADD (0x37), the same on the ST7789 and the ILI9341; neither board mirrors rows. `lg_draw_scroll_area()` and `lg_draw_scroll()` make the chat list the scrolling window, and every send maps screen rows onto the panel's circular memory rows, split where they wrap, so painters stay in screen coordinates.
- Scrolling moves the list with one command and paints only the rows that came into view. The flashing arrow is taken off before a scroll and put back after, so it never scrolls with the content. Measured on the Hosyond: 31 scroll steps of 10 px averaged 12 ms each, including one 250 px jump that costs a full 105 ms list repaint; a full repaint of an 18-message list is 107 ms.
- Keyboard: letters with shift (once for one capital, again for caps lock, again for off), a numbers and symbols page (`123`), and two emoji pages of the 48 emoji (the smiley key; previous and next arrows). A press repaints one key, about 1 ms.
- Bubbles show D42's markers for our own messages in the theme's marker colours: clock (waiting), up arrow (an AP took it), down arrow (delivered), eye (read).
- A message from the other handheld that arrives while the list is scrolled up leaves the list where it is and flashes a down arrow (on and off every 700 ms) in the middle of the list; reaching the end, or tapping the arrow, removes it. Verified over serial: `new message below; arrow flashing`, then after scrolling down, `reached the newest message; arrow gone`.
- The spike now builds for the ESP32-S3 (`build-spike-s3`, defaults `sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.spike`) and runs on the Freenove, with PSRAM: internal 173 KB free with the chat open (the LVGL build idled at 161 KB), PSRAM 8,122 KB free. Hosyond: 140 KB with the chat open.
- Both handhelds run the spike, which has no Groups or Settings screens. **Not verified by eye**: that the hardware scroll draws correctly on both panels.


### Added (spike, part two: a 1:1 chat without LVGL on the Hosyond)
- `lg_draw` gains painted regions: `lg_draw_region()` sends a rectangle in bands and calls a painter per band; `lg_paint_panel()` and `lg_paint_text()` draw clipped to the band and a clip rectangle; `lg_text_wrap()` word-wraps UTF-8 with emoji. `lg_draw_box()` is now a painter too.
- `spike/spike_chat.c`: the Messages tile opens a chat with the first known handheld. Bubbles with wrapped text, time and state; a list that scrolls under a dragged finger; an input field that shows the tail of what is typed; a lowercase keyboard (letters, space, comma, full stop, question mark, backspace, hide) where a press repaints only its key. RAM holds a layout per message (id, position, height, side), never its text: a bubble fetches its message when painted. Console: `spike chat | scroll <px> | kb <on|off> | type <word> | log`.
- Measured on the Hosyond after the Freenove sent eight messages (seven shown): chat open with 7 bubbles, keyboard shown and hidden, typing through the keys: free 141 KB, lowest 137 KB, largest block 108 KB. The LVGL chat with its keyboard costs about 12 KB more than its launcher (128 KB idle after the RAM cuts), so about 116 KB.
- Paint cost: a key press 1 ms; a full repaint of the message list 83 ms on average (6 repaints: open, three scroll steps, keyboard shown and hidden). That is about 12 frames a second while dragging: usable, not smooth. It fetches and wraps each visible message once per 24-line band; caching the wrap and fetching once per paint is the obvious next step, and the ST7789's hardware vertical scroll could move the list without repainting it.
- **Not verified by touch**: drag scrolling and typing were driven from the console; the owner has to try them.


### Changed (handheld RAM: 24 KB back, and PSRAM on the FNK0104B)
- From the static RAM audit, the low-risk cuts plus the inbox:
  - Removing a group compacts the message ring in place instead of through a 6.7 KB scratch copy.
  - The console reads messages one at a time (`hh_service_message()`) instead of keeping a 6.7 KB copy of the list.
  - Handhelds build `lg_client` with a one-slot inbox (`LG_INBOX_SIZE=1u`, set for every component in `firmware/handheld/CMakeLists.txt`; `lg_client.h` keeps 32 by default for the simulator). The service copies each message out as it arrives, so this frees 8.3 KB.
  - One names buffer for saving and loading instead of two (1 KB). The self test's two E2E keys live on the heap only while it runs (1.3 KB).
- Measured on the Hosyond with the LVGL launcher idle: 128 KB free, lowest 125 KB. It was 104 KB, lowest 101 KB, before.
- FNK0104B: 8 MB octal PSRAM enabled (`sdkconfig.defaults.esp32s3`: octal, 80 MHz, malloc above 4 KB may use it, Wi-Fi and lwIP buffers prefer it). Boot logs `Adding pool of 8192K of PSRAM memory to heap allocator`. With the launcher idle: internal 161 KB free, lowest 128 KB, largest block 88 KB; PSRAM 8,122 KB free of 8,192 KB. Internal RAM is about 15 KB lower than without PSRAM (the driver reserves 32 KB of internal RAM for DMA and the mapping takes some), so internal RAM is still the figure that runs out; tuning is open.
- Memory is reported as internal 8-bit RAM everywhere (`[MEM]` marks, console `status`, the Status screen), since with PSRAM on, the old free-heap call counted the 8 MB and hid what runs out. PSRAM is reported beside it: `[MEM] ...; PSRAM free N of M KB, lowest L KB`, `status` prints a `PSRAM:` line (`none on this board` on the Hosyond), and the Status screen adds it when present. The console's figure had also included the classic ESP32's 32-bit-only RAM, which made it 35 KB higher than the `[MEM]` line; both now agree.
- Built for esp32 and esp32s3 with zero warnings and flashed to both handhelds. `tests/target` was not rerun: the `lg_core` change is an `#ifndef` around the existing default, so the simulator build is unchanged.


### Added (spike: handheld UI without LVGL, measured on the Hosyond)
- `components/lg_draw`: a retained-box renderer. A box (panel, border, rounded corners, one line of text) is redrawn only when asked, and only its own rectangle goes to the panel, in bands of lg_bsp's 24-line DMA buffer. Glyphs come straight from LVGL's generated font tables (uncompressed 4 bpp) with the 14 px emoji face as fallback; LVGL is never started. Touch uses the same two-sample confirmation as the LVGL input path.
- `firmware/handheld/main/spike/spike_ui.c`, behind `CONFIG_LG_HH_UI_SPIKE` (`sdkconfig.spike`): the 2x2 launcher and a live Status screen. Console `screen status|home` drives it. Not a product build: no messaging screens.
- Build: `idf.py -B build-spike -D SDKCONFIG=sdkconfig.spike-esp32 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.spike" build`, zero warnings; image 999 KB against 1,192 KB.
- Measured on the Hosyond, same boot and same console steps as the LVGL build (free heap / lowest since boot):
  - Display start: LVGL, display and touch cost 31 KB; lg_draw, display and touch cost 14 KB (11.5 KB of it the draw buffer both need).
  - Launcher idle: LVGL 104 KB / 101 KB; spike 120 KB / 116 KB.
  - Status screen shown: LVGL 93 KB / 90 KB; spike 120 KB / 116 KB.
  - Largest free block stayed 108 KB with the spike; LVGL left 92 to 104 KB.
  - The spike's boxes are static (about 7 KB of .bss) where LVGL's objects are heap, and the numbers above already include that.
  - Redraw: the Status screen, with its clock, heap figures, and a box counter changing, sent 1.8 full screens of pixels per 10 s in about 50 boxes, 115 to 126 ms of drawing per 10 s (about 1.2% of a core). The idle launcher sent nothing. Switching screens costs one clear plus its boxes, 2.2 to 2.7 screens.
- **Not verified**: how the text looks on the panel, and touch; the owner has to look. The Hosyond is running the spike, without messaging, until it is flashed back.


### Changed (Groups is its own tile on the handheld)
- Owner: groups should be their own thing, not made from inside Messages. The launcher is two by two: Messages and Groups, then Status and Settings. The Groups tile says how many groups there are and how many this handheld is in.
- The Groups screen (`ui_group_open_list()` in `ui/ui_group.c`) lists every group with its members, named from the handhelds this one knows ("you" for itself). Groups this handheld is not in are muted and not tappable, since only members may change them. + makes a group; tapping a group opens the editor, whose Save, Remove, and back return to Groups. It redraws only when the group table or the known handhelds change, so a tap is never lost to a redraw.
- Messages no longer has the + and group chats no longer have the pencil. Console: `screen groups`.
- Flashed to both handhelds; six rounds of `screen groups` and `screen home` on each with no crash, Hosyond 104 KB free. **Not verified by touch.**


### Changed (D53: an AP takes grid time back from a handheld)
- Flashing and the brownout-prone hub restart APs, and grid time lived only in their RAM, so the owner kept having to set it again although both handhelds still had it. An AP with no time now takes it from a registering handheld whose clock the grid set: new optional node io `on_client_time`, called before `REGISTER_ACK` so the ack carries the time back. The AP logs `[TIME] Took grid time ... from handheld N`, marks its stratum unknown so any AP closer to the source corrects it, and floods it at once. No wire change: `REGISTER.client_time` was already sent, and is 0 on a handheld the grid never set.
- Verified on the bench: `tests/target` on SOUTH, `LG_TESTS: 666 checks, 0 failures`. APs flashed one at a time, then all three restarted together through their serial ports (twice). All reported `Grid time 0 (UNSET)`; about 5 s after boot MAIN logged `[TIME] Took grid time 1789655713 from handheld 2 (this AP had none)`, and NORTH and SOUTH took it from MAIN within 30 ms (`from AP 0 (stratum 255)`). Nobody touched the admin page.
- Tests: `test_time_from_handheld` (all APs lose time, a handheld restores one AP and is unrestricted at once; an AP with time is never offered one; a handheld with no grid-set clock offers nothing).


### Fixed (admin page stuck on "Connecting to this AP")
- The merge left two `const esc` declarations in `admin.html` (main's strips characters, the Groups card's encodes them). A redeclared `const` is a syntax error, so no script ran and the page never left its loading card. The Groups helper is now `escHtml`. Checked with `node --check` on the page script and in the preview: setup, dashboard, availability, outages, and adding a group all render with no console errors. Flashed to MAIN, NORTH, and SOUTH.
- The owner confirmed the page loads on a phone and set grid time. A group message then went end to end: the Freenove's `group new Cooks 1,2` reached both handhelds as groups version 4, and `send Cooks dinner at seven` arrived on the Hosyond with a notification, marked `delivered 1` on the sender.


### Fixed (both handhelds hung on the conversation list; merged onto main's monitoring work)
- Both handhelds were found with the task watchdog firing every 5 s, the drawing task spinning in `lv_event_mark_deleted` under `rebuild_list`. Cause: the groups branch still built the alert layer and notification watcher after `ui_launcher_start()` released the display lock, the race main had already fixed. The merge keeps one lock held across the whole start-up; the two versions together would have taken the lock twice and released it once.
- All five boards had been flashed from the groups branch, replacing main's uncommitted work (AP monitoring, D48 to D51). That work was committed on main and merged here. Conflicts were additive (handheld names beside groups); `.chips` on the admin page kept main's style, and the group member picker uses `.members`.
- `tests/target`: the simulator no longer fit the classic ESP32's largest free block (151 KB asked, 130 KB available) once names and per-device rosters were both in it. Its event queue and capture buffer are static now; `LG_TESTS: 651 checks, 0 failures` on SOUTH.
- Group member pickers (handheld and admin page) list only handhelds the grid has seen, never the roster's unclaimed places (owner: "Do not Mock Handhelds 3 and 4"). The preview mock has two handhelds.
- Flashed NORTH, SOUTH, and both handhelds. Ten rounds of `chat` and `screen home` on each handheld: no watchdog, panic or restart; Hosyond 105 KB free. **MAIN is off USB** and still runs the earlier groups build without monitoring or names, so it disagrees with the other APs until it is reflashed. Ports now: NORTH COM16, SOUTH COM18.


### Added (D52: groups made and removed at run time)
- FAMILY, KIDS and LEADERS are no longer built in. A grid starts with no groups; the admin page (new Groups card, `POST /api/groups`) and handhelds (a + on the Messages screen, a pencil in a group chat, `ui/ui_group.c`) make, rename, change members of, and remove groups. A handheld may change only a group it is in, and is always in a group it makes. Up to 8 groups; names 1 to 15 bytes.
- Protocol: `LG_T_GROUPS` (0x52) carries the whole table, `LG_T_GROUP_EDIT` (0x53) carries one edit from a handheld. Both have exact-length decoders. The table is versioned (seq, author AP), newest wins, and ids only grow. `lg_roster_t` now holds the group table by value, with membership as bits per group; `lg_node_t` and `lg_client_t` take a mutable roster owned by the firmware. New core calls: `lg_groups_apply_edit`, `lg_groups_newer`, `lg_groups_removed`, `lg_node_edit_groups`, `lg_node_announce_groups`, `lg_client_edit_group`; events `LG_CEV_GROUPS`, `LG_CEV_GROUP_REFUSED`; io `on_groups_changed` (node) and `on_groups_removed` (client). `lg_roster_prototype()` is replaced by `lg_roster_init_prototype()`, which has users only.
- APs save the table in NVS (`lgcfg/groups`), flood it on link up and every 30 s, send it to every handheld on registration, and print it with the new `groups` console command. Handhelds save it in NVS (`lghh/groups`), and a newer table that drops a group deletes that group's messages from the outbox, the inbox and the message list, and closes its chat. Handheld console: `groups` shows members; `group new|set|rm` edits.
- Tests: `test_groups` in `test_core.c` (edit rules, table full, id reuse, both body layouts); `test_group_edits` in `test_messaging.c` across the two-hop chain (a handheld makes a group, a non-member is refused, the admin edits with duplicated backbone frames, an older forged table is not adopted, a malformed edit is counted, removal deletes messages on every handheld, a late joiner receives the table). The simulator gives every node and client its own roster with the old three groups as a fixture.
- An AP and a handheld from before this change disagree: the old ones still have the three fixed groups. Update APs and handhelds together.
- The admin page preview mock answers `/api/groups`.
- Verified: `tests/target` on SOUTH, `LG_TESTS: 562 checks, 0 failures`, `LG_TESTS_RESULT: PASS`. All five boards flashed, APs one at a time. Over serial: the Freenove's `group new Cooks 1,2` reached both handhelds as version 1 within 100 ms; the Hosyond's `group set 2 Taken 2` on a group it is not in was refused (`status 4`, "Only members can change this group"); its `group rm 1` reached both as version 3, and the Freenove logged `Group 1 was removed; deleted its 1 message(s)`. NORTH, restarted afterwards, loaded `Groups version 3 (made on AP 0): 1 group(s)` from flash. Free heap after: Hosyond 110 KB, Freenove 165 KB. **Not verified**: the handheld group editor by touch, the admin page Groups card (needs a phone on the network), and a group message after a change, because grid time was unset after all three APs were reflashed.
- `tools/bench_devices.json`: MAIN moved to COM21 and SOUTH to COM16, confirmed with `id`.
- `tests/target` had the same newer-LVGL deprecation warnings as the handheld; its `dependencies.lock` was pinned to 9.5.0 as in the main checkout.


### Fixed (handheld: leaving a chat hung or restarted the Hosyond; emoji clipped in text)
- The notification banner and the chat keypad kept pointers to one-shot LVGL timers, which LVGL frees when they fire. The next banner wrote to freed memory. The Hosyond then hung in `lv_event_mark_deleted` while deleting the chat screen (task watchdog), or panicked there with LoadProhibited. Both timers now pause themselves and are never freed behind the pointer.
- Screen entry points (`ui_chat_open_conversation`, `ui_chat_open_list`, `ui_home_open`, `ui_launcher_open`, `ui_settings_open`) ignored `lg_display_lock()`'s result. After a 1 s timeout, or before the display started, a console command drew without the lock and corrupted LVGL's event list. They now wait up to 3 s and skip the change, with a `[UI] Display busy or not started` warning, if the lock is not taken.
- Emoji inside text use a new 14 px face, `lg_font_emoji_14` (about 40 KB of C source), generated by `tools/build_emoji_font.py`, which now writes both sizes. The 20 px face was taller than 12 and 14 px lines and was clipped in bubbles and banners. Keys and icons keep 20 px. Text fonts that fall back to emoji take the taller line height of the pair.
- Verified on both handhelds over serial: 12 rounds each of `chat` then `screen home`, with two emoji banners 19 s apart. No watchdog, panic or lock warning; Hosyond free heap 117 KB afterwards. Before the fix the same run hung the Hosyond on the first round.

### Changed (handheld screens: bare icons, tighter chat, the brand mark)
- Icon buttons (`lg_ui_icon_button`) are a bare accent glyph with no box or outline, shaded only while pressed. Symbols draw at a new theme size, `font_symbol` (16 px on 240 px screens, 20 px otherwise); emoji stay at the emoji font's only size, 20 px. The target is at least one touch minimum wide and three quarters of it tall, with a small extended click area.
- Chat screen: message text and the entry field use `font_small`, bubbles have half the vertical padding, and the gaps between bubbles and rows are halved. Delivery markers draw arrows and the eye at `font_symbol`; the clock stays at 20 px.
- The launcher shows the admin page's mark (`assets/brand/localgrid-icon.svg`) beside "LocalGrid", rebuilt from LVGL arcs, lines and circles by `lg_ui_logo()`, because the build has no SVG renderer.
- Flashed to both handhelds: Freenove `[GRID] Registered with node 0 as device 1`, Hosyond `... as device 2`. Handheld firmware built for esp32 and esp32s3 with zero warnings. **Not verified**: tap reliability on the smaller targets, and how the mark reads at about 30 px.
- A fresh checkout resolved a newer LVGL than 9.5.0 that deprecates `lv_obj_add_flag` and friends, giving 46 warnings. `dependencies.lock` is gitignored, so the version is not pinned by the repository; `lg_ui`'s `^9.2.0` accepts it.

### Added (D51: text-to-speech for handhelds without a microphone)
- Owner, 2026-09-16: classic CYDs have no microphone, so they need text-to-speech to talk. English only. SAM is accepted for the prototype and must be replaced before release. Recorded as D51 in `docs/DECISIONS.md`.
- Next is a spike on the Hosyond, ahead of milestone 1R. No code yet.

### Changed (alerts stay until their X is tapped)
- Owner, 2026-09-16: an announcement or emergency must not go away unless its X is tapped (D41 revised). Before, an announcement cleared itself after 6 s, and either kind was cleared by a tap anywhere on the screen.
- `ui_alert.c` now puts an X in the top right corner for both kinds, replacing the emergency's Dismiss button. The announcement's 6-second clear timer is removed. The cover still takes every touch but does nothing with it, so a stray touch neither closes the alert nor reaches the screen underneath. An announcement still stops flashing after four cycles and chimes once. An emergency keeps flashing and repeats its siren until closed.
- Unchanged: an emergency still replaces an announcement that is showing, and a newer announcement replaces an older one. The replaced text is still in the Everyone conversation.

### Added (a handheld can rename itself, D50)
- Before this, a handheld's name was compiled into `lg_roster.c`, so nobody could change it. Settings > Device > Name now opens a one-line field over LVGL's keyboard. OK saves and returns to Settings, and the hide key returns without saving. The console has `name` to show the name and `name <new name>` to change it. A name is 1 to 23 bytes of UTF-8, and the screen explains when one is too long (emoji count as 4 bytes).
- Protocol: new message type `NAME` (0x23, SYSTEM). The body is u32 device, u32 version, u8 length, and the UTF-8 bytes, decoded to an exact length. The handheld sends its name when renamed and on every registration. An AP keeps the newest version per device, floods a newer one to every AP, and pushes it to its own handhelds. It also gives a registering handheld every name it holds, floods them all on neighbour link up and every 5 minutes, refuses a handheld naming any device but itself, and returns a newer copy to a handheld that sends an older one. A record that is not newer is dropped and not forwarded, so floods stop.
- Versions are at least `boot << 12` and grow by one per rename, so they keep growing across reboots with no extra NVS counter.
- Sticky (D48, D49): handhelds keep every name they know, and APs every name they hold, in NVS as one blob of packed NAME records, rewritten only when a name changes. `[GRID] Device N is now called "..."` is logged on both.
- The status snapshot, the people list, and the `[GRID]` and `[MSG]` log lines use the chosen name, falling back to the roster name.
- Tests: `test_names` in `tests/target` covers the wire format and its refusals, a two-hop spread to every AP and handheld, a second rename replacing the first, re-registration sending nothing new, duplicated backbone frames, a late joiner, a reflashed handheld getting its name back, a split grid healing on link up, restoring from flash, and refusals of a spoofed device and a wrong length.
- Memory: 36 bytes per device per table, about 1.2 KB each for `lg_client_t` and `lg_node_t`, plus two 1 KB static blobs for flash I/O.
- **Not yet on the boards**: `tests/target` has not run on a board, and the AP and handheld firmware have not been flashed, because the chaos run holds the bench.

### Changed (Settings uses the theme's colours, in Gume's layout)
- The tab strip was the only `lv_tabview` on the handheld, and nothing restyled it, so LVGL's default light theme drew a white strip, tab names in `t->text` that barely showed on white, and a blue selected tab with a blue underline. The new `lg_theme_style_tabview()` gives the tab strip the bar colour with muted names. The selected tab takes the page's surface colour, an accent outline, and the bar text colour. Pages lose the default padding, and the scrollbar uses the outline colour.
- Rows follow `docs/mocks/settings-screens.html`. Read-only facts are compact text rows with a hairline (`ui_list_fact`), so only controls look tappable. Main actions are filled with the accent colour (Reconnect, Test sound), and Restart is filled red (`ui_list_button`, `lg_theme_style_button_filled`). Volume is four buttons with the current level filled (`ui_list_choice`), so one tap picks a level instead of stepping through them. "Calibrate touch: needed" uses the warning colour.
- Grid also shows which AP the handheld is connected to and whether grid time is set. Its note now says time is set on any AP's admin page (D45). Sound gains Test sound and Test urgent. The urgent test plays at full volume even when muted (D40).
- `docs/mocks/settings-screens.html` shows the screen before and after, drawn from the LVGL default theme source and the Terminal theme.

### Fixed (a fresh clone builds again: LVGL pinned to 9.5)
- `components/lg_ui/idf_component.yml` asked for `lvgl/lvgl: "^9.2.0"`, and `dependencies.lock` is gitignored, so a fresh clone or worktree took the newest 9.x. LVGL 9.6.0 deprecates `lv_obj_add_flag` and `lv_obj_remove_flag` in favour of per-flag setters such as `lv_obj_set_hidden()`, and the handheld build failed on both chips with 42 warnings. The manifest now asks for `~9.5.0` (9.5 patch releases only), the version the main checkout was already locked at. Moving to 9.6 touches `lg_ui` and the handheld screens and is left to the owner.
- `dependencies.lock` stays uncommitted: it records one chip target and is rewritten every time the other target builds, so the pin in the manifest is what holds the version.

### Added (FNK0104B sound: an ES8311 codec backend, built but not flashed)
- The Freenove FNK0104B had no sound because only the classic ESP32's DAC was ever driven: `lg_bsp_audio_start()` refused the codec board with `Codec at 0x18 not driven yet`. `lg_bsp_audio.c` now has a second backend for the ES8311, chosen by the board profile (D9), under the same cue vocabulary.
- Control goes over the touch controller's I2C bus at 0x18 (100 kHz), and samples go over I2S with the `i2s_std` driver: ESP32 as master, 16 kHz, 16-bit Philips stereo, MCLK at 384 times the rate (6.144 MHz) on GPIO4, BCLK 5, WS 7, DOUT 8. The microphone input is not used. The register sequence is the playback part of the ES8311 bring-up, with the codec's own coefficient row for 6.144 MHz at 16 kHz (pre-divider 3, multiplier 2, DAC oversampling 0x20, BCLK divider 4). It was written from the datasheet and Espressif's ES8311 driver, checked against Gume's working Arduino setup, and no GPL code was copied.
- The ESP32-S3 has no audio PLL on ESP-IDF v6.1, so MCLK comes from the 160 MHz PLL through the I2S fractional divider. Espressif's ES8311 example uses that same rate and multiple on the S3. The board profile comment that said MCLK must come from the APLL is corrected.
- Sines are generated in 128-frame blocks with a continuous phase, and every change of loudness is eased over 3 ms, so notes and rests do not click. The four steps (0, -6, -12, -18 dB), the volume setting, and urgent always playing at full (D40) behave as on the DAC board. The DAC volume register resets to silence, so it is set once from the profile's `max_volume` (85% is register 0xBC), converted through decibels because the register is logarithmic.
- The codec, its I2C device, and the I2S channel are claimed by the audio task at the first sound, because the touch driver opens the shared bus during the display start, after audio starts. A sound asked for before then gets `ESP_ERR_INVALID_STATE`. A codec that does not answer is logged once, and later sounds are refused.
- The amplifier (GPIO1, active low) and the I2S clock go on together before a cue and off together 1.5 s after the last one, so a run of cues does not pop the amplifier each time. The DAC board keeps its 60 ms tail.
- The console's `tone` and `cue` now print why a sound was not played. On the codec board `tone` refuses 8000 Hz and above, which would alias.
- Cost: the I2S driver adds about 25 KB to the handheld image on both chips (esp32 1172 KB, 24% of the app partition free; esp32s3 1150 KB, 25% free). The codec path uses 512 bytes of sample block, four 256-frame DMA buffers (about 4 KB, claimed only at the first sound), and a task stack of 3 KB instead of 2.5 KB.
- Handheld and tests builds for esp32 and esp32s3 have zero warnings. **Not flashed and not heard**: the chaos run is using the boards. To check it on the FNK0104B, look for `[AUDIO] ES8311 at 0x18 up` after `cue received`, then try `tone 1000 500`, `volume low`, and `cue urgent`.
- **Verified on the FNK0104B, 2026-09-16:** flashed with the chaos-run fixes (release build, `app-flash`). Boot logs `[AUDIO] ES8311 at 0x18, I2S MCLK 4 BCLK 5 WS 7 DOUT 8, amplifier on GPIO1 active low; set up at the first sound`; the first notification during chaos run 4 logged `[AUDIO] ES8311 at 0x18 up: 16000 Hz, MCLK 6144000 Hz on GPIO4, ceiling 85% (register 0xBC)`, and the owner confirmed the notification sound is audible. Popping and loudness not yet judged.

### Fixed (handhelds crashed in malloc and free after a notification banner, found by the chaos run)
- The first hour-long chaos run crashed the FNK0104B twice and the Hosyond once, each 50 to 120 ms after `[UI] Notified: ...`: `assert failed: remove_free_block ... next_free field can not be null` in `free()` from the network task, `StoreProhibited` in `malloc()` from the drawing task, and `LoadProhibited` in lwIP. Two unrelated tasks dying inside the allocator means the heap was corrupted earlier by someone else.
- Cause: `lg_ui_toast()` created its hide timer with a repeat count of 1 and kept the pointer. LVGL 9 deletes a timer whose repeat count runs out (`auto_delete` defaults to true, `lv_timer.c`), so 15 s after a banner the pointer was freed memory, and the next banner's `lv_timer_set_repeat_count`, `lv_timer_reset` and `lv_timer_resume` wrote into the heap. Every banner shown more than 15 s after the previous one did this.
- The SMS keypad's pause timer in `ui_chat.c` had the same bug, plus a double free: after it had deleted itself, leaving the chat deleted it again.
- Both timers now have auto-delete turned off, so a finished timer pauses and the pointer stays valid. The other timers in the handheld repeat forever and were not affected.
- `tools/chaos.py`: run from a git worktree it now takes the main checkout's `tools/.flash.lock`, so flashes from other sessions wait for it. It also records `CORRUPT HEAP`, task watchdog, LVGL `Asserted at`, stack smashing and stack overflow output as findings.
- **Second cause, found by the debug build:** `ui_launcher_start()` released the display lock and then called `ui_alert_start()` and `ui_notify_start()` from the main task. The first builds the alert layer's widgets on LVGL's top layer, and the second creates a timer, both while the drawing task could be walking the same tree. With LVGL object checks on, the Hosyond crashed on every boot about 5 s in, reading freed child arrays (`A8 = 0xfefefefe`, the heap poisoning fill) inside `lv_obj_is_valid`. Both calls now run under the lock. The normal build lost this race only now and then, which may explain some of the earlier crashes too.
- **Third:** `lg_ui_screensaver_set_enabled()`, called by the console's `saver` command from the console task, and `lg_ui_screensaver_dismiss()` could delete the saver's widgets without the lock. Both take the recursive lock themselves now.
- **Still open:** every caller ignores `lg_display_lock()`'s return value, so a lock wait that times out after 1 s would go on unlocked. No timeout has been seen.
- `tools/chaos.py`: readers stop before ports close (Python crashed with exit 139 closing ports under blocked reads after run 1), a reset pulse clears the victim's links and registration (run 1 reported a pulse "recovered in 0.1 s" before the board had even restarted), and a board silent for 90 s (APs) or 240 s (handhelds) gets its reset lines released again, then its port reopened, recorded as a harness finding. Twice in run 1 a CP210x port went silent with no error while its board was fine.
- Verified: debug builds with all three fixes, zero warnings for esp32 and esp32s3, flashed by `app-flash` to both handhelds. Both reach `Launcher ready` with no crash output over 40 s; the Hosyond crashed on every boot before the lock fix. **Chaos run 2 on this firmware is in progress.**

### Fixed (handheld serial log no longer shows 1:1 message text)
- AGENTS.md forbids logging the plaintext of 1:1 messages, but the handheld service logged it on receive (`[MSG] From ...`), on send (`[MSG] Sent to device ...`), and on refusal (`[MSG] Not sent (...)`). For 1:1 messages those lines now carry the message identity and length instead: `[MSG] From 1 (Handheld 1): 1:1 boot 44 seq 7, 12 bytes`, `[MSG] Sent to device 2: 1:1 boot 44 seq 7, 12 bytes`, `[MSG] Not sent (outbox full): 1:1 to device 2, 12 bytes`. Author, boot, and seq match the sender's Sent line to the receiver's From line. Broadcast and group lines still show the text.
- `tools/chaos.py` matches direct probes by message identity, since 1:1 text is no longer logged: the sender's `Sent to device N: 1:1 boot B seq S` line ties the oldest unsent direct probe to (sender, boot, seq), and the receiver's `From` line with that identity delivers it. A `Not sent ...: 1:1 to device N` line refuses it. `--self-check` has 5 more samples and passes. Checked offline with a scripted refuse and out-of-order deliveries. Not run on the boards.

### Added (chaos runs: APs and handhelds taken out at random, built but not run)
- `tools/chaos.py` takes APs and handhelds out at random for a set number of hours while the handhelds message each other, then writes `chaos-runs/<start>/summary.md`, hourly raw serial logs and `events.jsonl` (gitignored). Faults come from the USB serial bridge with no firmware change: an RTS pulse (crash and restart) on any board, or EN held low for 5 s to 10 min (power loss) on the Elegoo APs and the Hosyond. The FNK0104B is never held, because its serial port is the ESP32-S3's own USB.
- Measures: backbone relink time, handheld failover to another AP, time to steady state, delivery and latency of broadcast, 1:1 and FAMILY messages sent with both handhelds registered, restarts nobody injected, with their reason, crash output, lowest heap, and links kept by keepalive ACKs.
- Safety: at most all APs but one out unless `--blackout`, never both handhelds, no new fault until the grid is steady, every board released and every port closed with DTR and RTS low on any exit, the flash lock held, and every port's device ID checked before the first fault. Seeded schedule (`--seed`). Without `--live` it prints the schedule and opens nothing.
- `.claude/skills/chaos/SKILL.md` documents the run.
- **Boards are discovered, not listed.** Every USB serial port is opened and reset, and each board's `LGID:` line supplies its ID, role, board code, and AP or device index. The device map only gives a known ID its bench name, and its ports are ignored. Whether a board can be held in reset comes from its port's USB vendor ID: separate bridges (CP210x, CH34x, FTDI) can be held, the chip's own USB (Espressif 303A) gets a pulse instead. The group for group messages comes from the handhelds' `groups`. The schedule names slots (AP1, HH1), filled in index order. This replaced port numbers, board codes and a group name copied into the script, which had already broken: after a re-plug, node-main answered on COM21 and COM18 was gone.
- **Not run on the boards**, on the owner's instruction. Checked offline only: `--self-check` (20 parser samples taken from the firmware's log formats, safety rules over 200 seeded schedules, 0 failures), and the live code path against a fake serial grid on the PC, including failover, a handheld hold, and Ctrl+C during a hold (nothing left held, every port closed). The fake-grid runs found and fixed three harness bugs: an event key clash, a last experiment cut short and falsely reported as not recovered, and messages during unrelated faults being excused.
- **Open for the owner:** whether resetting boards and typing console commands from the PC fits D25, and whether to allow the all-AP blackout. Relays (D47) are not built, so there is no relay fault yet.

### Added (admin page: outage log filter)
- "What happened to the APs" has a button per AP with its outage count, plus "All APs". The choice survives the 5-second refresh and is remembered in the browser; buttons are rebuilt only when their counts change, so a tap is never lost to a refresh.

### Fixed (admin page: availability looked frozen)
- The availability strips cover two hours at one sliver a minute, so a few new minutes were invisible at the right edge, and the history kept before the packed-record change was dropped at that flash. Each AP now also shows how much of its recorded time it was reachable (for example `100% of 12 min`), which changes every minute, and the strips have a `2 h ago / 1 h / now` axis. The hint no longer says the history is lost on unplugging.
- Availability strips drew white on a phone: their colours were `var()` inside the SVG `fill` attribute, which that browser ignores. Colours now come from CSS classes (`.av0`–`.av3`), and the page declares `color-scheme: dark`.
- The page no longer scrolls sideways on a phone: grid and header children may shrink (`min-width: 0`), so the wide AP table scrolls inside its card.

### Changed (D48 sticky, D49 compact: AP history in packed bytes, kept, shared, and decoded by the browser)
- Two standing rules. D48: anything a device learns that others could use survives restarts and is shared with neighbours. D49: devices keep and send compact bytes, and the browser or PC tool does the decoding.
- `grid_state.c` rewritten around one packed sticky record: where and when grid time was set, 16 incidents at 16 bytes each (`grid_incident_t`: kind and reset reason share a byte; duration in 16-bit seconds; previous run in 16-bit minutes), and availability at two bits per minute (30 bytes per AP instead of 120). The record is about 0.5 KB in RTC memory, which survives a crash, watchdog, software restart, or brownout. It is also saved to NVS every 10 minutes, at once when anchored, and whenever an outage is explained, because on these boards a power-on reset (opening a serial port, pressing EN, unplugging) clears RTC memory.
- On boot an AP takes the record from RTC memory or from flash, adds the minutes it was down once grid time is known, and marks any outage still open as unexplained.
- APs flood their incident log (entries sent exactly as stored) and their packed availability. Receivers merge what they lack: incidents are deduplicated by AP within 90 s, and availability fills only minutes with no record, lined up by grid-time minute. A neighbour that hears an AP with under 3 minutes of uptime resends at once instead of on its 5-minute cycle.
- New `GET /api/history`: the APs, their availability, and the incident log as a binary record (layout documented at `h_history`), decoded with `DataView` in the page. Restart-cause text now lives in the page. The JSON status buffer shrinks from 6 KB back to 3 KB, and the history reply is about 0.5 KB for three APs where the JSON was about 4.5 KB.
- The incident log now shows which AP saw each outage.
- Verified on MAIN after a power-on reset: `History kept from flash (RTC memory was cleared by a power-on reset): 1 incident(s), time setting generation 1`. Its status shows all three APs and the anchored availability. Heap after 30 s with two stations: 48 KB free, 39 KB lowest.
- Recovered along the way: NORTH's app partition was erased when an earlier flash was interrupted. It was reflashed with its identity and settings intact.
- Known: after several restarts with nobody at stratum 0, strata climb (MAIN 5, NORTH and SOUTH 4). It is harmless, since only lower strata are taken, but "hops from source" stops meaning much until the time is set again.

### Changed (grid time in milliseconds, with a stratum, and no jumps)
- `TIME_SYNC` is 8 bytes: grid time in seconds, milliseconds into that second, quality, and stratum (the distance from the AP where an admin set the time: 0 there, one more per hop, 255 unknown). The old 5-byte form still decodes, as stratum unknown. A new optional `io.time_now` lets the core send milliseconds and stratum, and `io.on_time` receives the whole message.
- APs keep grid time as a millisecond offset. They take time only from a lower stratum than their own, or when they have none, so time flows outward from where it was set. Before, carrying APs corrected each other in whole seconds and pushed each other back and forth: the owner saw jumps of up to 2 s.
- Differences under 100 ms are ignored. Corrections under 1 s are slewed in at 100 ms per second, so the clock never steps. Only a larger difference, or an AP with no time, steps at once.
- Every AP holding time now repeats it every 60 s, not only the one it was set on, so each AP's last sync stays recent. Setting the time from the admin page passes its milliseconds.
- Grid state layout 4 adds the sender's stratum. `status`, and the admin page's AP table, show sync corrections in ms and hops from the source.
- The time card names the AP this AP carried the time from, when it last synced, the correction, and how many hops it is from the source. It also says which AP the admin originally set the time on and when, or plainly that this is no longer known because every AP that saw the setting has restarted.
- Tests: `tests/target` gains TIME_SYNC encode and decode checks (milliseconds, stratum, the old length, a rejected 1000 ms) and a check that milliseconds and stratum cross two hops. 447 checks passed on NORTH.
- Verified on three APs with time set on NORTH's console: NORTH stratum 0, MAIN and SOUTH stratum 1, each within +71 ms and -81 ms of NORTH after syncing. The largest correction applied was a 317 ms slew, with no whole-second steps. All five boards run this firmware.

### Added (APs watch each other: uptime, time source, availability, and what happened)
- Grid state layout 3 (189 bytes; every AP must run it). Each AP's broadcast now carries its name, uptime, time quality, the AP it last synced time from with the age and correction of that sync, the grid time an admin last set with the AP it was set on, and its boot counter, the reason its current run started, and how long its previous run lasted (from the power trace, which a brownout keeps).
- Every AP keeps a table of the others from those broadcasts, aged to now. `status` lists each AP with uptime, time quality, and last sync.
- Outages are detected and explained. When a backbone link to an AP drops, the watching AP logs `[GRID] AP 2 SOUTH is unreachable`. When the AP returns, its broadcast says why: a new boot counter gives `was unreachable for 3 s: it restarted after <cause>` (brownout, crash, watchdog, power-on or reset), and an unchanged one gives `kept running: the backbone link dropped`. The last 16 incidents are kept.
- Availability: every second each AP records which APs it can reach, closing one entry a minute. The last two hours are kept per AP.
- The admin page shows all of it:
  - The time card says which AP the admin set the time on, when, and which AP the page's AP carried it from.
  - The AP table lists uptime, time state, last sync, last restart, and signal.
  - An availability strip per AP shows two hours, minute by minute.
  - "What happened to the APs" lists each outage with its duration and cause.
- Fixed: an AP that came back with no grid time could wait forever. Time was repeated only by the AP the admin set it on, and announcements made at link-up were lost when the returning AP had not yet confirmed its side of the link. Now any AP that hears a neighbour announce "no time" sends its own within 5 s.
- `tools/flash.py` and `tools/serial_capture.py` release RTS before DTR when closing a port. The old order pulled EN low on the way out, so every tool run restarted a board twice, and the unseen second restart wiped grid time on every AP a tool had touched at once.
- APs log `[UP]` every second (uptime, heap, stations, links, frames sent, events). After any reset that keeps RTC memory, they print the last 16 of those seconds. The core task is under the task watchdog with panic enabled, and `CONFIG_LG_NODE_WIFI_TX_POWER_QDBM` caps transmit power for brownout A/B tests.
- Verified on the three APs: SOUTH was restarted twice in 3 s while MAIN watched. MAIN logged both restarts with their cause. SOUTH had grid time again 3.5 s after its last boot (`AP 2 has no grid time; sent ours`), and all three report CARRIED time with their sync sources. The web page itself has not been viewed yet.
- Known cost: AP heap after boot is about 9 KB lower (a 6 KB status buffer and a larger web snapshot).

### Added (supply voltage on every board: `power`, `tools/power.py`, power skill)
- New component `lg_power` gives every firmware the same console command: `power` for one reading (16 ADC samples averaged), `power -m <s> [-i <ms>]` to monitor with each reading listed and then min, average, and max, and `-q` for the summary only. Tool lines start with `POWER:`, `POWER_SAMPLE`, or `POWER_RESULT`, and every reply ends with why this boot started.
- Board profiles gain `supply_sense` and `supply_divider_milli`, taken from Braino's measured profiles: Hosyond GPIO34 and FNK0104B GPIO9, both 2:1 into ADC1. The Elegoo AP boards have no sense pin, so they answer "not measurable" and print their restart counts by cause instead. `CONFIG_LG_NODE_SUPPLY_SENSE_GPIO` names a divider if one is wired.
- `tools/power.py` queries one board, the handhelds, the APs, or all of them in parallel, so a hub that sags under radio load shows on every board in the same window. `.claude/skills/power/SKILL.md` explains how to read the table.
- Measured on the bench, all five boards on the shared hub:
  - Hosyond: 4.24 V now; over 60 s, 121 readings averaged 4.23 V (4.22 to 4.24 V).
  - FNK0104B: 4.17 V now; over 60 s, 121 readings averaged 4.17 V (4.17 to 4.18 V).
  - Neither handheld sagged while every board was running.
  - APs: brownout counts are MAIN 269, NORTH 280, SOUTH 277. MAIN's and SOUTH's are unchanged since earlier today, so they come from the hub overload. NORTH had no earlier count to compare against.
- Built with zero warnings (AP and both handheld targets, layers ok) and flashed to all five boards. `tests/target` does not carry the command.

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
