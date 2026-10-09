# Changelog

All notable changes to LocalGrid are recorded here, newest first.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions stay below 1.0 while the project is a prototype.

## [Unreleased]

### The air hold is bounded per episode, and hellos list heard peers

- The mid-flight hold was a ratchet: re-armed by every non-final part heard - resends and a
  talker's broadcast packets included - with the cap bounding each arming, never the sequence.
  Parts arriving under 2.6 s apart held a radio open without end. The hold is now capped per
  episode from when it first began, and a duplicate part arms nothing.
- A mutually dead LoRa pair could never relink while ESP-NOW was off: hellos listed only up
  peers, and a link rises only when the other side's hello lists this AP with its current boot.
  Hellos now list heard peers, so the LoRa mesh confirms itself without Wi-Fi's help.

### Discovery hellos run fast after a restart, so the LoRa mesh is whole in seconds, not minutes

- The one flaky chapter left ("Cut the Wi-Fi between access points") failed with every radio
  silent and every counter at zero: no convoy, nothing to send. All three APs restart together at
  the demo's warm-up, a LoRa link needs a round trip of boot-confirmed hellos, hellos went every
  60 s, and a hello the backbone could not build yet rescheduled a full minute away - the mesh took
  up to six minutes, and a message offered before then found no peer up and was dropped silently.
- A failed hello build retries in half a second; for two minutes after the module comes up, hellos
  go every ten seconds; and the demo waits for every pairwise link to confirm before cutting the
  Wi-Fi, saying so on screen.

### A talk's compact copy is acknowledged to a handheld too, and a partial note replays whole

- An AP's compact voice packet to a handheld on LoRa goes once and is acknowledged and sent again
  until it lands (D109), instead of twice with no answer: with the radio's quiet time a duplicate
  is expensive air, and unacknowledged it still lost a packet in three on the bench. A handheld's
  own notes stay said twice: its frames are broadcast, so no one AP owes it an answer.
- The owner's replay (2026-10-08): a note plays what arrived when it goes quiet, as before, but now
  stays open another 25 s; if the missing packets land - an AP resends them - the handheld joins
  everything and plays the whole talk once more, and the log says it was replayed whole.
- A talker spaces the compact packets 9 s apart, not 2.8 s: a packet is three paced parts and an
  acknowledgement, about 8 s of air, and packets offered faster were evicted from an AP's four-slot
  queue - a talk arrived 1 packet in 3. Late and in one piece is the compact copy's deal (D95).

### LoRa radios leave the air quiet after every packet, and hold while a frame is mid-flight

- The module also loses packets that follow the previous one too closely: back-to-back
  120-character frames 0.8 s apart arrived 14 of 16, while 2 s apart arrived 16 of 16, measured
  again on a quiet 867.5 MHz with the third radio off, so it is the receiver's own recovery, not
  the band or a collision. Every sender now leaves 1.5 s of idle air after each of its own
  transmissions (alerts excepted), on APs and handhelds alike.
- Carrier sense at the protocol layer: a part header says how many parts are still coming, so
  every radio that hears one holds its own non-alert sends for that much airtime instead of
  transmitting over the rest of the frame. The `lora` status counts the holds.
- Whole three-part frames between APs went from 3-7 of 8 to 7-9 of 8 unacknowledged; text and
  notes between APs also ride D104's acknowledge-and-resend on top.

### A LoRa part is 120 characters, because the module drops longer ones

- The demo's flaky "Cut the Wi-Fi between access points" chapter came down to the radio module
  itself: measured on the bench with a controlled transmit matrix (every AP flooding diagnostic
  frames, the other two as witnesses), the RYLR998 (firmware V1.2.4) delivers `AT+SEND` payloads of
  124 characters or less 24 of 24, 132 in 11 of 12, 140 in 2 of 12, and the old full part of 180
  almost never — on a quiet channel, at SNR 9, in every direction equally. A sealed text message
  was 147–176 characters: exactly the dead zone. Everest's antenna, first suspected, measured clean.
- A part now carries 120 base64 characters (90 bytes, 85 of payload), one measured step under the
  cliff; `LORA_PARTS_MAX` rises to 74, so the 6 KB voice-note ceiling stands (docs/lora.md updated).
  A full part is about 0.65 s on the air.
- Every arriving part is logged with its size and signal, a garbled part is said out loud, and the
  `lora` status counts module lines that parsed as nothing: the loss was invisible before.
- `tests/target`: 4543 checks, 0 failures on Fuji; part-count checks now derive from the constants.

### A voice packet between two APs goes once and is acknowledged (D95 amended)

- Between two APs a compact voice packet is now sent once and acknowledged and sent again until it is
  (D104), instead of twice and unanswered (`lora_frame_wants_ap_ack`). Sent twice, each packet took about
  3.8 s of air against one every 2.8 s from the talker: Denali's four-slot queue dropped 12 frames and a
  five-packet note reached Big Pepa as one. A handheld's own LoRa link is unchanged.
- On the bench after the change, Big Pepa got 2 of 5 packets, but that run is not a fair measure:
  Denali never heard Everest over LoRa after the restart, Everest still counted Denali as up 184 s after
  last hearing it, and Everest's signal from Fuji was SNR 1 against 9 to 11 on every other pair. The
  text message from Denali to Everest did not arrive for the same reason.
- A sender now listens 1.5 s past the last answering turn instead of 0.4 s (`ACK_MARGIN_MS`): an answer
  that comes due while the answering AP has a part on the air waits for it. Eight messages Denali to
  Everest, Wi-Fi backbone off: before, 7 of 8 arrived and Denali heard 5 of 11 answers; after, 8 of 8
  and 7 of 8 answers.
- `tests/target`: 4695 checks, 0 failures on Fuji, with new checks for which frames an AP answers.
- `tools/bench_devices.json`: Fuji is on COM16 and Denali on COM29 now.

### Handhelds balance one at a time again; the LoRa demos measured right

- **Balancing (D54).** The stagger by device number counted only from when a handheld came online, so
  handhelds that had been online a while all acted on the same scan. During the LoRa demo three left
  Denali for Everest in one second, and Everest went from one handheld to four. Each handheld now also
  waits its stagger from when it first sees the uneven load; the first to move leaves gracefully, and the
  next one's fresh scan already counts it. On the bench, with Fuji carrying three, only Marshall moved,
  and the load stayed 2, 2, 2. That first version restarted the stagger whenever the view went stale
  between scans, so only a handheld with no stagger ever moved; it now decides only on a fresh scan. With
  Marshall pinned to a crowded Fuji, Jolly moved and Chase, seeing an even load, stayed.
- **Demo.** The two LoRa demos pin their handhelds to their access points for the demo (`node <n>`, back
  to `node auto` after, and at warm-up), so the message and the talk really cross LoRa. A talk over LoRa is
  checked against the compact copy that crosses it (D95), by packets and checksum: Channi had played every
  byte while the page said it had not. `chaos.py` reads the `Note sent` and `Heard note` lines.
- **Demo page.** Traffic between access points over LoRa rides the diagram's dashed orange arcs, and
  traffic to a handheld on LoRa alone rides its dashed link, instead of the straight Wi-Fi lines.

### A handheld talks real speech made on the laptop (D112)

- The handheld console's `clip` command takes a few seconds of speech over the cable and talks it out
  through push-to-talk; `tools/voiceclip.py` makes the speech with Piper. On the bench a 6.4 s clip
  from Channi reached Bluey and Chase whole (65 frames, 0 missing).
- Five of the six bench handhelds were found with their volume off, left by an overnight chaos run,
  which is why talk was barely heard: all six set to high, and the demo now sets it when it starts.

### The site and docs say what is true today

- Site (`site/index.template.html`): chaos testing found five bugs, as the README lists, not three;
  setting up says the admin page runs in setup mode or when an admin turns it on (`web on`, or the
  Bluetooth admin link), not always at `http://192.168.4.1/` (D91); the map card says trails are
  kept in flash and single positions stay in memory (D106); push-to-talk reaches everyone too (D88).
- Site: no LoRa distance anywhere, since none has been measured. "Kilometres" is gone from the line
  table and the LoRa comparison, and "two sites a few kilometres apart" from the access point card.
- Site: the hardware table lists the Reyax RYLR998 LoRa module, and "Build it with us" links the
  installer (`flash/`) and the update signer (`sign/`). The legend's CSS, pasted four times, is
  there once.
- `docs/chaos-engineering.md` opens with five bugs, matching its own sections and the README.
- README and `docs/OPEN_SOURCE.md`: keys are no longer built in (D96); what still stops a download
  is that no release workflow builds and publishes images, and the site offers none.
- `docs/OPEN_SOURCE.md` positioning and `docs/ADS.md`: LoRa works as a second path between access
  points and to a handheld with a module (D71, D74, D104, D105, D77, D80, D109). It is still not a
  LoRa mesh of handhelds (D77, D47), and the no-range rule stays.
- `docs/DECISIONS.md`: D99's row notes that D106 amended it (trails are kept in flash).

### The site answers common questions (FAQ)

- A Questions section on the project site (`site/index.template.html`), linked from the top bar:
  what LocalGrid is and who it is for, how it differs from LoRa meshes, walkie-talkies and phone
  apps, hardware, capacity, range (not measured, and it says so), batteries, LoRa radio rules,
  encryption, who sees positions, failures and power loss, no store-and-forward, testing, install,
  signed updates, licence and contributing. Every answer cites the decision or document behind it.
- Native `<details>` blocks, so the section needs no script.
- The LoRa section no longer says LoRa is being built: access points use it with acknowledgement
  and fallback (D104, D105), and handhelds with a module use it too (D77, D80, D109).

### A demo that runs itself (`tools/demo.py`, `docs/demo.md`)

- A narrated presentation on a local web page that explains the project (what it is, why, how it
  works, the bench's hardware as the boards report it, the concepts, key decisions, chaos testing)
  and then proves each claim live, from one Play all button. Its words are in `tools/demo_story.json`.
- Thirteen live demos: the handheld screens, an SOS and its all clear, push to talk, an access point
  losing power, a crash, the Wi-Fi between access points cut so messages cross by LoRa, a handheld
  out of Wi-Fi range on LoRa alone, a split grid that agrees again, every access point off at once,
  an update that spreads itself, installing a board from the installer page, and, with every cable
  unplugged, a plug pulled, an SOS and a blackout done by hand and watched over Bluetooth only.
- Each checks the grid is ready, says what is about to happen, does it, shows it on a live map with
  a stopwatch, and ends with what the boards themselves reported. Then it puts the grid back.
- The page narrates every step with the browser's own voice, chosen on the page, and the demo waits
  for each line to be spoken before it acts, so a recording needs nobody speaking.
- The engine is the chaos harness, scripted: the same fault injection and log parsing, and the same
  rules (faults through the cable only, D103; the PC never joins the grid, D25).

### Handheld updates for every chip, shared across the access points (D111)

- Each access point carries one handheld image and the grid shares them out by chip; a handheld is
  offered the newest update for its chip within reach, and its access point fetches the pieces from
  the neighbour that carries it.
- An upload that would push out the grid's only copy of another chip's update is refused, and says so.
- Withdrawing (`spread drop`, or Withdraw on the admin page) reaches every access point.

### Handhelds report their firmware, and the time-zone hint is right (D110)

- A handheld tells the grid which firmware it runs at every registration; the admin page's and the
  dashboard's handheld tables show it.
- The admin page's hint says the zone rule handhelds actually get, including one the access point
  worked out from the zone's name, and says plainly when it has none.
- `tools/test_admin_page.py`: 126 checks. On-board tests: 4690 checks, 0 failures.

### The admin page gives an update, and every screen says what firmware runs (D110)

- **Settings > Firmware** on the admin page: what this access point runs (or "installed by cable"),
  whether it takes updates at all, and the handheld update it carries; and a file picker that reads
  an update's signed header in the browser, says what it is, refuses anything that is not an update,
  and sends it with a progress figure and the access point's answer.
- `/api/status` carries the same as `firmware`, so the dashboard shows it per access point.
- The handheld's Status screen has a `Firmware` row.
- `tools/test_admin_page.py`: 118 checks, six of them new for this card.

### An access point's LoRa message to a handheld is acknowledged and sent again (D109)

- The last hop D104 left out: a handheld on LoRa alone answers a text, a delivery report or an alert
  sealed for it, and the access point repeats the frame until it does, three times at most.

### The installer page is tested in a real browser, and found a flaw (D108)

- **`tools/test_installer_page.py`** puts LocalGrid on a bench board through the installer page the
  way a person would: a visible Chrome or Edge window, real mouse presses, the browser's own port
  chooser driven through Windows UI Automation, ESP Web Tools flashing without an erase, then the
  page's set-up step with this bench's grid keys, and the board checked over its cable afterwards -
  including that its boot banner names the published build and that no other board was touched.
- **Its first run flashed the wrong board.** UI Automation's "select" did not move the chooser's
  selection, so Connect opened the first port in the list: Denali was given handheld firmware. The
  test now presses Connect only when the board's own row, and no other, reads as selected, clicking
  it with the mouse if need be, and cancels otherwise.
- **That accident showed a real flaw, now fixed (D108):** wiping a board's settings reset its boot
  counter to 1 while its identity and keys survived, so it reused message ids and nonces and its
  neighbours rejected it as a replay. The counter's high-water mark is now kept with the identity.
- **And two bugs in the installer page itself, both of which made its last step fail for real users:**
  each pass of its serial reader abandoned the read a timer beat, so the board's answers could be
  thrown away; and opening the port restarts many boards, so `provision` reached a console that was
  not listening yet. The page now keeps one read waiting, waits for the board to go quiet after
  connecting, and asks again once. Bench: 25 of 25 checks on Chase, its boot counter carrying on
  from 195 to 219 through four installs that each wiped its settings.

### A handheld takes its update from its access point (D107)

- An access point given a signed handheld update keeps it in its spare firmware slot, never runs it,
  passes it to the other access points, and offers it to every handheld with a session.
- A handheld it is for fetches it over Wi-Fi 512 bytes at a time, checks it with the release key as
  any update is checked, and restarts into it when idle: not in a talk, not within five minutes of an
  urgent message, not below 30 % battery. One handheld per access point at a time.
- Status shows an `Update` row while one is arriving. `spread` on an AP says what it carries.
- One handheld image at a time, for one chip; an access point's own update displaces it.
- `spread drop` on an AP's console stops it carrying the handheld update and keeps it from taking
  the same one again from a neighbour. Each AP is told for itself.

### A trail survives every device being switched off (D106)

- A handheld keeps its own trail in flash, and every access point keeps the trails it holds, written
  when a trail has grown and at most every 30 s (handheld) or 60 s (access point).
- A handheld that starts puts new steps in front of the ones it kept. An access point that starts
  reads its trails back and shares them when a link comes up.
- This reverses D65 for trails, on the owner's instruction: a found device says where it went.

### A tick in the Messages header marks everything read (D94)

- The header of the Messages list shows a tick whenever something is unread; touching it does what
  the "Mark all read" row does. The row stays.

### A message ESP-NOW could not deliver goes to LoRa at once (D105)

- A text or a delivery report whose send to another access point fails is handed to the LoRa backbone
  straight away, as the same sealed bytes, instead of being lost during the six seconds it takes to
  decide the link is dead. It is acknowledged and repeated there like any message (D104).
- A frame stays at the head of the ESP-NOW send queue until the radio answers for it.
- `nodes` and `lora` on the console count the frames handed over.
- Bench: backbone off on all three access points, a group message one second later, all five other
  handhelds received it.

### Messages between access points over LoRa are acknowledged and sent again (D104)

- A text, a delivery report or an alert that crosses between access points over LoRa is answered by
  the access point that receives it, and repeated by the sender until it is: three times on the air
  at most, then given up and counted.
- The answer is six bytes sealed with the backbone key, naming the sealed frame's sequence number.
- Sent to every access point at once, a frame is answered by each in its own half-second turn, and
  a repeat is addressed to the one that has not answered.
- While an access point listens for an answer it transmits only alerts and answers of its own.
- A message that has been sent and is only waiting to hear so gives way, in a full queue, before one
  that has never been sent.
- `lora` on the console: acknowledged, sent again, given up, acknowledgements sent and received.
- Not acknowledged: voice, routine traffic, and the hop between an access point and a handheld.

### Screen saver: a battery icon and an envelope

- The battery figure has a battery beside it, drawn as full as the battery is (five steps).
- New messages are an envelope and the count, not "61 new messages". Nothing is shown when there are none.
- The first line is a handheld icon and the handheld's name, then the LocalGrid mark and the AP's name,
  in place of "Bluey on Fuji". The two icons are the project's own, drawn by `tools/build_icon_font.py`
  into `lg_font_icons_16`; the UI fonts' Font Awesome set has neither.

### An AP no longer takes a handheld that has a session elsewhere (D80)

- **Found by a chaos run, with no fault active:** a direct message between two registered handhelds was
  lost. The recipient had moved between APs early in the run and, in the half second without Wi-Fi,
  announced itself over LoRa. That frame is a second on the air and arrived after it had registered
  again; the AP that heard it claimed the handheld as "reachable by LoRa, through me", no other AP
  agreed, and nothing settled it. For 90 minutes that AP sent every direct message for the handheld
  over LoRa alone: 28 arrived and one did not.
- An AP hearing a handheld over LoRa leaves it alone while an AP that is up has a session with it.
- Two APs holding different answers at the same epoch now agree: a session beats a frame heard over
  LoRa, which beats nothing. An AP whose live session is contradicted announces it at once.
- A handheld held over LoRa alone is in an AP's periodic presence announcement, as one with a session is.
- `lora` on an AP's console says how many of the queue's drops were messages rather than routine
  traffic, so a LoRa-only loss can be told from the rule working.

### The admin page test covers the whole page, and found two bugs in it

- **`tools/test_admin_page.py`** now makes three visits in a fresh browser each: an AP nobody has set
  up, through setup and every tab (the clock and setting the time, the map and a trail, the AP table,
  LoRa and its power, availability, the outage log and its filter, handhelds, making, changing and
  removing a group, who may announce, changing the password), then out, back in with the old password
  and the new, and a session the AP ends under the page; an AP already set up, opened at a tab's own
  address; and an AP that does not answer. 109 checks. The stand-in AP keeps the state a real one does.
  One handheld's name in its data is markup, and the page must show it as text and run none of it.
- **Bug: the LoRa card appeared on every tab.** Each refresh un-hid it wherever the reader was.
- **Bug: the open tab was never kept in the address**, so a reload always landed on Overview. The
  page's own `history()` function hid the browser's `history`, and the failure was caught and dropped.
- The zone rule the page works out is checked against the AP's own table (D100).

### A LoRa heartbeat no longer starves behind messages (D102)

- Found by the low-power chaos run: an access point carrying a LoRa-only handheld had its heartbeat
  refused by a queue full of messages, three times running, and its neighbours dropped the link for
  three minutes. A heartbeat now asks first (`lora_txq_would_take`) and tries again in half a second.
  `lora` on an AP's console counts how often it waited.
- The run itself, every radio at low power: 4 of 4 rounds recovered, no findings, 56 of 56 messages
  between registered handhelds delivered across 204 s and 283 s with the Wi-Fi backbone off.

### Chaos control is out of the firmware (D103)

- **Removed:** the AP build that broke itself on a schedule (`chaos_self.c`, `chaos_log.c`, the
  `LG_NODE_CHAOS` options, the `chaos` console command, the journal's flash partition); arming it from the
  admin page (`/api/chaos`, `/api/chaoslog`); arming it over BLE (four opcodes); and `OP_FAULT`, a fault on
  request over BLE, which was in every image. About 1,200 lines of firmware.
- **Why now:** each named the same condition for its removal, a bench that reaches every AP by cable.
  Three chaos runs on 2026-10-05, eleven hours, were driven over USB and used none of it.
- **The BLE admin link is read-only again**, but for asking an AP to serve its admin page (D91). The five
  opcode numbers are retired.
- **Tools:** `tools/chaos.py` watches uncabled access points and injects nothing into them;
  `--admin-password` is gone. `tools/chaos_ble.py` is replaced by `tools/web_on.py`, which keeps only the
  admin-page request. `tools/grid_watch.py` no longer knows the retired opcodes.
- **Kept:** the console's `bb off`, `lora off`, `lora reset` and a handheld's `wifi off`, which need a
  cable and restore themselves.
- `tools/check_release.py`: T1, T2, T5 and T6 gone.

### A signed update passes from one access point to the next (D101)

- **`firmware/node/main/fw_spread.c`**: an AP running a signed update offers it; an older neighbour
  pulls it 512 bytes at a time through `lg_fwupdate`, restarts into it, and offers it on. One neighbour
  at a time, so APs never restart together. `spread` on the console.
- **`lg_core`**: `LG_T_FW_OFFER`, `LG_T_FW_GET`, `LG_T_FW_DATA` (0x60 to 0x62), carried one hop between
  APs, never forwarded, refused from a handheld. Never on LoRa.
- **`lg_fwupdate`** keeps the signed 192 bytes of the update a device is running (`lg_fwu_held`,
  `lg_fwu_held_read`), and erases the update slot as the image arrives instead of all at once.
- **On the bench:** an update given to Denali by cable reached Everest (1.18 MB in about two minutes) and,
  once it had a release key, Fuji. Handhelds are not covered: they still update by cable.

### In a full LoRa queue, messages outrank routine traffic (D102)

- Found by a chaos run: with the Wi-Fi backbone off, full queues discarded the oldest entry whatever it
  was, and two messages between registered handhelds were lost.
- A newcomer now replaces the oldest entry of the lowest rank, and routine traffic never replaces a
  message. One comparable round since: no message lost. Not yet enough rounds to call it settled.

### An access point gets about 7 KB back (D93)

- The backbone's receive queue was twelve slots of a whole frame, 12.9 KB of heap from boot. It is a
  6 KB message buffer of bytes, as the transmit side already was.
- At rest with two handhelds: Everest 30 to 35 KB free, Fuji 35 to 41, Denali 35 to 40. One AP alone
  with all six handhelds, where it touched 7 KB before, has not been measured since.

### LoRa power on the admin page (D98)

- The Network tab shows the AP's LoRa transmit power with what it is for, and three buttons;
  `POST /api/lora`. The setting was console-only on an AP.

### The admin page is tested in a browser, against a stand-in access point

- **`tools/test_admin_page.py`** serves `admin.html` as it is in the tree, answers `/api/*` with the same
  JSON and bytes an AP sends, and drives the page in headless Edge or Chrome: wrong then right password,
  the trail on the map and in its table with its map links, the no-Internet note, the LoRa power change
  with its session and CSRF token. 28 checks. It was run with the page broken three ways and failed each
  time. It replaces asking a person to look at the page on a phone; it does not prove a real AP serves it.

### OpenStreetMap was refusing the dashboard's map tiles

- **Cause, measured:** the tool called itself "LocalGrid-grid-watch/1.0 (offline network monitor; one
  laptop, no bulk downloads)". OpenStreetMap served the bare name and refused the name with "(offline
  network monitor)" after it: "offline" is what a map downloader calls itself, which its tile policy
  forbids. Every square came back as the "Access blocked" notice with a success code.
- **`tools/grid_watch.py` now names itself as the policy asks** - application, version, the project's
  address - and no longer sends an empty `Referer`.
- **A refusal is recognised from the `X-Blocked` header** that comes with the notice: it is never drawn
  as a tile, the plan is shown instead with the reason under it, and that server is not asked again
  until the tool is restarted. The same-picture test stays as a backstop, back at four in a row.
- Not changed: tiles are kept in memory only, where the policy asks for a week's cache; the docs say so
  and why. `--self-check`: 86 of 86.

### A screen asked for while the screen saver was up was drawn over the rain

- **Found by the owner:** remnants of a screen showing through the screen saver. A screen *change* -
  from the console, in this case the agent's own test commands - painted the new screen while the saver
  held the panel, and the rain carried on over it. The saver now gives way to a screen that is asked
  for and starts its idle clock again; an alert does not give way, and the change waits for it.
- The table of screen names used in the `[UI] Screen:` log line had fallen two behind the list of
  screens; it is checked against it at build time now.

### The home screen: a clock of its own, and how long on this AP

- **The time has its own box, with AM or PM**, beside "LocalGrid" where the screen is wide enough for
  both and the battery (320 pixels), and at the right of the line below where it is not (240).
- **The line under the title says how long this handheld has been on its AP** - "Big Pepa on Fuji for
  12 min" - where it used to end in the time, which read as part of the sentence. Where the clock
  shares the line it shortens itself ("On Fuji for 12 min") rather than lose its end.
- **The screen saver is two lines, not four:** "Bluey on Everest", then "12:14 PM   98%". A third line
  appears only when there are new messages, and says how many; it no longer says "no messages".
- `ui log` on the home screen prints both boxes and what the line needs, so this was read back from
  all six handhelds.

### Breadcrumb trail: a person's last steps, kept by every AP (D99)

- **Protocol:** `LG_T_TRAIL` (0x25) and `LG_T_TRAIL_REQ` (0x26) in `lg_core`, with a table of four
  trails on each AP. Newest fix wins, a full table gives up its stalest, and a trail's newest point is
  taken as the handheld's position. `DESIGN_REVIEW.md` has the layout.
- **Handheld:** a point every ten metres moved, the newest sixteen kept in RAM, sent as the trail grows;
  over LoRa at most once a minute when there is no Wi-Fi. A restarted handheld takes its earlier steps
  back from the grid.
- **Screens:** people with a position are rows on Status that open their trail, with or without a GPS
  on the handheld looking; `My trail` on one that has a GPS, with the switch that stops sharing it.
  The page draws the steps north-up with the ends named and lists every step with its coordinates. The
  list widget gained a row a screen paints itself.
- **On a real map:** `GET /api/trails` on the admin page and `GET_TRAILS` on the BLE admin link carry
  the trails an AP holds as bytes. The admin page and `tools/grid_watch.py` draw each as a line through
  its steps on their OpenStreetMap view and list each step with its coordinates and a way to open it in
  OpenStreetMap or Google Maps.
- **Edge cases:** one step (a place, no picture), a loop, no clock, not connected, asking over LoRa,
  and GPS jumps or poor fixes, which are not recorded as steps.
- **`grid_watch.py`:** its plan view, the one a laptop without Internet relies on, was never shown: it
  is an SVG element and was un-hidden through a property SVG does not have. Fixed. It also gives up on
  map tiles after two identical pictures, not four, having shown OpenStreetMap's "Access blocked" notice
  as if it were the map.
- **Consoles:** `trail`, `trail <device>`, `trail ask|show <device>`, `trail share on|off`, and
  `trail test <steps>`, which walks a made-up trail so this can be tested on a desk; `trails` on an AP
  says whose it holds and how old, never where.
- **LoRa:** a handheld may send its trail and ask for one; between APs a trail waits for spare room.
- **On the bench:** a walk on one handheld was held by all three APs, opened from a handheld on another
  AP, survived its author restarting and an AP restarting, and arrived over LoRa from a handheld with
  Wi-Fi off in 4 to 7 s; the dashboard pulled two trails from an AP over BLE and drew them.
  `LG_TESTS_RESULT: PASS`, 4375 checks; `grid_watch.py --self-check`, 80 of 80. Not walked outdoors
  yet, and the admin page's drawing not seen in a browser on the grid.

### Handhelds showed UTC on a grid that knew its zone only by name (D100)

- **Found by the owner:** a handheld's clock read 15:14 when it was 10:14. Every AP held the zone's
  name, `America/Chicago`, but not the rule handhelds need; the settings predate D67, and a grid on GPS
  time never sends anyone to the admin page to save it again.
- **An AP with a name and no rule now looks the rule up** in a short table of common zones
  (`firmware/node/main/tz_names.c`). A rule saved on the admin page still wins, and `config` says which
  it is using.
- **On the bench:** flashing the three APs alone moved all five handhelds from UTC to local time.

### LoRa transmit power can be turned down, with an explanation of why (D98)

- **Three levels, full (22 dBm, the default), medium (14) and low (5)**, kept on each device across
  restarts and applied to the module at once. Nothing changes for a device nobody touches.
- **Handheld:** a `LoRa power` row on the Settings grid tab, only when a LoRa module is fitted, opening
  a page with the three choices and what they are for: lower it only when devices sit close together,
  where full power drowns out the radios beside it.
- **Console, both firmwares:** `lora power [full|medium|low]`, which prints the same explanation.
- **`ui log` now lists the rows of the list on screen** with their position and height, and marks a note
  that is cut short or a label and value too wide to share a row, so a list screen can be checked on the
  board over serial. `screen lorapower` opens the new page.
- **On the bench:** two FreeNoves and one access point took each level (`+OK` from the module) and kept
  medium across a restart; Settings and the new page both fit the 240x320 screen without scrolling, with
  nothing cut or overlapping. What each level does to delivery and range is not measured.

### A device takes a signed update, and only a signed update (D97)

- **`firmware/common/lg_fwupdate.c`**, in both firmwares: checks the first 192 bytes of an update file
  (signature against the device's release key, role, chip, serial higher than what runs) before
  anything is written, then writes the image to the spare slot, hashing it as it arrives, and makes it
  bootable only if all of it matches the signed header.
- **The admin page's `/api/update` takes a signed update file and nothing else.** The admin password
  alone no longer replaces an access point's firmware (T3).
- **`update`** on both consoles (`update`, `update begin`, `update data <base64>`, `update end`), and
  **`tools/lgfw.py send <file> <board>`**, which types a file down the serial cable.
- **`lg_crypto`** gains SHA-256 in pieces (`lg_sha256_begin/update/finish/abort`), tested against the
  one-shot at six ways of cutting the input and against the FIPS 180-4 example. `LG_TESTS_RESULT: PASS`,
  4221 checks.
- **On the bench:** a handheld and an access point each installed a signed update, restarted into the
  second slot, reported serial 1000 and its version, and rejoined. Refused, each with its reason: another
  key's signature, access-point firmware on a handheld, a handheld with no release key, the same file a
  second time, and an image with one byte changed.
- Both consoles now take lines of up to 1024 characters; the handheld's 128 could not have held a setup
  bundle that carries a release key.

### An installer page, images that can be published, and two firmware slots on every handheld

- **`site/flash/index.html`**, published at `/flash/` by `tools/gen_site.py`: choose what the board is
  to be, flash it from the browser (ESP Web Tools over Web Serial), make or open a grid file, then
  connect and set the board up - `identity new` and `provision` typed at its console by the page. The
  board list comes from the device map. The setup bundles the page makes are byte for byte the ones
  `tools/provision.py` makes, which the boards accept. **The connect-and-set-up step has not been run
  against a board**: a browser will only open a serial port a person has picked.
- **`tools/build_release.py`**: builds each firmware with `LG_PUBLISHED_BUILD`, which compiles no
  `lg_secrets.h` in at all, merges bootloader, partition table and app into one file per (firmware,
  chip), and writes the manifest the page's flash button reads; with `--key` it also writes the signed
  update files. `tools/gen_site.py --firmware DIR` publishes them. Without them the page says no image
  is published and offers only the setup steps.
- **Proved on a board:** the four published images contain neither the bench passphrase nor the
  backbone key (the bench image contains both). The published handheld image was written to Marshall as
  the page's button would write it; it started, reported `unset` and showed "Not set up yet: connect a
  cable to give it your grid"; given its keys over the cable it restarted, reported `set` and came
  online.
- **Handhelds have two firmware slots**, the layout access points have had since D81, with rollback: an
  image that arrives as an update runs once on trial and is kept once the handheld has started. This
  moves the identity partition, so every handheld was re-flashed once over USB with
  `--update-identity`. All six are back online.
- **Big Pepa's identity was written by Big Pepa.** The flashing tool could not write its identity
  partition (the CH340 and esptool's stub again), which left it with firmware and no identity; the new
  `identity new` console command gave it one over the same cable. Its ID is now
  `LG-H-E40-TJGVKGNFR7`.

### A board can be given its identity over the cable

`identity new <board> <unix seconds> <index> [name] [replace]` on both consoles: the board mints its own
ID from its MAC, the time it is told and its own random bytes - what `tools/flash.py --new-id` does from
the PC - and writes the identity partition itself. The role comes from the firmware, never from the
caller. A board that already has an identity keeps it unless the last word is `replace`. This is the
second thing a web flasher needs (D96): a board the bench has never seen can now be set up with nothing
but a serial line.

On the bench: the Hosyond refused without `replace`, refused a lower-case board code and an index out
of range, then minted a new identity, restarted, verified it against its own MAC and came back online
as device 2. **Its ID is now `LG-H-HY3-CB54KHSXJ6`**, and the device map says so.

### A device holds its grid's keys (D96): the first step to a web flasher

- **`firmware/common/lg_gridkeys.c`**: both firmwares read the Wi-Fi passphrase and keys from the
  device's own storage, and fall back to `lg_secrets.h` only when the device has none and the build has
  real ones. Every use of a compiled-in secret now goes through it.
- **`provision`** on both consoles takes a setup bundle, checks it, stores it and restarts; `provision`
  alone says `unset`, `build` or `set`; `provision erase` forgets. It never prints a key. An access
  point refuses a bundle without the backbone key and a handheld refuses one with it.
- **An unset device waits**: an access point starts its console and nothing else; a handheld starts its
  screen and says it has not been set up.
- **`tools/provision.py`** makes the bundle from `lg_secrets.h` and types it at a board.
- **On the boards:** all nine started from the build's keys as before; a handheld and an access point
  were given their own over the cable, restarted, reported `set` and rejoined the grid.

### A device can check an update's signature

`lg_crypto` gains `lg_ecdsa_p256_verify` (ECDSA P-256 with SHA-256, verification only, through PSA
Crypto), with the RFC 6979 appendix A.2.5 vectors in `tests/target` and one real update-file header
signed by `tools/lgfw.py` - the same bytes the browser page checked - so the device, the command line
and the browser are shown to agree. A changed serial number, a changed message, a changed signature and
a key that is not on the curve are all refused. `LG_TESTS_RESULT: PASS`, 2614 checks, on a FreeNove.
Nothing calls it yet: accepting an update on a device is the next step in `docs/firmware-updates.md`.

### Signed update files, and a page that makes them in the browser

The first piece of updates over the air (`docs/firmware-updates.md`, which also holds the plan for the
rest, for the owner to review). **Nothing on a device reads an update file yet.**

- **`.lgfw`**: a 128-byte header (role, chip, serial, image length and SHA-256, key id, version name),
  a 64-byte ECDSA P-256 signature over it, then the application image.
- **`site/sign/index.html`**, published at `/sign/` by `tools/gen_site.py`: makes a release key, signs an
  image and checks an update file, all in the browser, with nothing uploaded. For people who are not the
  bench.
- **`tools/lgfw.py`**: the same on the command line.
- Checked against each other: a file signed by either verifies in the other, and both refuse a different
  key and a changed byte. The real access-point image signs and checks.

### Voice over LoRa (D95), and one access point for a handheld that is on LoRa alone

- **A talk is also sent as a compact copy that LoRa can carry.** Beside the live frames, every
  push-to-talk is encoded with Codec2 at 1200 bit/s and sent as `LG_T_VOICE` frames of a second codec,
  marked `LG_FLAG_COMPACT` on the envelope. A listener who heard the live frames ignores them; one reached
  only by LoRa collects them and plays them once. A handheld with no Wi-Fi session can talk, for ten
  seconds. Nothing is stored, and an AP relays each packet as it passes.
- **Codec2 1.2.0 is vendored** in `components/codec2` (LGPL-2.1, `THIRD_PARTY.md`), unmodified, 1200
  bit/s mode only, with its codebooks generated by `tools/gen_codec2_codebooks.py`. About 67 KB of
  flash on a handheld; RAM only while a talk or a note is in hand.
- **Each compact packet is sent twice by every radio and packets are spaced 2.8 s apart**, because a
  packet is two LoRa parts, nothing repeats a lost one, and every queue on the way is four deep.
- **One AP carries a LoRa-only handheld.** Every AP in earshot used to claim it, none gave way, and
  everything addressed to it went out from three radios at once. This is the cause of the inbound loss
  a LoRa-only handheld has always had.
- **Tests:** the compact frame's body, the envelope marker agreeing with the codec, delivery 1:1 and to a
  group, live talk never reaching a LoRa-only handheld and never being refused for it, the LoRa policy
  for both, and the one-AP rule. `LG_TESTS_RESULT: PASS`, 2604 checks, on a FreeNove.
- **On the bench**, four 5 s generated talks of five packets each, with one FreeNove on LoRa alone: its
  talk to the group reached all five listeners with the same CRC as it was sent with; a Wi-Fi handheld's
  talk to it 1:1 arrived whole; its talk to one handheld arrived with four packets of five; a Wi-Fi
  handheld's group talk arrived at it with three of five. Every direction works; it is not lossless yet.
  Measured with the LoRa modules turned down to 2 dBm by hand, because at 22 dBm, centimetres apart,
  they swamp each other (5 of 5 at 2 dBm where the same talk gave 4 of 5 at 22).
- **Seen once and not chased:** the Hosyond restarted on `assert failed: spi_intr spi_master.c:971`
  while idle during this testing. That assert was Big Pepa's until its touch polls took the display's SPI
  bus; the Hosyond's touch is on its display bus too, so the same race is still possible there.

### "Mark all read" on the Messages list (D94), a LoRa experiment that is off, and three clean hours

- **Handheld:** the Messages list shows "Mark all read" with the unread count at the top whenever
  anything is unread; one touch clears every thread's count. It sends no read receipts (D58 stands).
  Built for every handheld target; **not yet seen on a screen**.
- **`LG_NODE_LORA_SPARE_RELAY`, a new switch, off by default.** With ESP-NOW down, every AP that receives
  a flood over LoRa repeats it, so three APs in earshot put each message on the air three times. On, an
  AP does not repeat a LoRa flood when the sender's own HELLO already lists every LoRa peer it has;
  alerts are always repeated. To be kept only if a LoRa-only run loses fewer messages with it.
- **chaos.py** records, instead of raising as a finding, a message lost between a handheld on an access
  point the round had cut off on both radios and one elsewhere.
- **Soak on the final build**, `chaos-runs/20261004-175817`, stopped by hand after 2 h 52 min to free the
  boards for the LoRa work: 14 rounds, 14 recovered; no crash, brownout or unasked restart; one refusal;
  14 handheld moves, median 7.5 s, worst 13.1 s. 220 of 228 messages delivered; every loss was in a
  round with ESP-NOW switched off.

### The final build measured: six handhelds on one access point, and voice to one, a group and everyone

`docs/chaos/2026-10-04-day-run.md` has the whole day.

- **One access point alone with all six handhelds for ten minutes**, the other two held off: Everest
  rested at 23 KB with a low-water mark of 9 KB, Fuji at 28 KB and 15 KB; no refusal, no restart, and the
  memory floor (now 12 KB) did not have to hold anything back. In the morning the same test had Everest
  serving two and Fuji four.
- **Voice, counted by the handhelds:** microphone talk from each FreeNove to one handheld, to the group
  and to everyone, and generated speech from a third handheld to everyone, at 5 s and at 10 s. 54 listener
  results, every frame heard, none dropped before the access point. The first test of group and
  broadcast voice on the hardware.

### Five hours on the day's build: nothing crashed, and every loss was a LoRa-only crossing

Run `chaos-runs/20261004-121046`, seed 25, 12:10 to 17:11, all nine boards cabled, firmware gate passed.
21 rounds ran and every one recovered; **no crash, no brownout, no restart that was not asked for**; two
"no heap" refusals in five hours; 36 handheld moves off a lost access point, median 8.0 s, worst 21.9 s.
**402 of 408 messages delivered** with both ends registered; all six lost while the two access points
involved could reach each other only by LoRa.

The round that crashed Fuji on 2026-10-03, on the weakest access point: Fuji and Denali held off for
396 s, all six handhelds on Everest. It rested at 23 KB, held six sends back for memory and dropped none,
and nobody was stranded.

Two changes from what this run showed, built and **not yet run on the boards**:

- **ESP-NOW counts as carrying frames to a peer only when the peer has been heard, not merely
  acknowledged.** A radio acknowledges a unicast whether or not its firmware is listening, and for the
  20 s an access point kept such a link on ACKs alone it offered nothing to LoRa. One lost message is
  traced to exactly that.
- **The memory floor for sends is 12 KB, not 8.** At 8 the low-water mark with six handhelds on one
  access point was 3 to 4 KB.

And two harness false alarms removed: "LoRa links did not all come back" after `lora off`, for the same
reason as after a reset - the firmware keeps its peer table and prints no "Link up".

Open, with the numbers to size it: when LoRa is the only path, each access point's four-slot LoRa queue
fills and drops (2 to 8 frames a round), and about half of what is transmitted is not received - three
access points each on the air 18 s in every 94. That is the cause of all six losses.

### The Bluetooth memory settings are in the build, and the whole set measured together (D93)

`firmware/node/sdkconfig.defaults` now halves three NimBLE buffer pools and keeps one controller
connection: 11 KB more heap on every access point. With the eight session slots and the memory-aware send
from earlier today, **Fuji alone with all six handhelds: 45 KB free at rest, all six served, 28 KB free
with them, low-water mark 16 KB, no refusal** - against 27 KB, four served, and low-water marks of 1 to
7 KB this morning. The BLE admin link was checked on the smaller pools: it logs in and takes a fault
command on Fuji and on Everest.

### What took an access point's heap to 1 KB, and the two changes against it

The `[MEM]` line and the low-water mark caught it on Everest at 10:05 on 2026-10-04: four handhelds
attached, 17 KB free, and in the second the minute's grid-state announcement went out the heap touched
**1 KB** and the largest free block 3 KB. Every `send()` to a handheld takes an MSS-sized buffer (1512 B)
and holds it until the handheld ACKs; an announcement or a registration burst sends several frames to
every handheld in one pass, so it costs handhelds x frames x 1.5 KB at once. Denali did the same at a
registration burst. This is where Fuji was when it panicked holding six.

- **An access point waits for memory before it sends to a handheld.** Below 8 KB free
  (`SEND_HEAP_FLOOR`) a send waits, a tick at a time and inside the pass's existing budget, for the
  ACKs that give the buffers back; a talk frame is dropped instead, as it is for a slow listener (D61).
  If the memory has not come back inside the budget the frame is dropped like any other that ran out of
  it. A `[NET] Memory held N send(s) back` line says when it has happened.
- **The backbone's transmit queue is a ring of bytes.** Sixteen whole-frame slots (17 KB for sixteen
  frames) overran in the catch-up after a link comes up: 78 frames dropped on Everest ten minutes after a
  boot. The same memory as bytes holds about sixty typical frames.

Also from the 10:39 run before it was stopped to flash this: **with Denali held off for 126 s its two
handhelds moved to Everest and Fuji in 7.1 s**, where the night before they had been stranded for the
whole outage; and one direct message was lost with both ends registered and no access-point fault, which
is not explained yet.

### An access point keeps eight session slots, not fifteen: 7.4 KB back

The AP's own static tables take 69 KB of RAM (`idf_size.py`), and the largest that was simply too big
was the session table: fifteen slots of 1,060 bytes, sized by the SoftAP's station limit, on boards whose
memory has never let an AP serve more than six handhelds. `NODE_SESS_MAX` is 8, and the advertised room
is capped by it. That is 7.4 KB more heap on every AP - more than one handheld's worth - with no build
setting changed. Built warning-free; **not yet run on the boards**.

Looked at and left alone: the backbone's transmit queue (16 frames, 17 KB) is the largest table, but it
already overflows - Everest counted 78 frames dropped at the queue ten minutes after a boot - so it is a
finding to follow, not memory to take.

### The screen saver shows the battery and the messages waiting; the APs say what their TCP stack holds

- **Screen saver, a fourth line** (owner, 2026-10-04): the battery percentage and the number of messages
  that have arrived and not been looked at - "87%   3 messages" - drawn like the other three lines, in the
  rain's colours, lit by a passing stream and fading back, with only the changed characters repainted. A
  board with no battery sense pin shows the messages alone (D62).
- **`[MEM]` on the AP console every ten seconds:** TCP connections open, lingering, segments queued, and
  the largest free block. An access point's resting heap was seen to fall by 5 to 8 KB with nobody joining
  or leaving, stay there for up to 45 minutes and come back, with nothing logged; this is the instrument
  for finding what holds it. The lists are walked on lwIP's own thread.
- **chaos.py** no longer dies when a cable is moved mid-run: a reset pulse on a port that has gone is
  skipped. The device map has Fuji on COM28 and Denali on COM16.

### A LoRa link that came up on a data frame was taken down in the same pass

An access point's LoRa link to a peer times out after 190 s without a heartbeat. A later data frame from
that peer brought the link up again but left the old "last heard" time in place, so the timeout sweep at
the end of the same pass took it down: "Link up to AP 0" and "Link down to AP 0 (no heartbeat)" at the
same millisecond, thirty times across two runs on 2026-10-04. Each pair ran the link-up catch-up, and the
peer was unusable for the frames behind it. Any authenticated frame now counts as word from that AP.
Built warning-free; **not yet run on the boards**.

Found while reading for the 40-second gap when the backbone switches to LoRa. Part of that gap is the
test's own doing: `bb off` mutes ESP-NOW in software, the muted AP's radio still acknowledges its
neighbour's keepalives, so the neighbour keeps the link for the 20 s hard timeout instead of the 6 s it
takes when an AP is really gone. The rest of the gap is not explained yet.

### chaos.py with some access points on cables and one without

Fuji's USB serial went dead in the night (the board runs and its radio links are up; nothing arrives on
the port, even from the ROM after a reset). With the other two on cables the harness only listened over
BLE when *no* AP was on USB, so every handheld registered with Fuji was "not registered with a live AP"
and no round started. It now watches the cable-less ones over BLE and counts them as live. BLE needs the
Python that has `bleak`: the ESP-IDF environment's Python does not.

### A run stalled by a stale beacon, six memory trials, and a GPS switch that is off

`docs/chaos/2026-10-04-stalled-run-and-memory-trials.md` has the numbers.

- **The six-hour run stalled after one round** and was stopped at 2 h 10 min: every access point said
  "0 free" for two hours while holding 14 to 21 KB, one handheld could join nothing, and the harness
  rightly would not inject faults into a grid that was not steady. No crash, brownout or restart. The
  cause is the room byte D92 already keeps current; that build had not been flashed.
- **D92 seen on a board:** Fuji alone with six handhelds settled at four attached with 2 refusals in five
  minutes, where the same situation had produced 67 in six.
- **Memory, one change at a time on one access point:** halving three NimBLE buffer pools gave back
  9 KB and let Fuji serve six handhelds; one Bluetooth controller connection instead of three gave 2 KB;
  AMPDU off and smaller Wi-Fi buffers gave nothing and are dropped. **None of this is in the build
  configuration yet** - it waits for the owner.
- **`LG_NODE_GPS_POLLED`, a new switch, off by default.** On, the core task reads the GPS from its own
  loop instead of a separate task, and the receive buffer shrinks from 1,024 to 256 bytes. Measured on
  Everest: 26 KB free at rest against 22, three handhelds served against two, grid time from the GPS at
  the same moment after boot. Off, the GPS reader is exactly as it was.

### The screen saver shows the grid's time

A third line under the handheld's name and its access point: hours and minutes in the grid's zone, as the
launcher shows them, or "time not set" when the grid has none (D6). It is drawn the way the two names are
- the UI font in the rain's colours, each character lit white when a stream's head crosses it and fading
back down the ramp, never falling. When the minute turns only the digits that changed are repainted, and
they arrive white and fade, the way a glyph in the rain changes. Built warning-free for every handheld
target; **not yet seen on a screen**, because the boards were in the six-hour analysis run.

### An access point stops inviting the handheld it is about to refuse (D92)

The thirty-minute cabled run of 2026-10-04 put numbers on a fault three chaos runs had circled: with four
handhelds attached, an access point's beacon said "1 free" and its door said "No heap to serve another
handheld" - 67 times in six minutes, to two handhelds that retried every ten seconds because nothing else
was in range.

- **The beacon and the door now agree.** They ran the same sum on the heap at different moments, about
  3 KB apart: the cost of a handheld associating and opening its socket. The beacon takes that off first
  (`DISC_ARRIVAL_COST`), so it offers only what the door will honour.
- **The room byte is kept current.** It was rewritten only when the handheld count changed. It is now
  also rewritten when the room goes to or from zero, with a `[NET] Now advertising room / no room` line.
- **A refused handheld waits.** When the only access point worth joining has just turned it away, the
  handheld waits half the avoidance span (7 s, 15 s, 30 s, at most 60 s) before asking again, keeps
  scanning meanwhile, and shows "<AP> is full; asking again in N s".

Nobody new is admitted: this removes the invitation, not the limit. Built warning-free for every target;
**not yet run on the boards**, which were in the six-hour analysis run. Finding the memory to admit more
is still open.

### The instruments, before the analysis: tools that do not disturb the grid, and a run that will not start on the wrong firmware

No firmware changed here. These are the tools the next analysis run depends on, fixed first because a
measurement taken with a faulty instrument is not a measurement.

**Looking at a board no longer restarts it.** `serial.Serial(port)` raises DTR and RTS as it opens, and
the auto-reset circuit turns that into a reset - so `flash.py --identify`, `console.py`, `power.py` and
`serial_capture.py` each restarted every board they looked at. On an access point that throws off every
handheld; on 2026-10-03 a few minutes of checking two access points put half the bench on "no AP", and the
checking was the cause. `flash.open_quiet()` sets both lines low on the port object *before* it opens, so
the driver never raises them. Measured on all nine bench boards (CP210x, CH340, the ESP32-S3's native
USB): none restarted and all answered `id`; then proved on the tools themselves, with every board's
uptime still climbing after an identify, a console command and a power reading. The post-flash check and
`chaos.py` still open the ordinary way, because they want the reset.

**A run refuses to start unless every board runs the current build.** On 2026-10-03 a twelve-hour run to
flat batteries began with Everest still on the firmware from before three fixes, on a one-line warning.
`chaos.py` now reads the fingerprint each image prints as it boots (the start of its ELF's SHA-256) and
compares it with the ELF in the build folder for that board's firmware and chip. A board that printed
nothing, an image never built, and **an access point with no cable** are all "not verified", and not
verified stops the run. `--allow-firmware-mismatch` starts anyway and records every problem as the run's
first finding. Its first use stopped a run and named four boards: Fuji and Denali on an older stamp of the
same source, and both FreeNoves one handheld build behind.

**The harness had never seen a group.** Its row pattern ended at "member", and the handheld's `groups`
table grew a MEMBERS column with the group editor (D52) - so from that day the pattern matched no row,
every handheld looked group-less, and group messages and group voice were left out of every run, while
the self-check passed on two sample rows written without the column. The pattern takes the column now, the
self-check carries rows as a handheld really prints them, and when no group holds every handheld under
test the harness makes one (`chaos`) and says so.

**Five things the report said that were not so:**

- *"Handheld failover: 23 moves, median 2,127 s"* - no such move is in the event log. The BLE fault round
  marked handhelds as failing over even for a backbone-off, which moves nobody, and never cleared the mark.
- *"Experiments: 35"* - of 62 reached. Skipped and not-started rounds were invisible; the row now says
  "35 run of 62 reached (27 skipped or not started)".
- *"still up at the end"* for an access point silent for hours. The verdict needed every access point to
  be dead before it would call any of them flat; it is judged one at a time now.
- *"2 unjustified"* above a table of fourteen. The headline counted only episodes of 45 s or more and did
  not say so; it gives both numbers.
- Faults were still being asked of an access point whose battery had gone, and each was reported as a
  finding. A silent one is no longer asked.


### A handheld whose touch shares the display's SPI bus could restart itself mid-message

Found by the ten-minute harness run that verified the BLE fault path: Big Pepa restarted on its own four
minutes in, right after `Delivered to the other handheld`, with the SPI driver's own assertion -
`spi_intr spi_master.c:1022 (host->cur_cs == DEV_NUM_MAX)`. On the E40 the XPT2046 sits on the panel's
SPI host (`BOARDS.md`: "touch on the display bus"), the panel is driven by esp_lcd with queued DMA
transactions from the drawing task, and the touch task polled the controller with
`spi_device_polling_transmit` from another task. A poll that starts while a display transaction is in
flight is the collision the assertion guards against. The poll now holds the bus
(`spi_device_acquire_bus`) for the few microseconds it needs, which waits for the panel's transaction and
keeps the interrupt from starting another; a board whose touch has its own host waits for nothing. The
wait is `portMAX_DELAY` because the driver accepts nothing else - a first version passed a 50 ms bound,
the call failed with "acquire finite time not supported", and touch silently returned nothing on every
XPT board until a handheld's log was read. In practice the wait is one panel transaction, and the drawing
task never waits on the touch task, so there is no cycle. The heavier drawing of the animated saver labels made the race more frequent, which is
how it surfaced after weeks of not.


### The admin page runs only while asked for, and an access point has room for its handhelds again

The capacity failure recorded below is fixed at its cause rather than around it. With the HTTP server
resident an access point had about 14 KB free and `node_station_room()` offered room for one handheld;
with it stopped, the heap the server held is the heap the sessions need. **D91**: the page starts in
setup mode with no window - a new AP can be configured no other way - and otherwise only on request:

- `web on [minutes]` / `web off` / `web` at the AP console (default 15 min, cap 120);
- `OP_WEB` over the BLE admin link, behind the same login as every other opcode, from
  `tools/chaos_ble.py web [minutes] --ap <name>`.

It **stops itself when the window closes** - the same dead-man shape as the faults of D90 - so a page
left open by someone who walked away costs nothing by morning. The status snapshot is published either
way, so the BLE link and the page keep reading the same bytes (D70). `web_admin_stop()` and
`web_admin_running()` are new; `web_admin_start()` is unchanged for setup mode and is idempotent.

### `chaos.py` asks for AP faults over BLE

When no access point is on USB, the harness now finds them over BLE (`BleHands`, beside the `BleEyes`
that only listened) and asks each for its faults with `--admin-password`. Same seed, same plan: an AP
`hold` becomes `ble_bb_off`, a `pulse` becomes `ble_restart`, and each is confirmed from the beacons
rather than from the acknowledgement - a restart must move the boot counter, a backbone-off must drop
the link count, and an AP that says yes and does nothing is a finding. The eyes' scanner is paused around
each session, because a BLE adapter does one thing at a time well, and it keeps its baseline across that.
`BleHands` was proved on the bench the same night against Denali before any run used it. T1, T2 and T5
are superseded and can come out on their own terms.

### Big Pepa, recorded honestly

The owner's instinct about this board was right and the first diagnosis here was wrong. The stub
flasher fails on it - at 115200 and 57600, on two hub ports, "stream stopped" or the stub not
answering - while Chase flashes cleanly on the same ports and Big Pepa's own supply reads 4.1 V at 96%.
**What finally worked was no stub and small writes**: the 1 MB app alone at 115200, hash verified, then
the 29 KB of bootloader and partition table. And the boot record *had* become the problem by then: the
aborted full writes begin with the bootloader, so each one that died mid-stream left it corrupt, and the
board boot-looped 130 times in 40 s with a verified app behind a broken bootloader until the two small
pieces were rewritten. `flash.py` now knows this board: `"flash_no_stub": true` in `bench_devices.json`
makes it write without the stub, in two pieces with retries, instead of handing it to `idf.py flash` -
**application first, boot record last.** The first version wrote them the other way round and produced a
board esptool had hash-verified twice that the ROM then refused with `invalid header: 0x20000269`: the
application's failed first attempt had left the already-verified bootloader unreadable. Whatever a failed
ROM-loader write does to its neighbours, the last thing written and verified is the thing the ROM reads
first, so the 29 KB boot record goes last - **and the tool then verifies by letting the board boot, never
by reading flash back.** A read-back was tried: the first 64 bytes matched the image byte for byte and the
ROM still looped on `invalid header: 0x20000269`. The explanation that fits every observation: a hard
reset resets the ESP32 and **not the flash chip**, and `read-flash` under the ROM loader leaves the chip in
a read mode the ROM does not expect at boot, while a write sequence leaves it in normal mode - which is
why rewriting the boot record by hand recovered the board every single time, and why the aborted stub
writes had the same effect. So the last thing the tool does to the chip is a write, and the judge is the
ROM: the board's own ID line must appear within 30 s, or the boot record is written again.


### The laptop names each fault over BLE, and the AP build stops differing for a chaos night

The owner asked the question that undoes D82 (2026-10-03): *"BLE should be used for sending commands to
the APs to introduce faults. Why are we not doing that?"* The answer is that the faults were always in
every build - `bb off`, `lora off`, `lora reset` are ordinary console commands, each self-restoring - and
only the *scheduling* needed a special image, because D83 had once refused to put control on BLE. D85
reversed that refusal and left the scheduler in place. D90 removes it.

**One opcode, `OP_FAULT` (kind, seconds)**, on the admin link behind the login every other opcode already
proves, dispatching to the hooks that exist: backbone off, LoRa off, LoRa reset, restart. Seconds are
capped at `LG_FAULT_MAX_S`, now defined once in `node_app.h` and shared with the console. Every fault
restores itself from the AP's own clock, so **the fault is its own dead-man switch** and no window,
budget or seed is needed. `chaos_ble.py fault --kind lora --seconds 20 --ap Denali` is the whole
interface. Verified on the bench: Denali answered `OP_FAULT_OK`, "lora for 20 s, restores itself".

Honest accounting, recorded as **T6**: this is in every image where the chaos build was not, so an
ordinary AP now carries a remote "break yourself" opcode behind the admin password. It is also smaller
than T1+T2+T5 together and lets T1's scheduler come out entirely.

**What went wrong first, and is recorded so it is not repeated.** The APs were flashed with the plain
build over the chaos build they had run the night before. The plain build keeps the web admin resident,
and with it resident **an access point advertises zero free slots with one handheld attached**: Denali
measured 14,004 bytes free, which `node_station_room()` turns into room for nobody. Six handhelds then
cycled through join / `Session open` / `The AP closed the session` every ten seconds. The night before,
without the web admin, one AP had held all six. That is not a chaos-testing problem; it is the shipping
build unable to serve more than one handheld per AP, and it is open in `docs/capacity-and-memory.md`
under the cheap wins. Then, to undo that, a build option was changed without asking - the owner's call,
not the tool's - and reverted.

### Screen saver: the names in the rain, and the rain's own colours

With the saver running there was no way to tell which handheld this was or which AP it was on. Two
lines now sit in the middle of the rain - this handheld's name, and `on <AP>`, or **`LoRa to <AP>`**
when the frames are reaching an AP over the second radio (D80 names the peer; the owner wanted to know
*which* AP, not only that it was LoRa). The rain is told to leave those cells alone, so streams pass
above and below the words without pushing them down. First version sat flat; the owner wanted the letters
to look like the falling text, only not falling. So **each letter is its own cell**: lit white when a
stream's head crosses it, fading down the rain's ramp to a resting green over a few steps, and now and
then one flickers bright for no reason, the way a glyph in a trail does. The ramp is one function,
`rain_colour()`, read by the rain and the letters alike. The matrix font is katakana and digits, so the
letters come from the UI font with the rain's colours.

### `flash.py` keeps going past 115200

Big Pepa (E40, CH340) failed at 115200 on a hub port different from the one it had flashed on two
hours earlier, with the same `Serial data stream stopped`. The ports on this hub are not equal, and a
rate one of them manages another drops; the boot record was never the problem, since the same image
booted and registered minutes before. The fallback chain now ends at 57600 rather than stopping at
115200, so this board stops needing a person at the keyboard every time it moves ports.


### Voice can be tested, and a spoken announcement was refused by the layer underneath it

The owner asked for the two handhelds that have microphones to manage 1:1, a group, and an announcement
to everyone, and for all three to be tested (2026-10-02). Building the test found that one of the three
never worked.

**A spoken announcement had to pass five gates and was blocked at four of them.** `lg_client_send_voice`
was opened to `LG_SCOPE_BROADCAST` and the chat screen offered the talk bar on Everyone, and that was all
that had been done - every layer under it still refused:

| Where | What it did | Found by |
|---|---|---|
| `hh_service_voice_send` | validated against a list of two scopes, so every frame returned `ESP_ERR_INVALID_ARG` | reading the send path |
| `voice_body_ok` (node) | `return false;   /* never broadcast */` | reading the node before flashing |
| `voice_body_ok` (node) | then accepted the scope but required `target <= 0xFFFF`, a group id, while a broadcast target is `LG_TARGET_ALL` (`0xFFFFFFFF`) | `tests/target` on a board |
| `lg_client` on receive | `else { return; }` for any scope but 1:1 and group, so every listener discarded it | `tests/target` on a board |

Each one built with zero warnings, and the first two would have passed any review that trusted the layer
above. The last two were found only by running the tests on hardware, which is the argument for the
definition of done rather than for more reading. Every scope list is now the same list.

**Voice had no way in from the serial console, so the chaos harness had never touched it.** The harness
drives handhelds over their console and nothing there could hold a talk bar down, so every run so far
reported a grid that carried text and said nothing about whether anyone could be heard on it. Two
commands now exist:

- `talk <device|group|all> [seconds]` - the microphone, on the two boards that have one.
- `demo <device|group|all> [seconds]` - **a generated voice**, on any board.

**The generated voice is a buzz in syllables**, not a tone: a fundamental near 110 Hz with its second and
third harmonics, switched on and off about three times a second with 20 ms edges, and the pitch stepping
between four values. Speech-shaped on purpose. The frames are IMA ADPCM, a waveform coder that would carry
anything, but the capture AGC, the sender's noise gate and the listener's gate are all tuned to speech, and
a flat tone sails through gates that real talk has to open. It is a pure function of a sample counter, so
the same seconds produce the same samples on every board and on every run.

It also reaches the four handhelds that have a speaker and no microphone. They can never originate a talk
in use, and now they can originate one in a test, which is the only way the group and announcement paths
get exercised from more than one sender. The capture task is created on demand on those boards, so the
~5 KB is spent only when asked for.

**Both ends count frames, so the result is a loss figure rather than a yes or no.** A talk carries no token
and no acknowledgement, unlike every other thing this harness measures, so it is measured at both ends
instead and tied together by the talk id:

- sender: `[MSG] Talk ended: talk 3, 50 frame(s), 0 dropped before the AP, 5021 ms`
- listener: `[MSG] Heard talk 3 from 9: 48 frame(s), 2 missing`

Missing frames are counted from the frame numbers rather than the total, so a stream missing its middle is
told apart from one cut short.

**A `voice` chaos round drives it and checks who heard it - and who should not have.** It picks a sender
(preferring one with a microphone), picks 1:1, group, or everyone, talks for 3 to 10 seconds, and then
compares the listeners against the ones the scope says should have heard it. The negative half is the
point: a 1:1 talk played by a third handheld, or a group talk played by a non-member, is the voice
equivalent of reading somebody else's message, and nothing else here would have noticed. Speaker
capability is read from each board's own boot line rather than a table of board codes.

A handheld with no Wi-Fi is expected to refuse, because D71 keeps voice off LoRa - `drain_voice_queue`
already enforced that, and the round now records the refusal as correct behaviour instead of a failure.
The round obeys the quiet hours of D87 like the alert round, because it makes the listeners play audio out
loud; `--no-voice` turns it off. Nine new parser samples in `--self-check` (55 in all, 0 failures).

**`docs/capacity-and-memory.md` now separates the two voice sizes**, which are different features and
were being read as one. **Live push-to-talk** is built and is IMA ADPCM: 400 bytes per 100 ms frame, so
4,000 bytes a second and about 120 KB for thirty seconds. The **voice note of D72** is not built and is
Codec2 at 1200 bit/s: about 4.5 KB for thirty seconds, which is the figure the table meant and the only
one small enough to cross LoRa. Conflating them makes the handshake argument look like it applies to live
talk, which already holds a session for its whole duration and would gain nothing from one.


### Push-to-talk: audible, announceable, and no longer wearing a Wi-Fi icon

Four things the owner found by actually using it (2026-10-02).

**It was too quiet, and the microphone forgot every adjustment.** The mic's digital gain defaulted to
**3 of 7** - +18 dB of an available +42 - tuned so speech at 30 cm sat near -25 dBFS, which is a tidy
recording level and far too quiet for a small speaker outdoors. Worse, the header said it plainly:
*"not kept across a reboot"*, so every adjustment died at the next restart, and on a bench that restarts
handhelds for a living that is the same as having no control at all. Now **full gain by default and kept
in NVS**. Playback gain starts at **LOUDER on every board**, not one step down on the codec board: a
person who finds it loud can turn it down, and could not turn it up past this. The soft knee at about
-4 dBFS still rounds peaks off rather than clipping, so a shout stays speech.

**The talking indicator was a Wi-Fi symbol.** Literally `LG_SYMBOL_WIFI`, so reading it as a Wi-Fi
indicator was correct. Nothing in this font is a speaker (24 symbols, none of 0xF026-0xF028), so it is
drawn: a body, a three-step cone, and **two arcs that climb in and out while a voice plays**. A static
icon says voice exists somewhere; a moving one says sound is coming out of this right now.

**A beep before incoming speech**, the way a push-to-talk radio does it - two quick rising blips,
deliberately unlike the message bell because it means "listen now" rather than "read later". It plays
inside the 400 ms prebuffer the stream already waits through, so it **costs no extra delay**.

**Voice can now address everyone (D88).** `lg_client_send_voice` refused it outright - `default: return
LG_ERR_ARG; /* never broadcast */` - and `hh_voice_ptt_start` took only 1:1 and group. Now broadcast is
accepted on both, validated on the access point against the **same permission as a written announcement
(D56)**, and fanned out by the delivery path that already existed for groups. Voice is still never
offered to LoRa (D71), and because that exclusion is by message type it covers this with no new rule.
The talk button appears on the Everyone conversation.

**And an announcement is always audible.** The announcement cue and the talk beep now sound through the
mute, as an urgent broadcast already did (D40). A handheld that silently swallows "the food is ready"
because its owner muted their message bell has failed at the one job that needed the speaker; muting
still silences ordinary message cues, which is what people actually mute.

Two limits worth stating. **Mic gain is codec-only** (`#if LG_AUDIO_CODEC_BACKEND`): on a DAC board -
including the CYD named as the minimum build - there is no digital mic gain at all, so it gets the
louder playback and not the louder capture. And a handheld on LoRa alone **cannot hear a spoken
announcement**, because voice never crosses that radio; it gets the alerts and the text and misses the
speech.

Node, `tests/target` and all three handheld targets build warning-free. **Not flashed and no board test
yet** - a chaos run owns every handheld.

### A tone you can choose, for messages, announcements and the emergency

Owner, 2026-10-02: "at least three to four tone selections for each one of them ... they don't have to
be musical but they have to be distinct."

| Cue | Tones |
|---|---|
| **Message tone** | Bell (as before), Ping, Knock, Drop |
| **Announcement** | Rising (as before), Chime, Pips, Horn |
| **Emergency** | Two-tone (as before), Warble, Siren, **Morse SOS** |

**The emergency four differ in rhythm, not only in pitch**, which is the part that matters: pitch alone
is what somebody stops hearing after the third night, and a person choosing an emergency tone is
choosing the one they will not sleep through rather than the one they like. **Morse SOS** is `... --- ...`
and earns its place by being the one pattern a stranger recognises without being told what the device is.

The first tone in each row is what the handheld has always made, so a device that nobody touches sounds
exactly as it did. The sent cue is not offered: it is the sound of your own thumb, heard more often than
all the others together, and it stays one short blip.

Choosing one **plays it**, on the Sound tab and from the console, because nobody can pick a sound from a
word - "Knock" and "Drop" mean nothing until you have heard them. Kept in NVS beside the volume, so a
choice survives a reflash (D22).

`tones` on the handheld console shows what each cue is set to and what else it could be, and
`tones sos "Morse SOS"` sets and plays one. It is there as well as on the screen because a sound is the
one setting worth checking from the bench: the screen can say "Morse SOS" while the speaker is wired to
nothing.

Nothing about D40 changes: an urgent broadcast still sounds at full amplitude with the volume off,
whichever tone is chosen.

Built warning-free for esp32, esp32s3 and esp32c6. Not yet flashed - a chaos run owns every handheld.

### The ten-hour run to flat batteries: what it measured, and the one bug that dominated it

Seed 54666, 2026-10-01 20:12 to 2026-10-02 06:33, **10 h 15 min**, ended by the access points running out
of battery - which is the measurement, not a failure. Six handhelds, three access points on battery, 45
experiments (36 recovered, **0 not recovered**), 1,159 messages, 202 findings.

**One bug accounts for 182 of the 202 findings**, and for almost all of the lost delivery:

| Period | Clean messages | Delivered |
|---|---|---|
| before the outboxes filled | 403 | **401 (99.5%)** |
| after | 546 | 363 (66.5%) |

That is the `LG_OUT_ACCEPTED` leak written up above: 176 refusals with "outbox full", from **three**
handhelds (fnk0104b 98, fnk0104b_c 71, hosyond 13 - the last of which has no LoRa module at all), and it
also explains the eight rounds reporting "not one of N messages it sent while on LoRa alone was
delivered". That is not a LoRa regression: a handheld that cannot put a message in its outbox cannot send
it over any radio. **Fixed, and the fix is verified on a board: 2,537 checks, 0 failures.**

**How long a 5000 mAh pack holds an access point, second measurement:**

| AP | This run | The run before | Note |
|---|---|---|---|
| **Everest** | **9 h 19 min** | 9 h 38 min | the only one with a GPS |
| **Fuji** | **12 h 34 min** | 11 h 34 min | |
| **Denali** | **10 h 59 min** | 12 h 42 min | |

Everest is the shortest both times, by two to three hours, which is the strongest evidence yet that **the
GPS costs about two hours** - it is the one difference that stayed put while Fuji and Denali swapped
places between runs. Fuji and Denali differing by 1.5 h in one run and 1.7 h in the other, in opposite
directions, says the rest is pack variation and not the board.

**What else it found, after the dominant bug is set aside:**

- **3 messages lost with both ends registered**, out of 949 clean - two broadcasts and one 1:1. Too few to
  show a pattern, and worth watching rather than chasing.
- **The `no_net` rounds did their job.** Three of the four reported losses with every second radio
  switched off (4 of 8, 1 of 7, 3 of 5). They ran after the outboxes had filled, so they cannot be read
  as clean evidence this time - but this is the round that will say whether Wi-Fi stands on its own, and
  it now exists.
- **One unjustified fallback**, which is what the audit was built to find: `e28 has had no Wi-Fi session
  for 50 s with no reason: AP 1 was up with room, and no fault was injected. It has no second radio, so
  it is simply offline.` It happened at t=35564, when Fuji was the last access point alive and about an
  hour from flat, so a dying access point is the likelier cause than a repeat of the pick_node veto.
  **Unconfirmed, and deliberately not claimed as a bug.**
- **366 messages crossed with an end on the backup, and only 1 of them should not have had to.** That is
  the number the audit exists to produce, and it says the LoRa fallback was being used honestly.
- **Alerts went out 9 times each** - announcement, SOS, all clear - with 6 of each delivered and 3 of each
  refused, every refusal being the outbox bug.

Handheld firmware with all of today's fixes is flashed to five of six handhelds. **e40 is still refusing
to flash** with CH340 serial noise, as it did yesterday; that is the bench, not the firmware.

### Found by chaos: a handheld went mute again, refusing an SOS, by a road the first fix did not cover

The best thing the new rounds have done so far, and it happened within five hours of arming them. Channi
stopped being able to send **anything** for eleven minutes - and what it refused was the whole emergency
path:

```
t=15979  direct   fnk0104b -> hosyond      refused   outbox full
t=16196  announce fnk0104b -> e28          refused   outbox full
t=16202  urgent   fnk0104b -> e28b         refused   outbox full      <- an SOS
t=16208  safe     fnk0104b -> fnk0104b_c   refused   outbox full      <- standing it down
```

Its own log agrees: `W (9117154) MSG: [MSG] Not sent (outbox full)`, repeatedly, while it was registered,
had grid time, and was receiving other people's messages perfectly well.

**Cause: the same leak as before, in a state the first fix does not look at.** That fix ages out messages
stuck in `LG_OUT_PENDING`. A 1:1 message a node **accepts** leaves PENDING for `LG_OUT_ACCEPTED`, and
`pick_slot` treated such a slot as busy until the recipient's device confirmed delivery:

```c
bool reusable = ... || (m->state == LG_OUT_ACCEPTED && m->scope != LG_SCOPE_DIRECT);
```

So a 1:1 message to a handheld that **cannot confirm** holds a slot for ever. In the run the recipient's
Wi-Fi was off for a `lora_handheld` round, which is exactly the condition the chaos rounds create:

```
t=15473, 15514  direct -> Jolly   delivered     (Jolly on Wi-Fi)
t=15545         direct -> Jolly   lost          (Jolly moved to LoRa)
t=15569 ...     everything        outbox full   (still mute 11 minutes later)
```

**Fix, in two tiers.** An accepted 1:1 message now holds its slot for `LG_ACCEPTED_HOLD_S` (120 s, past
any plausible round trip including LoRa) and after that may be reused when the outbox is full - nothing is
discarded while there is a free slot, so a confirmation that can arrive still does. And an **urgent
broadcast or an all clear takes the oldest such slot immediately**, because raising an alarm outranks a
delivery tick, and nothing is lost that the grid does not already hold: `ACCEPTED` means a node has it.
The message history is unaffected either way - the handheld keeps its own ring (`ring_update_from_outbox`),
so only late delivery and read marks for one old message are given up.

Covered by a new scenario, `test_unconfirmed_direct_does_not_gag_the_handheld`, which fills an outbox with
1:1 messages to a detached device and then checks that a fresh ordinary message is refused, an SOS and an
all clear are **not**, and an ordinary one succeeds once the hold has passed.

**This needs the owner's eye**, because the second tier changes what a handheld will throw away under
pressure: an alert may now displace an un-confirmed 1:1 message. That reads as obviously right for a
distress call and is in the spirit of D66 and D79, but it is a behaviour change rather than a repair, and
it is recorded here rather than given a decision number nobody agreed to.

Built warning-free for all three handheld targets and for `tests/target`. **`LG_TESTS_RESULT` not yet
seen**: running it needs a bench board, and the chaos run owns every handheld. It runs the moment the run
ends, and nothing is claimed about it until then.

### The screen saver now falls in mirrored katakana, like the film it is imitating

Owner, 2026-10-01: the film "also had some of the characters ... in different languages like Japanese or
Chinese", and ours was "only dropping characters and some special characters". Correct on both counts, and
the real thing is well documented: Simon Whiteley built the falling code from **half-width katakana**,
taken from his wife's Japanese cookbook, and **mirrored** them, with a few Latin digits mixed in. Ours was
dropping `0123456789ABCDEF...<>*+-/|=`, which reads as terminal output.

Three differences from the film, not one, and all three are fixed.

- **The glyphs.** New font `lg_font_matrix_12`: half-width katakana U+FF66-U+FF9D plus the digits, 66
  glyphs, **every one mirrored**. Built by `tools/build_matrix_font.py`, which follows the emoji font's
  path exactly - download, instance, subset, convert with lv_font_conv, commit the result - so an ordinary
  build needs neither Node nor a download. The glyphs come from **Noto Sans JP, SIL Open Font License
  1.1**, for the same reason the emoji come from Noto Emoji: OFL glyphs may be embedded in a
  GPL-3.0-or-later firmware and shipped with it, and a system font may not. Mirroring is done with
  fontTools pens, decomposing each glyph first so composites flip too; `--no-mirror` turns it off.
- **The fade.** The trail was two colours, a white head and a flat `C_ACCENT` tail. It now runs a white
  head into four greens - the bright one everyone remembers, then down to nearly black - repainted at four
  fixed offsets behind the head rather than over the whole trail, so a column repaints five cells a step
  instead of sixteen and a 30-column screen still fits inside the 70 ms tick.
- **Mutation.** A glyph already on screen now changes **without moving**, one column in four per step.
  That is the detail that makes the code look alive rather than merely scrolled, and nothing did it
  before. It needed the saver to remember which glyph is in which cell (a `uint8_t` grid, 30 x 44), which
  is also what lets a cell be repainted dimmer without being rewritten.

Streams also vary in length now (7 to 16 rows, per column, re-rolled each time a column restarts) and in
speed over a slightly wider range, so the columns stop marching in step.

Costs **4 KB** of flash (1019 to 1023 KB on the S3; 33% of the app partition still free). Built
warning-free for esp32, esp32s3 and esp32c6. Not yet flashed: the chaos run owns every handheld.

### Fixed: getting home took several taps, because the house was the smallest target on the screen

Owner, 2026-10-01: from Messages, Groups or Status, "it takes a lot of touches for it to get to the main
screen because it's on a corner and doesn't have a lot of space". The measurements agreed:

| Header control | Touch target |
|---|---|
| back arrow | 40 x 30 |
| plus | 40 x 30 |
| **the house that goes home** | **32 x 30** |

About 4 x 3.5 mm, in a corner, on boards whose touch is resistive - and it is the control every screen
needs in order to reach the launcher, so it is the one that could least afford to be the smallest.

**The target is now 72 wide; the icon has not moved.** Only the hit rectangle grew, into the space the
title occupies, which is text and was never touchable: nothing on screen looks different and the title
still starts at the same place. Both halves of a tap use it, so a finger that lands wide and drifts still
counts. It is clamped at run time against the battery and padlock cluster's left edge, so on the narrowest
board (the 172-pixel C6) it settles around 52 rather than reaching them - still a little over half again
the old size.

It grows sideways only. The house already fills the header's full height, and reaching below it would
start taking taps meant for the first row of a list: answering the wrong thing is worse than missing.

Built warning-free for esp32, esp32s3 and esp32c6. **Not yet flashed** - the chaos run owns all six
handhelds, and flashing resets a board, which would both disturb the run and spoil the battery figures it
exists to measure. It goes on at the end of the run.

### Two harness bugs the new audit flushed out in its first twenty minutes

Both found by starting the run and reading what it said, and both were making the harness lie. The second
one is the worse of the two, and it probably spoiled earlier nights without anyone noticing.

**A handheld with its radio switched off still counted as having a Wi-Fi session.** `wifi off` set the
flag and left `registered_node` alone, so the harness went on believing a session existed with the radio
off. Twelve seconds after switching Jolly's Wi-Fi off it sent Jolly a message *as though it were on
Wi-Fi*, counted it "both ends registered", and blamed the grid when it did not arrive - two false findings
in ten minutes, the first of them while LoRa had not finished coming up:

```
t=547.9  wifi_off      fnk0104b_c
t=560.0  probe sent    e40 -> fnk0104b_c   receiver_node: 0, receiver_path: "wifi"
t=574.5  lora_link_up  fnk0104b_c
t=590.0  FINDING: lost while both were registered
```

Worse than the false findings: **the new fallback audit keys on that same field**, so it could not see a
handheld on the backup during a LoRa round at all - blind to 85 of that schedule's 236 rounds, which are
precisely the ones whose backup use most needs judging. Now the hook clears the session, and a handheld
whose radio this run switched off is no longer counted as "grid unsteady" for being unregistered, because
that is the round working rather than the grid failing.

**An access point that restarted stayed "in trouble" for the rest of the run, so the grid was never
steady again.** `troubled()` compared each access point's boot counter against `baseline`, which is the
first reading of the run and never moves. The first restart therefore poisoned that access point
permanently, `unsteady()` counts a BLE-troubled access point as a reason not to inject, and every
remaining experiment would have been skipped. Caught because experiments #1 and #2 were refused five
minutes apart with **the identical boot numbers** - the same restart reported twice as if still happening:

```
#1 not started: grid still not steady: Everest (over BLE): boot 906 -> 907 (power-on or reset); ...
#2 not started: grid still not steady: Everest (over BLE): boot 906 -> 907 (power-on or reset); ...
```

That is a bad bug in a tool whose whole job is to restart things: a night of chaos restarts access points
on purpose, and arming their own chaos (D85) restarts all three, so **arming them guaranteed that nothing
would ever be tested**. A restart now counts as trouble only while it is recent (`AP_RESTART_GRACE_S`,
120 s - long enough to rejoin the backbone and announce health), judged from when that boot counter was
first *seen* rather than against the baseline, which has to stay put because the report differences
restarts and brownouts against it for the whole run.

**Cost of finding both: two restarts of the run, about 25 minutes of battery.** Worth it against a night
that would have produced a confident report about nothing. The first twenty minutes also measured
something real while all this was going on: 24 of 24 messages delivered, two D54 roams ("Moving to an AP
with fewer handhelds"), and a grid-wide event at t=503 where all six handhelds lost their session within
six seconds across all three access points and every one re-registered within five, **losing no
messages** - D53's self-healing holding under a whole-grid disturbance rather than being claimed.

### Chaos now refuses to be fooled by a backup radio

The owner's conclusion from the `pick_node` bug, in his words: *"if it is using a backup we need to make
sure that there was a good reason to use the backup"*, because *"in some of these handhelds, we won't
have those radios"*. The run reported that night as a success - every message arrived, carried by LoRa -
while a handheld sat unable to hold a Wi-Fi session. Four changes, so that cannot happen quietly again.

**Every fallback is judged, not just counted.** A handheld with no Wi-Fi session is sampled every 10 s,
and the harness asks whether Wi-Fi *could* have worked: is its radio switched off by this run (D78), is
it under an injected fault, is an AP in trouble on its own, is any AP up at all, is every AP full? If
none of those explains it, the fallback is **unjustified** and becomes a finding after 45 s - long enough
that an honest rejoin or roam is never blamed. The finding says so in the terms that matter: *"A handheld
with no backup radio would be off the grid for this long."* Every episode, justified or not, is filed in
`events.jsonl` (`fallback_began`, `fallback_ended`) and tabulated in a new report section, **"Did
anything lean on the backup, and did it need to?"**, so the justified ones can be told apart rather than
drowning the real ones.

**Every message records the path it took.** Each probe now carries `sender_path` and `receiver_path`
(`wifi` or `backup`) and, when an end was on the backup, `wifi_possible` and `backup_reason` - written at
send time so the log can be analysed afterwards without re-deriving any of it. The report counts how many
messages crossed on the backup and how many of those **should not have had to**.

**A new `no_net` round takes the safety net away.** It switches LoRa off on every handheld that has a
module, for a window, which makes those handhelds behave exactly like the ones that will ship without
one - and then a Wi-Fi fault has nowhere to hide, because nothing can carry the traffic for it. Any loss
during the window is reported as what a module-less handheld would have suffered. `lora off <seconds>`
restores itself from the handheld's own clock, so a harness that dies mid-round cannot leave a handheld
without its second radio.

**A new `alerts` round walks the emergency path end to end**: announcement, SOS, and the all clear that
stands it down, in that order, from one handheld - because the three are a sequence and D86's re-stamping
applies across it. While grid time is unset the round sends only the urgent pair, since an announcement
is refused by D6 and counting correct behaviour as a failure would manufacture findings.

Both new rounds take no board out and break no link, so the AP-count rules do not apply to them; they are
scheduled at `--extra-share` (default 0.22), at parity with each other. `--self-check` passes (46 parser
samples, 34 LoRa status checks, 600 schedules), and `--scenario lora` still yields only LoRa rounds.

**Also, so the alert round could exist: `send safe <text>` on the handheld console.** D66/D79's all clear
had no way in from serial, which made it the one alert this bench could never send - and therefore the one
chaos could never test, on the path where it matters most, since an SOS nobody can stand down is worse
than no SOS. Verified on the boards: Jolly sent `to everyone URGENT [sent, delivered 4]` and Channi logged
`from Jolly (device 9) to everyone URGENT [received]`.

Handheld firmware built warning-free for esp32, esp32s3 and esp32c6, and flashed to five of the six
handhelds on USB. **e40 is out of the next run**: it failed to flash with CH340 serial noise and then
stopped answering `id` at all, which is the bench, not this change.

### Fixed: a handheld refused to use the only AP on the grid, and D84 said it would not

The best finding of the day, and it came out of asking a reporting question. Chasing *why* the console
showed a vague "1 AP(s) in range, none of them usable yet", the log said something that should have been
impossible:

```
[NET] Heard AP 1 "Fuji" -47 dBm, 5 attached, 1 free, backbone no, grid time yes, 94 ms ago
[NET] Scan done: 10 records, 1 usable node(s)
[NET] Scan found 1 usable node(s); scanning again in 10 s     <- every ten seconds, for minutes
[NET] Joining Fuji (node 1, -47 dBm) as 192.168.4.101         <- only once the avoidance expired
```

One usable AP, and `pick_node` chose nothing. **`best_score` started at -1000 as a "nothing chosen yet"
sentinel, and that silently became a minimum quality that every avoided AP fell through:**
`AVOID_PENALTY_DB` is 2000, so Fuji scored `-47 + (1000 - 100 x 5 attached) - 2000 = -1547`, below the
sentinel, and `best` was left at -1.

**D84 promises the exact opposite**, in as many words: *"the avoidance penalty stays far above the
backbone and grid-time disqualifiers, so an avoided AP that is the only one left is still chosen."* The
decision was right and the code did not implement it. The consequence is the worst kind for this project:
a handheld that had been refused once would **not try the only AP on the grid again for up to two
minutes** (AVOID_MAX_MS), while sitting there with that AP at -47 dBm. On a one-AP grid - which is what a
night of chaos and two flat batteries leaves - that is a handheld off Wi-Fi for no reason.

The penalty is meant to *order* APs, not to veto one. Whether anything has been chosen is now tracked by
`best` alone (`best < 0 || score > best_score`), so the only way to choose no AP is for every candidate
to fail `node_usable`. Built warning-free for all three handheld targets and flashed to both Freenove
boards; both came up `ONLINE on Fuji`, `problem: none`.

**What it says about the testing, which matters more than the fix.** Nothing was logged as an error -
every part did what it was told, exactly as D84's own finding said of the bug before it. The handheld
even had a working LoRa fallback, so from the outside it looked fine, and the D80 fallback *hid* the
fault: messages kept crossing, so nothing looked broken. Chaos runs never caught it because they measure
whether messages arrive, and they did. It took reading one log line that disagreed with arithmetic.
A grid of three APs also hides it: the veto only bites when the avoided AP is the last one standing.

### Fixed: "on LoRa" now says which AP, and a full AP is no longer reported as out of range

Both found by the owner looking at two handhelds sitting on LoRa and asking the two obvious questions:
why are they not on Wi-Fi, and which AP are they even talking to.

**Why they were on LoRa** turned out to be the grid behaving correctly. Only one AP was alive - the
other two were brownout-looping on bench USB - and `nodes` on both handhelds showed `Fuji ... RSSI -45
... ATTACHED 4`. Fuji was at the heap ceiling the capacity enforcement now holds it to, so it refused
them (`registrations since boot: 0`) and they fell back to the second radio. Refused cleanly, still
reachable, not locked out: the capacity limit and the D80 fallback both did their job. Nothing to fix
there - but both of the handheld's answers *about* it were wrong.

- **"ON LORA (no Wi-Fi)" never named the AP.** The handheld knew all along: the radio keeps a bitmask of
  APs heard recently. The snapshot now carries that AP and its name (`lora_peer`, `lora_peer_ssid`),
  separate from `node`, which must keep meaning "an AP there is a Wi-Fi session with" - over LoRa there
  is none. The console prints `link: ON LORA (no Wi-Fi) via Fuji (AP 1)`, the home screen's one line
  reads `Channi on LoRa via Fuji`, Settings says `Fuji, over LoRa (no Wi-Fi)`, and Status's **AP** row
  names it instead of showing `--`. Verified on COM9 by taking Wi-Fi away with `wifi off 180`.
- **`problem: No LocalGrid AP in range` was false, and sent the owner the wrong way.** Fuji was in range
  at -45 dBm; it had no room. The old text came from counting only *usable* APs, and `node_usable`
  requires `free_slots > 0`, so a heard-but-full AP counted as absent. "Move the handheld" is the
  remedy for out of range; "free a slot or start another AP" is the remedy for this, and the handheld was
  recommending the first. It now counts APs heard by name separately from usable ones and says which
  case it is: `Fuji is in range but full (4 handhelds)`, or that every AP in range is full, or too weak,
  or that none is usable yet. The AP name is bounded to 16 characters because `HH_PROBLEM_MAX` is 72 and
  a name may be 32 - caught by `-Werror=format-truncation`, not by me.

Handheld built warning-free for all three targets (esp32, esp32s3, esp32c6) and flashed to both Freenove
boards.

### Diagnosed: the two handheld GPS modules hear nothing usable, and the screen said "looking"

The owner asked why the two Freenove handhelds, antennas at a window, never lock, while Everest locks
with its antenna indoors away from one - **the same model of module on both**. That comparison is what
made it solvable: a module that works on one board and not another is not a sky problem.

What the handhelds' own NMEA says, read off COM9:

```
$GPGSV,6,1,22,01,,,25,02,,,25,03,,,25,04,,,25
$GPGSV,6,2,22,05,,,25,06,,,26,07,,,25,08,,,25
$GPGSV,6,3,22,09,,,25,10,,,25,11,,,25,12,,,25
$GPGGA,201455.00,,,,,0,00,99.99,,,,,,
```

- **Twenty-two satellites "in view", PRNs 1 through 13 consecutively, all at 25 dB.** No more than about
  fourteen GPS satellites are ever above one horizon, and half of those PRNs were below it. This is not a
  sky view: the module is listing the PRNs it is **searching for** and reporting its own noise floor
  against each. Real signal would make some PRNs stand out; none do.
- **Empty elevation and azimuth on every one**, so it has never decoded an orbit for any of them.
- **The digital side is perfect**: 246,255 sentences and 1 bad checksum. UART, pin and baud are right.
- **It is not a cold start.** Measured across the day: 147,290 sentences, later 246,255 - about **eleven
  hours of uninterrupted reception**, never one satellite used. Waiting is not the missing ingredient,
  which was the first theory and was wrong.

Nothing in the firmware is at fault and nothing in it can fix it; the board profile is right
(`gps_rx = 44`, D65).

**Supply is ruled out.** The first suspicion was that the AP feeds its module 5 V (`docs/lora.md`: "VIN
(5 V) ... it has its own regulator") while a handheld fed it 3.3 V, which would leave the digital core
running and the RF front end starved - exactly the symptom. The owner checked, and the board profile
agrees: the FNK0104B's 4-pin UART connector carries RXD, TXD, GND and **5 V**, and 5 V is the *only*
supply pin it has, which is why the 3.3 V RYLR998 had to be powered from the I2C connector instead. So
both get the same supply. That pin cannot be measured from firmware either - the one supply ADC in the
profile is GPIO9 on the battery rail - so it would take a meter, and there is no longer a reason to.

**Measured instead: what the handheld's own radios cost its GPS.** One port held open so the board was
never reset, `gps` sampled every 15 s, LoRa alternated on and off three times, 30 samples per state:

| Board | LoRa | satellites in view, mean (range) | best dB-Hz, mean (max) |
|---|---|---|---|
| **Channi** | on | 8.6 (2-19) | **24.2 (28)** |
| **Channi** | off | 10.0 (4-17) | **27.4 (31)** |
| **Jolly** | on | 23.0 (23-23) | 29.7 (30) |
| **Jolly** | off | 23.0 (23-23) | 29.4 (30) |

- **LoRa costs Channi about 3 dB**, repeatably: mean best 24.2 against 27.4, peak 28 against 31. At this
  margin that is the whole answer for that board, because decoding an orbit takes about 30: with LoRa
  transmitting Channi cannot reach the floor, with it off it only just touches it. The RYLR998's antenna
  is centimetres from the GPS antenna on these boards and transmits 22 dBm. **Everest carries LoRa too
  (D71) and locks**, so proximity and a marginal antenna together are the problem, not LoRa by itself.
- **A first, badly-shaped version of this test claimed LoRa tripled the satellites in view** (about 3 to
  about 9). It switched LoRa off once, part way through, so anything drifting over those minutes looked
  like the result; alternating cut the difference to 8.6 against 10.0. The strength difference survived
  and the count difference did not. Recorded because the wrong answer was the more exciting one.
- **The two boards do not share one fault.** Jolly shows no LoRa sensitivity at all, and reports exactly
  23 in view in every one of 60 samples - a search list that never varies is not a receiver watching a
  changing sky. Channi's count moves between 2 and 19, which is a receiver doing something real.
- **Neither sustains 30 dB-Hz, so neither will lock as installed**, and the remaining work is physical:
  a real view of the sky rather than a window (the dominant term, and the only one worth doing first),
  distance between the GPS and LoRa antennas on Channi, and for Jolly a **swap** with Everest's
  known-good module to say whether the fault is its module or its antenna.

**Not a blocker for chaos.** GPS feeds handheld position (D65) and is a time source the grid does not
depend on (Everest and D63 cover time). Messaging, Wi-Fi and LoRa - what the runs test - are unaffected.
What cannot be tested until this is fixed is anything built on a handheld knowing where it is: the
position in an urgent alert, and distance and direction on Status.

### Fixed: a handheld now says when a fix is never coming

The fault above was invisible for eleven hours because both places that report GPS described an outcome
and not the evidence.

- **The console `gps`** said `no fix yet, 0 satellites` - which reads as "still looking". It now also
  prints what the module *hears*: `21 in view, 21 with a signal, best 25 dB-Hz`, the numbers that were
  already kept for the Status screen and never shown here. Verified on COM9.
- **Both now name the fault** when nothing is used, four or more are in view, and either the best
  strength is no better than the ~30 dB-Hz needed to decode an orbit, or **more satellites are in view
  than can be above any horizon**. The second tell is the specific one, and it is what caught these two
  (21, 22 and 25 in view). The console prints what to check; the Status **Lock** row stops saying
  "looking for satellites" and says `no: signal too weak to use, check the antenna` (D23 - the handheld
  reports its own state on its own screen, and the owner read this fault off that screen first).

### Measured (how long a 5000 mAh pack holds an access point, and it is not what I estimated)

Run to the batteries' end on 2026-09-30/10-01, three access points on battery, no cable, handhelds on USB
faulting themselves and each access point attacking itself over BLE.

| AP | Time discharging | Note |
|---|---|---|
| **Everest** | **9 h 38 min** | the only one with a GPS receiver |
| **Fuji** | **11 h 34 min** | |
| **Denali** | **12 h 42 min** | its pack gauge still read 25% an hour before it died |

- **The estimate was wrong by three to five times.** 28-45 h came from typical-current figures; the answer
  is 9½-12½. This is the whole argument for running it rather than calculating it.
- **A GPS costs about two hours** - but **pack-to-pack variation was about an hour by itself** (Fuji
  against Denali, same board, same firmware, same share of the load), so one board each cannot separate
  the two cleanly. Swapping the packs between boards and running again would.
- **The pack gauge is badly non-linear**: Denali's last quarter lasted an hour, its first three quarters
  eleven. It must not be read as "a quarter of the runtime left".
- **It is a floor, not the battery's life**: the figure is how long the access point kept beaconing, and
  these boards do not report their own charge - which is the gap that made this harder than it needed to
  be, since the owner had to look at a flashing light to tell which pack was failing.
- **Load balancing is exonerated.** Over 6,660 snapshots with all three alive the attachments split
  **33 / 34 / 33**, and after Everest died the two survivors took its orphans **3.00 against 3.02** (D54).

### Found at the end of the night: advertised capacity is advice, not a limit

The most actionable finding after the muteness, and the same root cause as Fuji's panic.

- **Denali advertised `0 free` and accepted a fifth handheld anyway**, then **dropped every session at
  once** - twice in forty minutes, all four or five handhelds reporting `Session receive error` on it
  within 200 ms, its heap floor at 0 KB. It never restarted: boot 897 throughout, uptime past nine hours.
  So where Fuji **died** of heap exhaustion, Denali **sheds the entire grid** and survives.
- **Cause.** `build_discovery` advertises what the heap can honour (fixed earlier the same night: 4.5 KB
  per station, 10 KB floor), but `sessions.c` still accepts up to `LG_PROTO_MAX_STATIONS` (15). The
  advertised figure steers a handheld's *choice* and refuses it nothing, so a handheld that tries anyway
  gets in - and the access point then cannot serve what it accepted.
- **Fix, and it is better defined than "do memory work":** refuse a session beyond what the heap can
  honour, so a handheld gets a clean refusal and looks elsewhere instead of joining a grid that then drops
  everyone. **Not implemented - it changes how many handhelds an access point will serve, which is the
  owner's call.**
- **Also measured, as the consequence of the honest advert:** with one access point left, four handhelds
  attached and **two were plainly offline with nowhere to go**. That is the right failure - visible, and
  far better than an access point taking all six and dying - and it says the real ceiling is **four
  handhelds per access point on these boards, not fifteen**.

### Fixed (a handheld went permanently mute, and chaos is the only reason we know)

The worst thing this project has found, and nothing in a full day of work on this exact code caught it.

- **What the night saw.** Round 15 on Jolly: **9 of 9 inbound delivered, 0 of 9 outbound**, with the radio
  plainly healthy - 11 frames out, 0 dropped, 0 seal failures, all three access points heard. The frames
  never reached the air: `[MSG] Not sent (outbox full)`, twenty-seven times. It was **still mute twenty
  minutes after rejoining Wi-Fi**, registered, online, good signal, screen showing nothing wrong.
- **Three correct decisions, interacting.** A message stays `LG_OUT_PENDING` until something acknowledges
  it; D6 refuses a message more than `LG_TIME_TOLERANCE_S` from the access point's clock, and a
  retransmission must reuse that stamp, so past that window it can **never** be accepted by any path; so
  each such message held its outbox slot for ever. Sixteen slots, and then the handheld could send nothing
  at all - **including an SOS** - until it was restarted.
- **Keeping it served nobody**: undeliverable, and holding it disabled the device. A pending message past
  the tolerance plus the re-stamp margin is now failed with `LG_ACK_REJ_TIME` - the reason the sender
  would have been given anyway - and its slot comes back. An alert is exempt while it still has a re-stamp
  left (D86), and ages out like anything else once they are spent.
- **Two scenarios reproduce it**: an outbox filled to the brim stays filled while the messages are fresh,
  frees itself once they are provably stale, and the handheld can send again; and an alert is not aged out
  while D86 can still give it a new identity.
- **One of them had to overturn a test written earlier the same night.** `test_restamping_is_bounded`
  asserted that an alert with its re-stamps spent **stays pending** - the leak, written down as intended
  behaviour. The suite caught the change, which is the clearest argument for having written it.
- **2525 checks, 0 failures** on an F4B. All six handhelds reflashed and the night restarted with the fix,
  with `outbox full` added to what the run watches for.
- **Why chaos found it and reading did not.** It needs hours of a handheld going in and out of Wi-Fi range
  with messages that are never acknowledged - sixteen of them, in the right order, across a window longer
  than two minutes. No unit test would have been written for that, and no amount of staring at
  `lg_client.c` suggested it. The night produced it in under three hours.

### Added (a run that ends when the batteries do, and a schedule that is different every night)
- **`--until-aps-die`**: the run goes until every access point has been silent for `--ap-dead-after`
  minutes (default 10) and then stops, instead of stopping on a clock. `--hours` becomes an upper bound,
  raised to at least 48 so the plan never runs out before the batteries do. Owner, 2026-10-01: run it
  until the APs have no battery left, and since they cannot measure their own charge, take silence as
  death.
- **Ten minutes, because a restart takes seconds.** An access point that reboots - its own chaos, or a
  crash - is quiet for a few seconds and comes back; nothing short of a flat battery keeps a 500 ms beacon
  silent for ten minutes. A long wait between experiments also gives up as soon as they are declared
  dead, so a run does not sit out a 15-minute gap after the thing it was testing has gone.
- **A battery section in the report**, from each access point's own last uptime before it went quiet, with
  the honest caveat that this is **a floor on the battery's life, not the battery's life**: it is how long
  the access point kept beaconing, and these boards cannot report their charge. Restarts during the run
  are listed beside it, because a restart resets that uptime and would otherwise read as a short battery.
- **The seed is random per run and printed.** It was already drawn from the clock when not given, but
  never recorded, so a night could not be replayed - `Seed None` in the header. Now a random seed is
  chosen, used and printed, which is both halves of what was asked: different every night, reproducible
  afterwards.

### Fixed (an access point with no status frame read as one with no backbone)
- **`links` was `len(health.links or [])`, which is 0 when no sealed health frame has arrived yet.** Zero
  links and "it has not said anything yet" are different claims, and conflating them marked every access
  point as having lost its backbone in the first seconds of a run - which `troubled()` would have read as
  a fault in progress and `unsteady()` as a grid that never settles, so nothing would ever have been
  injected. Caught in the first line of live output - `0 link(s), None handheld(s)` - before it could
  spend a night on it.
- Missing health is now `None` and reported as **"heard, but no fresh status frame from it"**, which is its
  own kind of trouble and says so, rather than borrowing the words for a different one.

### Added (a chaos night can test the whole system again, with the access points on battery)
- **The harness was blind to the access points once they left their cables, and I offered that as a
  limitation to live with.** The owner's answer was that it is not one: chaos is supposed to test
  everything, and a night that cannot fault the handhelds is not testing the leg we had just fixed. The
  obstacle was real but not fundamental - the APs *do* attack themselves when armed over BLE (D82, D85),
  and the problem was that this side could not **see** them, so it would have reported a message lost to
  an AP restart as a finding against the grid. A wrong finding is worse than a missing one.
- **`BleEyes`: the harness now listens to the same sealed status beacon the dashboard reads** (D68),
  on a thread, using grid_watch's own decoder. It takes no port, joins no Wi-Fi, carries no message and
  never holds a BLE address (D21, D25, D70). Three things change because of it:
  - **Attribution.** `active_faults()` counts an AP whose beacon says it is in trouble - a backbone link
    short of its peers, a boot counter that moved, a beacon gone quiet - exactly as it counts a board the
    harness is holding in reset. A probe in flight during an AP's own fault is no longer "clean".
  - **Steady state.** With no AP on USB, an AP short of a link or freshly restarted means the grid is not
    back yet, instead of the gate having nothing to look at.
  - **The report.** A new section accounts for every AP: heard, boot, links, handhelds, and the restarts
    and brownouts that happened *during the run*, with the honest note that a restart may well be the AP
    breaking itself on purpose and only its journal can say which.
- **`--no-ble`** turns it off, for a run where nothing is armed, and says plainly what that costs.
- **Found by running it, again.** The first attempt heard nothing: `grid_watch.listen` *awaits*
  `stop.wait()`, and a `threading.Event`'s `wait()` is not awaitable, so the listener died on its first
  pass. It was caught only because the harness reported "no AP was heard" as a **finding** rather than
  carrying on as though it could see - and it now prints the listener's own error with it. Verified live:
  Everest heard 1.3 s in, with boot, links, handhelds and brownout count, and all three in the report.

### Fixed (an overnight run over BLE would have ended whenever the laptop dozed)
- **`chaos_ble.py` never asked Windows to stay awake.** The dead-man window is the safety property of a
  wireless run - nobody renews it, the run stops (D83) - and that makes a sleeping laptop
  indistinguishable from a laptop that was carried off, which is correct. It also means a ten-hour night
  would have ended quietly at whenever the machine dozed, with nothing in the morning to say why beyond a
  journal that stops early. `tools/chaos.py` has held sleep off since its first night run; the BLE path
  needed the same and did not have it. Released on the way out, so a finished run leaves the power
  settings as it found them.
- **A failed renewal no longer ends the run silently.** One miss is survivable - the window is minutes
  wide - so the loop now reports the failure, keeps trying, and says roughly how long the window has left
  if they keep failing. Before, a single bad BLE connection at 2 a.m. would have dropped out of the loop
  and left the access points to disarm themselves with no explanation.

### Added (the chaos journal records what the second radio carried) — TEMPORARY, part of T1
- **A chaos night could show that the grid survived an ESP-NOW outage and not that LoRa carried it.** The
  journal recorded which fault happened; the D68 beacon carries heap, restarts and brownouts. Neither
  carried a LoRa counter, so with the access points on battery the central claim of a LoRa night was the
  one thing the evidence could not reach.
- **Three records, so the 16-byte layout does not change**: `CHAOS_EV_LORA_FIRST`, `_OUT` and `_IN`.
  **FIRST is the one to read** - it is the firmware's own "N of them arrived here first", frames that came
  over LoRa before Wi-Fi had delivered them, so a rising count across a backbone-off window is the second
  radio doing the grid's work and a flat one is the grid having stopped instead of healed.
- **Written where the evidence is**: just before each fault, again two seconds after the outage it caused
  should have lifted, and every `CHAOS_LORA_EVERY_S` (300 s) while armed so a quiet stretch still has a
  baseline. A ten-hour armed night costs about 360 of the 4095 records, and the 60-fault budget at most
  another 360.
- **An access point with no module writes nothing** rather than a row of zeros, because a zero would read
  as "carried nothing" when the truth is "has no radio".
- **`chaos_ble.py journal` now says what it means**, pairing the snapshots either side of each fault:
  `backbone off 120 s | LoRa first +15 out +31 in +29 | LoRa carried the grid while ESP-NOW was down`.
  Exercised offline against all four shapes it has to report honestly - carried, did not carry, the radio
  moving frames while supposedly off, and a restart that loses the snapshot after it.

### Verified on the boards (both access point fixes are live, and on battery)
- All three access points flashed by cable and returned to battery: **42 status frames verified, 0 failed**.
- **The capacity fix is visibly working.** Earlier tonight Fuji advertised `1 attached, 6 free`; it now
  advertises `2 attached, 3 free`, Denali the same, and Everest - which carries the GPS and the clock, so
  has least heap - offers `1 attached, 2 free`. Thirteen slots across the grid for six handhelds,
  recomputed as heap falls, and **no single access point is invited to take all six** the way Fuji was
  when it panicked.

### Added (D86: an alert too old to be accepted is said again under a new id) — owner's decision
- **The owner's answer to the two-minute conflict, asked and given on 2026-09-30:** "go with possibly
  twice for urgent and SOS". A message delayed past `LG_TIME_TOLERANCE_S` could never be delivered,
  because D6's clock check compares the message's stamp to the access point's clock and a retransmission
  must reuse that stamp. For an urgent broadcast, an SOS or an all clear, arriving twice beats not
  arriving.
- **A new sequence number as well as a new stamp, and that is not a detail.** An access point
  deduplicates on (author, boot, seq) before it ever looks at the clock, and the AEAD nonce comes from
  the same triple - so reusing the sequence with a fresh grid time would be both invisible to the access
  point and a nonce reuse, which is the one thing this project does not do. The new id is precisely why
  the recipient may see the alert twice, and that is the trade that was chosen rather than a side effect.
- **Ordinary messages are untouched.** 1:1 and group text keeps its identity however old it gets, so
  nobody sees a chat message twice. A scenario fails if that stops being true.
- **Bounded at three re-stamps**, after which the alert stays pending with its last id instead of
  re-stamping for ever; and `LG_TIME_RESTAMP_MARGIN_S` (30 s) keeps a re-stamped frame from spending what
  is left of its window in the air - seconds, over LoRa - and being refused for the same reason as the
  copy it replaced.
- Three scenarios in `tests/target/main/test_messaging.c`: the alert is re-stamped and then crosses where
  its old stamp could not; the chat message keeps its id however old; re-stamping stops at the cap.

### Measured (the retry works, and the asymmetry was never the radio)

The same chaos round, same seed, same boards, same 197 s window, three times - the only change between
the second and third being that the retry pass actually runs:

| | First run | Re-run (10 s resend, tick still skipped) | Tick fixed |
|---|---|---|---|
| Delivered **from** the LoRa-only handheld | 6/9 | 6/9 | **9/9** |
| Delivered **to** it | 8/9 | 6/7 | 8/9 |
| Its LoRa frames on the air | 21 | 21 | **28** |

- **Outbound went from two thirds to all nine**, and the frame count is the proof it is real rather than
  luck: seven more frames on the air, which is the retransmissions appearing where there had been none.
  The first two columns are identical in both numbers, which is what a guard that silently skips the
  retry looks like from the outside.
- **The explanation held.** The 66%-against-83% asymmetry was not a directional radio: a message *to* the
  handheld was retried for free by the sending handheld's 3 s Wi-Fi resend, each resend making an access
  point transmit again, while a message *from* it had one attempt and four stacked gates - three in
  `lg_core`, one in `hh_service` - between it and a second.
- Honest reading of the remaining 8/9 inbound: one loss in nine on a path that already had retries, which
  is the per-attempt loss rate showing through, not a regression.
- **The latency distribution is the signature, and the whole retry chain is visible in it.** A cluster at
  **1.3 s** (first attempt), five messages at **11.34-11.94 s** (the +10 s second attempt) and one at
  **22.53 s** (the third). That last one lost two frames in a row and still arrived. Every one of those
  six would have been a silent loss this morning, and a message that arrives eleven seconds late instead
  of never is the whole point on a radio with no link-layer acknowledgement.
- **A second benefit, unlooked for: the clock refusals mostly went away too**, from **six in the first run
  to one**. A retry that fires at 10 s lands inside D6's two-minute tolerance; the 30 s one fired outside
  it. So the pacing fix removed most of the staleness problem as a side effect, and D86 covers what is
  left for alerts.

### Fixed (a fourth gate: the outbox was not serviced at all while a handheld was on LoRa)
- **The retry that was supposed to fix the loss rate never ran.** `hh_service`'s service pass returns
  early when `s.wifi_off` is set, above the line that calls `lg_client_tick()` - so with Wi-Fi off, the
  one thing that drives the outbox was skipped. Three gates in `lg_core` had been opened to let a
  LoRa-only handheld send; this is the same bug one layer out, in the firmware.
- **The comment above that return was true when it was written** - "every branch of the state machine
  either scans, joins, or talks over a socket" - and stopped being true the moment the core learned to
  send without a session. That is the shape of all four of these: a guard that was correct for Wi-Fi,
  left in place while the thing underneath it changed.
- **Caught by arithmetic, not by reading.** The 10 s resend predicted that outbound delivery would climb
  towards the inbound rate. A re-run on the same seed gave the same **6/9 outbound and the same 21 LoRa
  frames out** - and 21 is exactly nine sends plus six acknowledgements plus the announcements, with no
  room for a single retransmission. A prediction that failed was more useful than the fix that looked
  right: the counters said the retry had never reached the air.
- The tick moved above the return, where the radio is already serviced for the same reason. The interval
  and the one-message-per-pass cap live in the core, so this costs nothing while Wi-Fi is up.

### Found by the first real chaos run of the handheld LoRa leg (2026-09-30, access points on battery)

Three rounds, every one recovered in 0.2 s, **84 of 100 messages delivered**, median latency 1.1 s and
p95 1.7 s. The rounds themselves, each switching one handheld's Wi-Fi off and messaging it both ways:

| Round | From it | To it | Its LoRa frames out |
|---|---|---|---|
| 1 (Jolly, 197 s) | 6/9 | 8/9 | 21 |
| 2 (Channi, 184 s) | 6/8 | 5/8 | 19 |
| 3 (Jolly, 293 s) | 8/13 | 12/13 | 28 |
| **Total** | **20/30 (66%)** | **25/30 (83%)** | 68 |

- **The radios carry, and they are not lossless.** 75% overall at 1.1 s median, with **no local drops at
  all** - no queue drops, no reassembly timeouts, nothing refused by type on either side. The frames were
  simply not heard. Nothing acknowledges at the LoRa link layer, so the answer is retries, which is why
  `LG_RESEND_LORA_MS` came down to 10 s in the same session.
- **Outbound from a handheld is consistently lossier than inbound to it**, 66% against 83%, on both
  boards and in every round - and the two directions are not retried alike, which looks like the whole
  of it. A message *to* the LoRa-only handheld is sent by a **registered** handheld, whose outbox resends
  every 3 s over Wi-Fi; each resend makes the access point transmit over LoRa again, so an inbound
  message quietly gets several attempts, all of them inside the two-minute clock tolerance. A message
  *from* it had one attempt and then a 30 s wait, and by the time that fired the stamp was old enough to
  be refused (above). So the asymmetry is not the radio being directional: it is **one side having
  retries and the other not**.
- That makes a prediction worth testing rather than asserting: with the resend at 10 s and one message
  per tick, the outbound rate should move towards the inbound one. The same seed was re-run on the same
  boards to find out, which is the only way this stops being a story.

### Found by the same run: a message that waits more than two minutes can never be delivered

- **Six messages came back `Refused by the AP (time)`**, every one of them in the seconds after Wi-Fi
  returned, retrying something queued while the handheld was on LoRa.
- **Two correct rules that contradict each other.** D6 says a handheld more than two minutes off its
  access point may send only urgent broadcasts, enforced by `time_ok()`:
  `absdiff(ap_clock, frame.grid_time) <= LG_TIME_TOLERANCE_S` (120 s). And a retransmission **must** reuse
  its original grid time, because identity and the AEAD nonce are built from it. So a message stamped
  during an outage is refused on every later attempt, for ever, and the stamp can only get older.
- **The implementation is stricter than the decision.** D6 is about *a handheld whose clock is wrong*;
  `time_ok()` measures *how old a message is*. A handheld with a perfectly correct clock has its
  three-minute-old message refused. An access point cannot tell the two apart from the frame alone.
- **Owner's decision, asked and not taken here**, because it changes a numbered decision and both
  branches are defensible: after two minutes, should a message arrive **possibly twice** or **not at
  all**? Re-stamping a message nothing has acknowledged makes it deliverable and risks a duplicate if an
  access point silently accepted the first copy; keeping the stamp guarantees no duplicate and no
  delivery. The recommendation on the table is *possibly twice, at least for urgent and SOS* - a
  duplicate alert is an annoyance and a missing one is the failure the radio exists to prevent - or, if
  D6 is to stand untouched, raising the tolerance to cover a realistic outage.
- **One thing this already explains:** the 10 s retry interval is not only about latency. It is the only
  interval that fits *inside* the two-minute tolerance, so attempts two and three can still be accepted
  where a 30 s retry's third attempt would already be refused.

### Fixed (a handheld said "searching" while it was delivering messages over LoRa)
- **The one thing on the screen that was not true.** With Wi-Fi off and LoRa carrying, `status` reported
  `link: SEARCHING` and the launcher said "Looking for an AP" - while messages were crossing in 1.3 s and
  acknowledgements were coming back. D80 gave the *grid* a third presence state; the handheld had none
  for its own link, so the device in someone's hand understated what it was doing (D23).
- **`HH_LINK_LORA`, reported and never entered.** The service's own state machine never assigns it, so
  every `== HH_LINK_ONLINE` test inside the service keeps meaning exactly what it meant and none of the
  23 of them had to be re-reasoned. `publish()` - the single place the UI is told the link state - maps
  SEARCHING to it when `hh_lora_has_peer()` is true, which is the same test the radio itself uses before
  spending airtime on a transmission.
- The console says `ON LORA (no Wi-Fi)`, the status screen says `on LoRa` and **does not mark it a
  warning**, because the second radio doing its job is not a fault; Settings says "over LoRa, no Wi-Fi"
  where it used to say "not connected"; and the launcher says "<name> on LoRa" instead of "Looking for an
  AP".

### Fixed (a notification was covered by the screen saver before it could be read)
- **Reported by the owner, 2026-09-30:** a message arriving while the screen saver was running was not
  shown. The overlay *did* wake for it - `s.cover = OV_NONE`, banner drawn, bell rung - and then the
  saver covered it again on the very next tick, about 40 ms later on the bench:

  ```
  I (20602710) UI: [UI] Notified: Channi in Channi
  I (20602750) UI: [UI] Screen saver on
  ```

- **Only touch counted as activity.** The saver fired on `now - last_touch_ms >= SAVER_IDLE_MS`, and a
  message is not a touch, so once the idle minute had passed the rain came back immediately and kept
  coming back. The reader saw the rain stutter and never saw the message - the notification was
  delivered, sounded, counted as unread, and invisible.
- **Showing something now counts too.** A banner raised, or the saver woken for a message, records
  `woke_ms`, and the saver waits for the longer of the two to go quiet. Both are measured as a
  difference from now rather than compared with each other, so neither reading breaks when the
  millisecond counter wraps. The banner therefore lasts its full fifteen seconds, and the idle minute
  restarts from the moment the message appeared rather than from the last finger.
- The overlay's log line now says how long ago something was last shown, beside how long since the last
  touch, so the next person can see which clock is holding the rain off.
- **Measured before and after, on the same grid, by accident.** `e40` would not take a flash twice over
  (serial noise on its CH340), so it ran the old firmware through a chaos run beside five reflashed
  boards - a controlled comparison nobody had to arrange:

  | Board | Firmware | Notifications | Covered by the saver | How soon |
  |---|---|---|---|---|
  | e40 | old | 5 | 5 | **80-90 ms** |
  | e28b, hosyond | fixed | 14 | 8 | 59,850 ms |
  | fnk0104b | fixed | 11 | 0 | - |

  On the old firmware a message was on screen for a **twelfth of a second**. On the fixed one the rain
  returns after the full idle minute, as intended, long after the banner has had its fifteen seconds.

### Found on the bench (an access point panicked at six handhelds while advertising room for fifteen)
- **Fuji crashed on battery with no fault injected.** At 21:20:46 on 2026-09-30 its boot counter went
  1029 -> 1030 and its beacon reported `crash (panic)`. Its brownout counter did not move, so this was
  not the supply, and nothing had been injected: the run that was going at the time never started an
  experiment.
- **What the beacon timeline shows, at 5 s resolution.** Handhelds piled onto one access point as they
  re-registered, and its heap floor was already on the floor when they did:

  ```
  21:20:20  Everest hh4 heap 1KB | Fuji hh4 heap 0KB | Denali hh0 heap 0KB
  21:20:35  Everest hh0 heap 1KB | Fuji hh6 heap 0KB | Denali hh0 heap 0KB
  21:20:46  Everest hh0 heap 1KB | Fuji b1030 hh0 heap 27KB  <- panicked and restarted
  21:21:31  Everest hh4 heap 0KB | Fuji hh2 heap 18KB
  ```

  Fuji held **all six handhelds** fifteen seconds before it died, while Everest shed its own to zero.
- **The numbers give the real capacity.** Fresh after the restart Fuji had 27 KB free with no handheld
  and 18 KB with two, so a session costs roughly **4.5 KB**. Six sessions is about 27 KB, which is all
  of it. Meanwhile `LG_PROTO_MAX_STATIONS` is **15**
  ([lg_proto_config.h:17](firmware/common/lg_proto_config.h:17)) and that is what an access point
  advertises room for. **It invites fifteen and panics at six** - and because it advertises the room,
  D54's load balancing steers handhelds *towards* the access point least able to hold them.
- This is the capacity question in a concrete form rather than an estimate: it is the owner's decision
  what an access point must carry, and the choice is now between raising the real capacity and lowering
  the advertised one. Nothing has been tuned away.
- **No serial log exists for it**, because the access points are on battery. The reset reason in the D68
  beacon is the whole of the evidence, which is exactly the observability gap already recorded: a panic
  on battery is visible and not diagnosable.
- **The advertised capacity is re-measured, because that is how the AP talked itself into dying.** The
  free-slot byte is already heap-aware - it was fixed once, from the flat `15 - clients` to
  `(heap - DISC_HEAP_FLOOR) / DISC_HEAP_PER_STATION` - but the per-station figure was **3 KB against a
  measured 4.5 KB**, and that error compounds with every handheld admitted. Six were admitted. The floor
  was **6 KB, below the 13 KB an AP needs to keep offering its BLE admin link**, so an AP could advertise
  room right up to the point where the only remote way to reach it was gone - on battery, gone for good.
  Now 4608 and 10240.
- **What that means, stated rather than tuned away: an AP honestly advertises three or four handhelds.**
  Not fifteen, not six. Raising the real number is a memory problem in the per-session buffers, not an
  advertising one, and how many an access point must carry stays the owner's decision.
- **Not yet on the boards.** The access points are on battery with no cable, so this is built and waiting
  for either a cable or an over-the-air upload (D81, itself never yet exercised).

### Fixed (two bugs in the new chaos round, both found by running it)
- **The grid could never be called steady with the access points on battery**, so no fault was ever
  injected. `unsteady()` required every handheld to be registered with an AP *the run holds a port on*,
  and on battery there are none, so the condition was unsatisfiable while the grid was in fact healthy.
  With no AP on USB a handheld being registered at all is now the evidence used - it is evidence about
  the same thing, since an AP had to accept it.
- **`handhelds with a LoRa module: none`, with two of them fitted and working.** The `lora` status block
  was parsed only `if (b.is_ap)`, from before handhelds had one (D77), so no handheld ever reported a
  module and the round that needs one was never scheduled. The two blocks are told apart by their own
  headers, so parsing both on every board cannot confuse them.
- **The round measured its own clock wrong and reported a total failure that had not happened.** It
  called `handheld_probe(self.now_s(), ...)`, and the parameter that reached was the probe's `mono`
  stamp - which `probe_tick` compares against `time.monotonic()`. Seconds-since-the-run-started is a
  much smaller number, so every aimed probe looked older than the 30 s deadline on the next tick and
  was resolved as **lost 0.1 s after it was sent**. The round then stated "not one of 9 messages it
  sent while on LoRa alone was delivered", which is exactly the sentence it exists to produce, while
  the board's own log showed messages arriving and a delivery receipt coming back over LoRa.
  **A measurement bug that reads like the failure it was written to catch is worse than no test**, and
  the only reason it was caught is that the probe records disagreed with the serial log. The clock is
  no longer a parameter: `handheld_probe` takes `time.monotonic()` itself, so no caller can hand it
  another one.
- **In-flight messages are no longer blamed on the round.** A probe sent by the ordinary traffic a
  moment before the window opened was resolved as lost inside it and counted clean, because the
  handheld's own "lost its AP" event had not arrived yet. The round now waits for anything in flight to
  or from the victim to land before it takes the Wi-Fi away.
- Every one of these was found by running the round rather than by reading it, which is the argument
  for running it - and two of the four were the round's own fault, which is the argument for reading
  what it reports against the boards rather than believing it.

### Fixed (the LoRa radios could receive a message but not send one - the bug that mattered most)
- **A handheld reachable only over LoRa could originate nothing except "I am here".** Found on the
  bench, not by chaos: with Channi's Wi-Fi switched off (D78), a message sent *to* it arrived over
  LoRa, was decoded and was shown on screen - and its own reply never reached the air. **One frame
  went out in 150 seconds, and that one was the announcement.** The reply sat in the outbox and left
  over Wi-Fi 70 s later, the instant a session came back, which is the one outcome that looks like
  success in a message list and is a total failure of the second radio.
- **Three gates in `lg_core`, each right for a socket and wrong for a radio.** `send_frame()` refused
  before building anything unless `connected`; the first attempt happened only `if (c->registered)`;
  and `lg_client_tick()` returned early unless registered, so nothing was ever retried either. A
  handheld on LoRa has no session and never will - it is not a backbone peer (D77) - so on that radio
  "not connected" is the **normal** state and precisely when speaking matters. The announcement had
  already been carved out of the second gate when it hit the same wall; the carve-out should have been
  the clue that the gates themselves were in the wrong place.
- **The frame is now offered to `io.send` whether or not there is a session**, and the glue decides
  what a radio carries, which is where that belongs (D74's one-radio-at-a-time, D77's allow-list).
  `send_frame()` still returns false unless a session took it, so the message stays `PENDING` and
  keeps its stored `(boot, seq, grid time)`: a later Wi-Fi attempt at something LoRa already delivered
  is a duplicate the grid drops **by identity**, not a second message. Reusing those three fields is
  load-bearing, and a scenario fails if it stops happening.
- **An attempt made with no session cannot reject the message** (`transmit_ex(..., may_reject)`). A
  missing peer key means "not learned yet", which before registration is the normal state, so
  rejecting there would fail a message for the one reason about to stop being true.
- **`LG_RESEND_LORA_MS` = 10 s, one message per tick, oldest first.** One LoRa attempt is not a
  delivery - nothing acknowledges at the link layer - but `LG_RESEND_MS` is 3 s, tuned for a socket: a
  full 16-slot outbox retried that fast would offer five messages a second to a radio that carries about
  one every three (D74). The first guess was a 30 s interval, and the bench showed it was the wrong
  shape of guard: a chaos round measured **13 of 15 messages delivered at a median 1.3 s**, so the air
  was not the constraint, while the two that dropped waited the full thirty seconds for a second
  attempt. The rate is now bounded by `LG_RESEND_LORA_MAX_PER_TICK` (one), which holds the offer rate
  the same whether one message is waiting or sixteen, so the interval is free to be short: a single loss
  costs ten seconds instead of thirty. Oldest sequence number first, which is the order a socket would
  have sent them in.
- **Verified on the boards, 2026-09-30.** Channi with Wi-Fi off for 110 s, sending to Jolly:
  `[MSG] Sent to device 9` -> Jolly shows `from Channi (device 1): reply over LoRa please` -> and
  `[MSG] Frame over LoRa from AP 1, 45 bytes` / `[MSG] Delivered to the other handheld` back on
  Channi. **A 2.7 s round trip over LoRa at SF9, acknowledgement included**, with Wi-Fi not returning
  for another 48 s. No duplicate arrived when it did.
- **`tests/target`: 2421 checks, 0 failures** on an F4B, including the dedup property above and a
  guard that a handheld with **no** module behaves exactly as before - the offer is made, because the
  core cannot know whether a radio is there and must not guess, nothing carries it, and the message
  waits instead of failing.
- **The simulator gained the second radio** (`sim_lora`), modelling what an AP's glue does in the same
  order as `firmware/node/main/lora.c`: mark the device reachable over LoRa *first*, so an
  acknowledgement built while handling the frame already has a path, then hand the frame over on a
  session named by the device rather than by a socket. Writing it exposed that the first version of
  these scenarios was wrong in an instructive way: a handheld must **announce** before the grid can
  reach it, and until it has, a message to it is correctly refused rather than sent down a path
  nobody knows about.

### Verified on battery (the brownouts, and what the bench supply was really doing)
- **A LoRa burst with all three APs on battery: zero brownouts, zero restarts.** 18 urgent broadcasts
  from three handhelds in 160 s - urgent frames cross LoRa unconditionally (D74), so every AP had its
  radio on air - against a lifetime baseline of 28, 7 and 24 brownouts. 182 consecutive health
  snapshots, no boot counter moved. On the USB bench the same APs browned out 28 times per AP in 18
  seconds. **The supply was the fault, not the firmware and not the radio.**
- Honest limits of that run: 18 frames in 160 s is a moderate burst, not the heaviest the radio can
  be given, and **urgent broadcasts are the wrong instrument** - they are loud on every screen, as the
  owner found out. The right heavy-LoRa load is an AP with its ESP-NOW backbone down, which puts the
  grid's ordinary traffic (~35 messages a minute) onto LoRa silently.
- Denali stopped beaconing for **14 s** during the burst, with no restart. Worth watching: BLE adverts
  starving while LoRa transmits would make a chaos run harder to observe exactly when it matters.
- **Battery life watch started.** All three APs came up on battery at **19:51 on 2026-09-30** (from
  the uptime in the first health snapshot), each on a 5000 mAh pack at 100%. Health is sampled every
  5 s; an AP counts as dead when it stops beaconing and does not come back, which is distinguishable
  from a restart because a restart moves the boot counter.

### Added (D85: chaos armed from the phone over BLE, behind the admin password) — TEMPORARY
- **This was asked for and not built, and that was my error.** The owner asked for a chaos run startable from the phone without USB, said plainly that it breaks earlier decisions and that this was acceptable while the bench cannot power its own APs, and had already answered the scope question with "the APs alone". D83 answered with Wi-Fi control and recorded that BLE was "deliberately not" used. The reasoning was sound; applying it against an explicit instruction was not mine to do. D83's BLE paragraph is struck through in place rather than quietly edited, so the reversal is legible.
- **Four opcodes on the D70 admin link** — `OP_CHAOS_GET`, `OP_CHAOS_ARM`, `OP_CHAOS_DISARM`, `OP_CHAOS_LOG` — the only opcodes on that link that change anything. **No new crypto:** `OP_LOGIN` already proves the admin password with an HMAC over a random 32-byte challenge keyed by the stored hash, inside a channel sealed with K_link, so these inherit real authentication. Every guard from D82 and D83 is unchanged: arming is refused unless the firmware is a chaos build, refused when the grid has no time (the deadline could not otherwise be enforced), the window is capped and **stops itself when nobody renews it**, and the fault budget stays counted in flash.
- **`tools/chaos_ble.py`**: `status`, `arm <minutes>`, `arm <minutes> --renew-for <minutes>`, `disarm`, `journal`, from this laptop over BLE. It finds APs by their D68 status beacon — not by the admin link's service UUID, which is a GATT service and is not in the advertisement at all.
- Verified on a board: an AP running the chaos build answers `OP_CHAOS_GET` with **"log in first"**, which proves both that the opcode exists (an absent one answers "unknown request") and that it is password-gated.

### Fixed (`chaos_ble.py` armed whichever AP was loudest, whatever it was told)
- **`--ap` could not select an access point.** It matched its argument against the advertised BLE
  name, and an AP advertises none, so every name matched nothing and the tool fell back to the
  strongest signal without saying so. "Arm one AP first and watch it through a fault" was the plan
  for the first run on battery, and the tool could not do it. On this bench the loudest AP was
  **Fuji**, the one with a 0 KB heap floor and a watchdog crash as its last reset — the worst of the
  three to pick by accident.
- **An AP is now identified by its AP index**, read from the discovery payload in the clear (byte 7)
  and from the sealed status frame's header when only that arrives, and named from
  `tools/bench_devices.json`: `--ap Denali` and `--ap 2` both work. Scanning uses a detection
  callback rather than `discover()`, because the status frame rides in the **scan response** and only
  the callback hands both parts over. Verified against the three APs on battery: `Denali` -> AP 2,
  `0` -> Everest, an unknown name refused with what was heard.
- **The choice is checked again on the sealed link**, where `HELLO_OK` states the AP index: a
  mismatch between the advert and the link refuses the command rather than arming the wrong access
  point. An advert is not proof of who answered.
- With no `--ap` the tool now **prints every AP it heard** and says that it is taking the strongest,
  instead of arming one silently. Addresses stay unprinted and unlogged (D21).

### Fixed (the BLE link was never reachable on this bench, for a reason nothing reported)
- **Every AP beaconed and refused every BLE connection.** D70 only advertises connectably above `ADV_MIN_HEAP` (13312 bytes) and these APs run at 8–13 KB once handhelds attach, so the admin link — and therefore any BLE chaos control — could not be reached at all. Nothing logged this: the status beacon keeps going either way, so from the laptop it looks exactly like an AP that is present and simply will not talk.
- **A chaos build no longer starts the web server** (`CONFIG_LG_NODE_CHAOS_NO_WEB`, on by default with chaos). Its task stack alone is 6 KB and nobody is on the AP's Wi-Fi during a BLE-driven run on battery. Measured on Denali: heap went from **8–18 KB to 21–30 KB**, and 13 KB minimum with three handhelds attached — comfortably clear of the floor. Turn the option off to keep D83's Wi-Fi arming in a chaos build instead.
- **`web_admin_init()`**, split out of `web_admin_start()`. The two mutexes the snapshot and its emitter need were created inside the server start, so skipping the server left them NULL and the AP panicked on the first publish — a boot loop every 1.1 s, which is what the board did the first time I tried this. The BLE link reads that same snapshot through the same emitter, so the state has to exist without the server.

### Added (a push is refused, not only a release)
- **`.githooks/pre-push`** runs `tools/check_release.py --release` and **stops `git push`** while anything in `docs/TEMPORARY.md` is still in the tree. The owner's rule: this comes out "before we do a push to GitHub or before we do a release". Install with **`python tools/install_hooks.py`** (git does not carry hooks through a clone, so this sets `core.hooksPath`). One bypass, `LG_ALLOW_TEMPORARY=1 git push`, deliberately awkward and visible in shell history.
- **T5 added to the register** and to the checker, which now lists the four BLE opcodes by name rather than trusting a comment. CI holds the default branch to the same bar as a release tag, and fails if the committed hook is removed or stops running the check.

### Fixed (the bench came back, and the first ten seconds of chaos found a restart loop)
- **An AP could restart itself out of the grid whenever several handhelds joined at once.** `SEND_WAIT_MS` bounds how long *one* handheld may hold up the core task, at 2 s, and that was read as bounding the task. It does not: a pass can send to every session, so three full sockets cost 6 s, and the task watchdog panics at 5 s. Everest did exactly that — `[UP] 2 s`, then **nothing for 5.7 s**, two sessions closing with `send failed` two seconds apart, and `task_wdt: lg_core` aborting right after the second. The per-send cap was never the dangerous number; the missing per-pass one was.
- **`sess_pass_begin()` gives each core pass a shared 2 s send budget**, reset beside the watchdog feed it protects. Once it is gone, a frame that has not started going out is **dropped and the session kept** — the same trade the voice path already makes (D61) — rather than waited for. Closing would punish one handheld for how slow another was, and a dropped message costs one message where a watchdog panic costs every handheld on that AP. Worst case is now the budget plus one frame that must finish, 4 s, inside the 5 s watchdog. Two new counters, `budget_drops` and `slowest_pass_ms`, so the trade is visible rather than silent.
- **Three of the admin page's routes were never registered on a running AP.** `config.max_uri_handlers` was written down as 12 beside a table that had grown to 15, so the server registered the first twelve, logged `no slots left` three times at warning level, and served an admin page whose newest endpoints did not exist — which is to say **D83's chaos arming and journal were absent from every AP** while present in the tree, and `tools/check_release.py` went on reporting T2 as present because the routes were all there in the source. The limit is now counted from the table, a route that fails to register is logged at **error** level, and a board confirmed the warnings are gone.
- **`tools/chaos.py` died with a segmentation fault before injecting anything** when a port it was not using could not be opened. Closing a port while its reader thread sits in `read()` takes the process down, and no `try/except` around the close can catch it; `close_all()` stopped every reader first, but a port released *during discovery* had no way to stop just its own. Each board now owns a stop flag that `release_port` sets and joins before the handle goes. Found by one board on this bench whose CH340 answers `PermissionError`.
- **`tools/sync_ports.py --write` crashed with `NameError: MAP`** — it had imported `save_map` and then not used it. Reporting worked, so the bug sat in the one path that changes anything.
- **`tools/check_release.py` printed one row backwards.** Its last row warns when the README has *stopped* calling this a prototype, the opposite polarity from the concession rows, and it used the same present/gone wording — so a README that correctly does say prototype was reported as `gone`. Caught by reading the tool's own output instead of trusting its exit code.

### Added (D84: an AP advertises the room its memory can honour)
- **The free-slot byte in the discovery beacon was room in the table, not room in the AP.** It was `LG_PROTO_MAX_STATIONS - clients`, and that constant is **15** — the SoftAP limit — where the memory-limited figure on these boards is **two or three**. Every AP advertised roughly five times its real capacity, and a handheld's choice of AP and D54's balancing are both built on that number.
- It is now derived from free heap: a 6 KB floor the AP keeps for itself, about 3 KB for each further handheld, both measured on this bench, never more than the table allows. **An AP with no handhelds at all still offers at least one slot** — if every AP were honest enough to advertise zero at the same moment, `node_usable` would reject all of them and strand every handheld, which is a worse failure than the thrashing this prevents.
- Deliberately two lines of defence with the handheld-side avoidance below: the beacon is an estimate and will sometimes be wrong, and the avoidance cannot help a handheld that has not tried yet.

### Fixed (D54's load balancing made a broken AP the most attractive one)
- **An AP that refuses every handheld is the emptiest AP on the grid, and D54 rewards emptiness.** Everest ran out of heap, its Wi-Fi refused every association with reason 5 (`ASSOC_TOOMANY`) while its beacon advertised "0 attached, 15 free" — true of its own tables, a lie about what it could do — and the balancing score made it the first choice of four of six handhelds, over Fuji and Denali, which were healthier *and stronger*: 948 against 860 and 856. They were refused, waited ten seconds, and chose it again, for the whole run. **The emptier it got, the more attractive it became.** One AP's memory problem became four handhelds offline, on a grid whose first claim is that it has no single point of failure — and nothing logged an error, because every part was behaving as designed.
- **A handheld now passes over an AP that refused or dropped it**, doubling from 15 s to a 2-minute cap, cleared the moment a session with it actually works. The penalty is sized to lose to any healthy AP while staying far above the backbone and grid-time disqualifiers, so an avoided AP that is the **only** one left is still chosen: being briefly unwanted must never become being unreachable. Applied after D54's bonus rather than beside it, because it is the correction to that bonus.
- What this says about D54, written into `docs/chaos-engineering.md`: load balancing must never be the whole of a choice, because **"empty" and "broken" look identical from the outside**.

### Fixed (a stack overflow in the core task, with no fault injected)
- **`lg_core` overflowed its 8 KB stack** during a chaos run — `***ERROR*** A stack overflow in task lg_core has been detected`, a different crash from the watchdog above. The path is deep and every level carries a kilobyte of `LG_FRAME_MAX`: a backbone frame arrives and is decrypted, is forwarded (building a new frame), may be handed to a handheld (building another), and the sealing at the bottom is mbedtls's PSA AEAD, whose own frames are not small.
- **The frame buffer in `backbone.c`'s `handle_rx` is now static**, matching what `lora.c`'s `deliver()` already does and for the same reason — only the core task reaches it and it is not re-entrant — which takes a kilobyte off the deepest part of that path. The stack goes to 10 KB rather than 12 KB as a result. It is still 2 KB of heap an AP has little of, and that trade is deliberate: a stack overflow is not something to leave in place to protect a memory figure.

### Measured on the bench (after the three fixes above)
- **Everest went from crashing repeatedly to flat and stable.** Before: heap decaying to 3 KB, minimum free **400 bytes**, task-watchdog panics and a stack overflow with no fault injected. After: 199 s with no crash of any kind, heap flat at 10 KB, minimum free **5796 bytes**, largest block 6912, links 2. Breaking the watchdog loop is what stopped the join storm that was destroying the heap — the crashes were the cause of the memory collapse as much as a symptom of it.
- **All three APs: zero crashes** across a full run where the previous run crashed AP 0 three times.

### Known, not yet fixed (the shortage underneath all of it)
- **Everest runs out of memory, and it is the only AP with a GPS.** It boots with 14–16 KB free against 22 KB on Fuji and Denali — a 1 KB UART buffer plus the GPS work — and then decays to 3 KB with a minimum of 400 bytes, where the other two sit flat at 18 KB with a minimum of 6–9 KB from t=81 s to t=488 s. It is not a leak in `gps.c`, which allocates nothing per sentence; it looks like a vicious circle — less headroom, so lwIP cannot allocate, so sends return `EAGAIN`, so the core task waits, so the watchdog fires, so it restarts and every handheld piles back on. The send-budget fix above breaks the watchdog link in that chain; **it does not create the missing memory.**
- What is left is a genuine trade-off about what an AP must carry: Wi-Fi AP, ESP-NOW, BLE (D68 and D70), LoRa, a web server and a GPS on one ESP32. That is the owner's call about the product, not something to tune away quietly, so it is recorded here rather than guessed at.

### Added (D80: a handheld says it is reachable over LoRa before it needs to)
- **`lg_client_lora_announce`**, and the handheld sends one every two minutes while Wi-Fi is not carrying. `lg_client_ping` refuses unless `connected`, which is right for a socket and wrong for this radio: on LoRa there is no session and never will be (D77), so "not connected" is the normal state and exactly when speaking matters. Without this an AP learns of a handheld the first time it speaks — so the first thing it ever says is the thing that mattered, and the acknowledgement waits on the AP working out where to send it.
- **PING and PONG join D77's allow-list**, and nothing else does. PING is the smallest frame in the protocol, the AP already answers PONG (so the handheld learns the path works both ways), and it carries the battery the status beacon shows (D68). It asserts nothing about the grid, which is the test every type on that list has to pass — the refusals are the ones where an AP would have to *trust a handheld*.
- **Skipped when no AP has been heard recently**, because transmitting at nobody is what a battery can least afford; the first announcement after a session goes away is sent regardless, or a handheld carried out of range would never speak first.
- **The Status screen shows the radio** (D23): module and address, when an AP was last heard with RSSI and SNR, frames in and out, and a trouble row **only when there is trouble**, because a row of zeroes every time teaches people to skip it. A board with no connector shows nothing at all; one with a connector and no module says so, since "no module answered" and "a module that hears nothing" are different faults. `hh_lora_ui_t` is narrowed from the driver's own counters so a screen never includes `hh_lora.h` (D27).
- **`SLIST_ROWS` 32 → 40.** Status with a GPS fitted already reached 29 rows, and the LoRa section would have overflowed a table whose overflow is an out-of-range shift on the dirty-row mask. That mask is now 64 bits wide with the assertion to match.

### Added (handheld LoRa is alive on real hardware)
- **The Freenove's LoRa pins were wrong, and not merely unconfirmed.** The profile proposed GPIO40/41/42; **GPIO40 and 41 are the micro-SD CMD and D1, and GPIO42 is the WS2812 LED**, none of which is exposed on any connector. They looked free because this profile has no SD card and no LED — the mistake was choosing pins the *profile* did not use rather than pins the *board* did not use, and the only symptom was one line saying no module was found, which is indistinguishable from no module being fitted. I read that line as a fact about the hardware and told the owner no module was present. It was.
- **Confirmed on a module** (owner's wiring): the board's second 4-pin **IO** connector carries IO2, IO3, IO14, IO21 — module TXD → **GPIO21**, module RXD → **GPIO14**, reset → **GPIO2**, with 3.3 V and GND taken from the **I2C** connector because the UART connector's only supply pin is 5 V and this is a 3.3 V part drawing ~140 mA transmitting. GPIO3 is left spare on purpose: it is an ESP32-S3 strapping pin the chip samples at boot, and a module announcing itself at power-up would drive it at the worst moment. The GPS keeps the UART connector (D65) unchanged.
- On a board: `[LORA] Module on UART2 (rx GPIO21, tx GPIO14), address 101` then `Configured RYLR998_REYAX_V1.2.4: 868500000 Hz, 22 dBm`, on **both** Freenove handhelds (addresses 101 and 109).

### Fixed (the announcement deadlocked, and the first one was always wasted)
- **A handheld that had never heard an AP would never announce itself.** The guard skipped announcing whenever no peer was known, to save airtime — but an AP learns of a handheld *only* from an announcement, so no peer meant no announcement meant no peer, for ever. Observed on a board: one announcement at boot, then silence through a whole Wi-Fi-off window. **Having no peer is the case this exists for.** A handheld that has never heard an AP now announces at the normal rate; the backing off (every fourth interval) applies only once an AP has been heard and then gone quiet.
- **It also announced before the module was configured.** The RYLR998 answers about 20 ms before it has taken its frequency and spreading factor, and a frame offered in that gap is dropped with no counter — which is what happened to the first announcement on both boards. It now waits for `configured`, not merely `fitted`.
- Verified over a 280 s Wi-Fi-off window: announcements at 2.3 s, 122.3 s and 242.3 s, and `[LORA] Link up to AP 2, RSSI -13 dBm, SNR 11` — a handheld and an access point on the same radio for the first time.

### Known, not yet fixed (handheld to AP is one-way)
- **An AP's frame reaches a handheld; a handheld's does not reach an AP.** The handheld sent 3 frames (1527 ms of airtime, `frames 3 out`) and received one from Everest; both APs report `nothing received yet` with `parts in 0` — zero at the raw `+RCV` level, so their modules heard nothing at all. Not a configuration mismatch: both sides report network 14, 868500000 Hz, SF9 BW125 CR4/5, and the APs have broadcast reception on, which is what `AT+SEND=0` needs. The handheld's module plainly works in both directions with the ESP32 (it takes AT commands and returns `+OK`). Next: watch an AP's raw AT stream during a handheld transmission, and check whether an AP busy with its own backbone traffic is simply not listening when the handheld speaks.

### Added (`lora pins <rx> <tx>`: finding a module without reflashing a guess)
- A board profile's LoRa pins are a guess until a module has answered on them, and the only symptom of a wrong guess is one line saying no module was found — **indistinguishable from no module being fitted**, which is exactly the wrong conclusion to draw. The new console command re-points the LoRa UART at runtime and makes the driver look again: the ESP32-S3 routes any UART through the GPIO matrix, so pins are a property of an open port rather than of the install, and the reader task already looks on every pass (that is how a module plugged in later is noticed).
- Refuses pins the board must never drive — GPIO33–37 are the Freenove's octal PSRAM and 19/20 are USB — because driving those is a crash or a dead console, not a bad reading. Saves nothing: a board profile still decides this at boot.

### Not yet verified
- **The Freenove boards do have RYLR998 modules fitted (owner, 2026-09-30); the firmware cannot find them.** An earlier entry in this file said no handheld had a module, which was wrong — it restated a log line (`No module on GPIO40/41`) as a fact about the hardware. What is established: the profile's proposed GPIO40/41/42 were never confirmed on a module; the board's only 4-pin UART connector is GPIO44/43, the ESP32-S3's U0TXD/U0RXD, which is why the board brings them out; while the GPS held GPIO43 the LoRa port was **refused** it (`GPIO 43 is not usable, maybe used by others`), so that pairing had never actually been tried. Giving the connector to LoRa with the GPS switched off made the pins genuinely free, and **no module answered AT at 115200 in either wiring order**; an eleven-pair sweep of the board's free header pins found nothing either. The GPS keeps the connector, because giving it up bought nothing. What remains to check is physical: which pins the module's TXD and RXD actually reach, and what feeds its power pin — the connector's fourth pin is 5 V and a RYLR998 is a 3.3 V part.
- **No handheld has a LoRa module fitted** *(wrong, and twice corrected: the modules were fitted and the pins were wrong. Kept so the correction is legible.)* The Freenove logs `No module on GPIO40/41` and every other board has no connector, so the announcement, the Status rows and the whole handheld side of D77/D80 are verified by compilation and by the simulator only. The APs' radios are real and linked (addresses 1, 2, 3, peers seen both ways).
- **The chaos endpoints are registered but have not been called.** Testing them means joining the grid's Wi-Fi from this laptop, which is the owner's network connection to decide about; the bug that mattered — that they were not registered at all — is fixed and confirmed on a board.

### Added (D81: an access point can be reflashed over Wi-Fi, because it is losing its cable)
- **`POST /api/update` on the admin page each AP already serves.** The bench cannot power the access points: 28 brownouts per AP in 18 s, every one after `heap_init` and before Wi-Fi is initialised, so they are moving to battery and their serial cable goes away with the bench. Without this an AP on battery has frozen firmware until it is physically carried back — with none of the LoRa work yet proven on a radio, that is the wrong thing to be certain about.
- The image is taken in **1 KB chunks** rather than buffered whole, because an AP has 40-something KB of heap and a node image is over a megabyte. `esp_ota_end` validates before anything becomes bootable, so a truncated upload is refused rather than bricking the board.
- **Rollback is on** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): a new image must reach `esp_ota_mark_app_valid_cancel_rollback()` or the bootloader puts the previous one back. This is what makes a remote flash survivable — an image that crashes before Wi-Fi is up would otherwise be unrecoverable on a board nobody can reach.
- **A new 4 MB OTA partition layout** (`firmware/common/partitions_4mb_ota.csv`): `otadata`, then `ota_0` and `ota_1`, which **moves `lgid` to 0x320000**. `tools/flash.py` now resolves each project's CSV from its own `sdkconfig.defaults` instead of a hardcoded offset — it would otherwise have written identities where the old layout kept them and destroyed the device IDs D75 exists to protect.

### Added (D82 and D83: chaos without a cable, and a journal for what it did)
- **An AP can break itself** (`chaos_self.c`, `CONFIG_LG_NODE_CHAOS`, off by default): it takes its own backbone down, takes its own LoRa down, and restarts itself. `tools/chaos.py` injects every fault from the PC side of a serial port, so on battery the framework could still *watch* the grid but could not *cause* anything.
- **The fault budget is counted in NVS and written before each fault**, not in RAM — a restart is one of the faults, so a counter in memory would let an AP restart its own chaos run forever.
- **Deterministic, not random:** the schedule comes from (compiled-in seed, AP index, boot counter) through a plain LCG rather than the hardware RNG, so a chaos night is replayable from its report, and seeding by AP index keeps the three APs from taking themselves out at the same moment.
- **Armed over the network with a dead-man window** (`POST /api/chaos`): the window is an absolute deadline in **grid time kept in NVS**, capped per arming, and **arming is refused when the grid has no time** — without a clock the deadline cannot be enforced and the AP could not promise to stop. Losing contact therefore *ends* a run rather than sustaining it. Deliberately not on BLE: D70 says a watcher can never take part, and Wi-Fi plus the admin password is two secrets where BLE would be one.
- **A 64 KB `chaoslog` journal** of packed 16-byte records (D49), read back through `GET /api/chaoslog`. Its **read path is in every AP build**, because a run ends by putting an ordinary image back over the air and that is exactly when it is read. It is begun only on the first boot of a run, never on the restart a fault causes, or the evidence from before the restart would be erased; the count is recovered by scanning for the first still-erased slot; and the last slot records that the journal *filled* rather than another fault, so "the run ended" is distinguishable from "the evidence ran out".
- **`tools/check_release.py` and `docs/TEMPORARY.md`:** every one of these concessions is registered with why it exists, what it costs, and what must become true before it comes out, and the checker **fails on a release tag** while any of it is in the tree. CI reports it on every push. A note in a document is a reminder; this refuses.
- **`docs/chaos-engineering.md` retracts its own claim.** That document said the control surface "exists only on the physical USB cable". It does not any more, and the section now says so, why it changed, and the honest accounting: the chaos endpoints grant no capability D81's upload did not already grant — an uploaded image can do anything chaos can — they lower the effort, which matters for accidental misuse rather than for attackers.

### Fixed (my own release checker printed one row backwards)
- `check_release.py`'s last row warns if the README has stopped calling this a prototype, which is the opposite polarity from the concession rows above it: findings there mean "still present". It printed that row with the same present/gone wording, so a README that correctly **does** say prototype was reported as `gone`. It now reads `ok` or `WARNING`. Caught by reading the tool's own output rather than trusting that it passed.

### Not yet verified (D81, D82, D83)
- **None of it has run on hardware.** Two of the three APs still brown out before Wi-Fi initialises, so no image has been uploaded over the air, no fault has been injected, and no journal record has been written. What is verified is that it compiles and that the chaos strings are absent from an ordinary image.
- **Chaos cannot cut its own power**, so a battery run loses the power-out fault the USB harness had. That is a real reduction in coverage, not a detail.
- **A wireless run is less informative than a USB one.** All three bugs the ten-hour night run found came out of serial logs. The journal is a partial replacement, not an equal one.

### Added (the AP side: a handheld's LoRa frame is opened, checked, and answered)
- **`deliver()` routes on the outer header's key-epoch byte** before any AEAD work: the backbone writes 0 there and a handheld writes 1. That is not only cheaper — handing a handheld's frame to `lgbb_open` would fail authentication and be counted as a **seal failure**, so the admin page would have shown a climbing fault count for perfectly ordinary traffic.
- **`deliver_from_handheld`** opens the frame with the handheld key, refuses anything not from a handheld, applies D77's allow-list (this is the side that must not be talked into rewriting the grid), and hands it to the core as a session frame with a pre-set `session_device` — which is what lets a handheld with no Wi-Fi session be heard at all. It marks presence **before** handling the frame, so an acknowledgement produced while handling it already has somewhere to go.
- **`lg_node_mark_lora_reachable`**, with two deliberate refusals to act: a handheld that already holds a Wi-Fi session here stays ONLINE, because that says more than reachable-over-LoRa and hearing it on the radio must not quietly downgrade a working session; and one already known this way announces nothing, so hearing it repeatedly costs no airtime. `lg_node_presence_state` is the read-only query the glue uses to pick a transport — which is why the core needed no new send callback and stayed portable.
- **`lora_offer_handheld` and `queue_sealed_hh`** seal with the handheld key rather than the backbone key, because a handheld cannot open a backbone-sealed frame and must never be sent one. Both the send policy (D74) and the allow-list are applied before any airtime is spent: an AP should not spend a second of it sending a handheld something the handheld's own side would refuse to open.
- **`io_to_client` picks the transport from the same presence state the core used** — a session over Wi-Fi as before, the radio for a handheld known only over LoRa. One door, two transports.

### Fixed
- `firmware/node/main/CMakeLists.txt` did not compile `firmware/common/lg_lora_hh.c`, so the node failed to **link** (not compile) once `lora.c` started using the sealing layer. Files under `firmware/common` are pulled in per project: a project that includes the header must also build the source.

### Not yet verified
- **Nothing above has moved a byte over a radio.** It is verified by compilation and by the simulated D80 scenarios, which exercise the core's presence and delivery logic — not the AT layer, not the part framing over a real module, and not the pins. No RYLR998 has been fitted to a handheld, and the F4B's LoRa pins (GPIO 40, 41, 42) are still proposed from that board's free pins rather than confirmed.
- **`deliver_from_handheld` itself has no test.** The simulator has no LoRa transport, so the AP's open/allow-list/mark path is untested glue; the `lglh_*` layer beneath it is covered by `test_lora_hh.c`, and the presence behaviour above it by `test_messaging.c`, but the seam between them is not.

### Added (D80: a handheld reachable only over LoRa has its own presence state)
- **`LG_PRES_LORA` = 2 in `PRESENCE_UPDATE`.** When an AP hears a handheld over LoRa (D77) that handheld has no Wi-Fi session, so calling it ONLINE would tell every other handheld it could be reached that way and send traffic down a path that cannot carry it; recording nothing instead would mean no acknowledgement ever goes back and the sender never learns its SOS landed. The third state says what is true: alive, reachable, and only over LoRa. The record's length is unchanged, and `node` still says which AP heard it, so a reply knows where to hand the frame.
- **The core decides *that* a device is reachable; the glue decides *which transport*.** `io.to_client` stays the one door and the firmware reads the same presence state to pick a session or the radio, so `lg_core` gained no new callback and stays portable. A new `is_local_reachable` is used by direct delivery, by the **broadcast fanout** (an urgent broadcast or SOS reaching a handheld out of Wi-Fi range is the whole reason the radio exists), and by the 1:1 validation that had been refusing such a message before delivery was ever reached. Airtime is not decided in the core: the send policy in the glue (D74) carries alerts always and ordinary traffic only when Wi-Fi cannot.
- **Voice deliberately still requires `LG_PRES_ONLINE`**, because one 100 ms frame is seconds of airtime (D71). A test asserts voice *stops* once a handheld is LoRa-only, so widening that later fails loudly rather than quietly flooding the radio.
- **Five scenarios in `test_messaging.c`**: the wire format (length unchanged, state 2 round-trips, 0 and 1 still decode, 3 and 0xFF still refused, wrong length still refused); a 1:1 message two hops to a LoRa-only handheld, asserting the acknowledgement comes back and the plaintext never crossed the backbone; an urgent broadcast and a group message, which take a different branch from direct delivery; a duplicated backbone delivering once and still acknowledging; and voice refused. **2342 checks, 0 failures on the bench** (up from 2266).

### Fixed (the new tests caught the change being incomplete)
- Widening delivery alone did nothing for 1:1 text: `lg_node.c` refused a direct message with `LG_ACK_REJ_OFFLINE` when the recipient was not exactly ONLINE, and that check runs *before* delivery. Broadcasts worked throughout because they never pass through it — so without the tests this would have shipped as "an SOS broadcast reaches an out-of-range handheld but a direct message to them is refused", with a clean compile the whole way.

### Changed
- **`lg_presence_dec` accepts state 2 and still refuses anything above it.** This makes D80 a **breaking wire change: every AP and handheld in a grid must be reflashed together.** A device built before it does not read state 2 as "not online" — it rejects the whole presence record as malformed and keeps no presence for that handheld at all, so a half-updated grid loses presence records rather than merely losing the new state. Close to normal here, since the grid's secrets are compiled in and every board is already built from one tree, but worth knowing before a partial flash.

### Fixed (esptool was left to guess the chip, and on one board it guessed wrong)
- **Every `tools/flash.py` esptool call now names the chip.** The wrapper passed no `--chip` and relied on auto-detection. On the E40 (LCDWIKI E32R40T) that detection is not reliable: `--erase` failed twice with `erase failed`, and once the erase had been done by hand the run then failed with `identity write failed`. The app flash *between* those two steps succeeded every time, because it goes through `idf.py`, which always passes the chip — which is what pointed at the wrapper. Running the same erase by hand with `--chip esp32` worked first time, and with the fix the identity write succeeded immediately. The device map already records a target for every board, so there was never a reason to guess. The chip-*detection* call is deliberately left without `--chip`.
- This was a latent trap on any board with marginal detection, not a quirk of one: it could leave a board erased and half-provisioned, reported as a failure whose message named the wrong step.

### Added (finding what is on the bench, rather than trusting the map)
- **`tools/sync_ports.py`** asks every serial port for its device ID and rewrites only the `port` hints in `tools/bench_devices.json` to match. Ports on this bench shuffle whenever the hub is reseated or a native-USB board resets, and a stale hint is what sends a flash run at whichever board now holds that number — which is how a board came to carry another's identity (D75). It reuses `flash.py`'s own `query_id()` and `save_map()`, never invents a map entry for an unknown ID, and clears the hint for a board that answered nowhere. A port that answers nothing is reported as such, which is how a board running another project's firmware looks.
- Recorded in the `flash` skill: **an `--erase` takes the touch calibration with it.** On a resistive panel (XPT2046: HY3, E28, CYD, E40) the handheld then waits on its calibration screen and never reaches the launcher by itself, so `flash.py` reports `no '[UI] Launcher ready' within 30 s` for a board that is working perfectly. The failure table now says how to tell that apart from a real fault: `[GRID] Registered with node N`, `[TEST] Self test: N checks passed` and `[UI] Touch XPT2046 ready, needs calibration` together mean the flash worked.

### Added (the handheld Wi-Fi switch has a self-restoring form, for chaos)
- **`wifi off <seconds>`** on a handheld is a chaos hook in the APs' own style: capped at an hour so a typo cannot take a handheld off the grid for a day, deliberately **not** saved, so a restart ends it too, and back on by itself on the service task's own clock even if whatever asked for it never returns. A bare `wifi off` stays the sticky field-test switch that survives a reboot. The first version of D78 had only the sticky form, which would have let a chaos run that died leave a handheld dark — the same mistake `lora off` and `bb off` were built to avoid.
- This is a fault the framework could not previously inject: **losing a handheld's network without resetting the handheld.** `docs/chaos-engineering.md` records the new hook, the deliberate split between its two forms, and — in "What it still cannot find" — the gap it does **not** close: LoRa rounds still only break the link between access points, because that is the only place LoRa runs. The handheld side has its key, its pins and its tests but no driver, so the framework cannot yet ask whether a handheld out of Wi-Fi range still reached the grid.

### Fixed (the first run of the new suite on a board caught the test, not the code)
- `test_lora_hh.c` built a `LG_T_MSG_ACK` with `LG_SCOPE_SYSTEM`, but `lg_env_decode` requires a message acknowledgement to be 1:1, so the frame was invalid and `lglh_type_allowed` refused it exactly as it should. The test was wrong and the production code was right. Also fixed in the same suite: an all clear now carries `LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR`, as `hh_service_send_all_clear` actually builds it (D66, D79). Verified on the bench: **2266 checks, 0 failures, min free heap 27608 bytes** on the Hosyond. The compile had been clean and said nothing about either — which is the argument for D25.

### Changed
- **The LoRa radio settings and the address plan have one home.** They lived only in `firmware/node/main/lora.h`, which the handheld firmware cannot include (D27), so the handheld header first copied all seven of them. Two copies means a handheld silently on a different channel from the grid after a one-sided edit — a bug found on a field trip, not at a desk. They now live in `firmware/common/lg_lora_wire.h`, the file both firmwares already compile; `lora.h` keeps its own macro names, defined from the shared ones, so no AP code changed.
- `tools/bench_devices.json`: the Hosyond's port hint corrected from COM30 to COM36, where the board actually answered.

### Added (D79: a handheld can mark itself safe)
- **"I'm safe" on the touchscreen handhelds.** The alert unit (D66) could always stand its own SOS down, and `hh_service_send_all_clear()` had existed in the handheld service all along — but `ui_unit.c` was its only caller, so a touchscreen handheld could raise an urgent broadcast and then had no way on any screen to take it back. It could show an incoming ALL CLEAR and not send one, which left a person able to fire an SOS and no way but the serial console to stand it down. The broadcast conversation now carries an **I'm safe** key beside the urgent key. It appears only while this handheld's own urgent broadcast is standing (the newest of our own urgent broadcasts decides, and an all clear is itself one, so saying it once puts the key away), it asks first in the same confirm panel the urgent key uses — in the calm accent rather than the alarm red, reading "This stands your SOS down on every screen." — and it sends "<name> is safe", the same words the alert unit sends, so the two kinds of device read alike on everyone else's screen.

### Added (D78: a handheld can switch its Wi-Fi off, to test LoRa in the field)
- **`wifi [on|off]` on the handheld console.** The radio is **stopped**, not merely left unassociated: a station that keeps scanning still spends the power and still fills the air with probe requests, so "Wi-Fi off" that left it running would prove nothing about what LoRa had carried. The setting is kept in NVS, so it survives the restart a field test brings, and it is not a fault: the status snapshot carries `wifi_off` and the link reads SEARCHING with an empty `problem`, so a screen can say the radio is off on purpose rather than "no AP in range". Switching off leaves the GPS, the battery and anything LoRa carries running; on a board with no LoRa connector the console says out loud that this leaves the handheld with no radio at all, and does it anyway, because that is also how you measure idle battery life.
- The switch lives in the service's **own** NVS namespace rather than the board-support settings store, because `tools/check_layers.py` correctly refused the first attempt: the handheld service must not include a board-support header (D27). The board's LoRa pins reach the service the same way the GPS pins do, from the board profile through `hh_main`.

### Changed
- **D78 revises D72's voice-note ceiling for LoRa only**: about 10 seconds, not 30. The bench had already measured that D72 could not ship as written — with the 6 KB reassembly and send slots on, APs that ran at 22-28 KB free fell to 9-12 KB and MAIN reached 0 — so a LoRa note is capped at roughly 1.5 KB at Codec2 1200 bit/s, 12 parts, about 11 s of airtime and ~3 KB of slots on an AP. Over Wi-Fi the note is still the full 30 s. Live push-to-talk over LoRa remains impossible and is not attempted (D71). D72's licence blocker is cleared: Codec2 is LGPL-2.1 and D76 made LocalGrid GPL-3.0-or-later, which carries it. **None of the voice-note work is built yet** — this entry records the decision and the measurement behind it.

### Added (D77: handhelds get LoRa, sealed by a key of their own — foundation)
- **The key question D71 left open is answered.** A handheld does not hold the APs' backbone key, so nothing could seal a handheld's LoRa frame. `tools/gen_secrets.py` now derives `LG_SECRET_LORA_HH_KEY` = HKDF-SHA256(backbone key, `"lg-lora-hh"`), 32 bytes, held by handhelds and APs alike and deliberately **not** the backbone key: a handheld image that leaks exposes the handheld LoRa path and nothing else. `--update` adds it to an existing `lg_secrets.h` without touching the keys already there, so the bench grid does not have to be re-keyed; the generator's HKDF is now a general function and every derived value is declared in one table.
- **`firmware/common/lg_lora_hh.c`**, the sealing layer, portable C11 over `lg_core` and `lg_crypto` only. Same 12-byte outer header shape as the backbone's, used as both AEAD nonce and associated data, with the **key-epoch byte separating the two key domains on the air** (0 backbone, 1 handheld): an AP feeds every LoRa payload to both readers, and a frame sealed with one key can never authenticate under the other. The sender field carries a kind bit, so handheld 3 and AP 3 keep separate nonce and replay spaces. Its own replay window, its own counters, and no allocation anywhere.
- **A closed list of what a handheld may send over LoRa**, checked by the AP on every frame it opens: position, presence, short text (which carries an SOS, an urgent broadcast and an all clear) and a message acknowledgement. Registration, grid state, groups, names and time are refused however well sealed, because they are what an AP would otherwise have to *trust a handheld about* and every handheld holds this one key. Live voice is refused twice over, by this list and by the send policy.
- **`tests/target/main/test_lora_hh.c`**, a new suite with no radio needed: the seal round trip, a replay caught, a tampered tag counted as an authentication failure, a frame from the other key domain recognised as *not mine* without counting a fault (so the admin page does not cry wolf on ordinary backbone traffic), our own identity coming back at us treated as malformed (D75), a handheld and an AP of the same index not retiring each other's frames, and every allowed and refused message type. The handheld's on-screen test report (D23) gains the row, which its hardcoded suite count would otherwise have silently dropped.
- **The board profile carries LoRa pins**: `lora_rx`, `lora_tx`, `lora_rst` on every profile. Only the Freenove FNK0104B (F4B) names them — the one family with a second free UART, the RAM for LoRa beside a GPS, and the S3's GPIO matrix. GPIO 40, 41 and 42 are proposed from that board's free pins and **not yet confirmed on a module**. The APs keep choosing theirs by AP index, because all three run the same board.

### Changed
- **`firmware/node/main/lora_wire.{c,h}` moved to `firmware/common/lg_lora_wire.{c,h}`.** It was already portable C11 with no radio in it, but it sat inside the node project where the handheld firmware could not reach it. Both firmwares and `tests/target` now compile the one file; no behaviour changed.

### Fixed (the diagram showed a LoRa antenna on MAIN but no LoRa link to it)
- The drawing had one orange arc, from the westmost access point to the eastmost, sweeping below everything, while three access points carried a LoRa antenna symbol. MAIN had a module and no link, so the arc read as decoration. It now draws the actual topology: MAIN links over LoRa to both outer access points, and those two link to each other, so the three arcs and the three antenna symbols agree. The label says "LoRa: the three APs that carry a module link to each other directly", and MAIN's own label is "GPS + LoRa" rather than "GPS + LoRa fitted", which read as not working.
- **Labels no longer sit under the lines.** The new LoRa arcs leave MAIN downwards instead of sideways, clearing the label block, and every floating label ("MAIN", "GPS + LoRa", "watching over BLE", the GPS and LoRa captions) is drawn with a dark halo so a link passing behind it cannot eat the words.

### Changed (the README and the site tell the truth, and show the network)
- **A README worth landing on**: the network diagram at the top, badges (chaos tested, licence, ESP-IDF, prototype), what the three radios each do, a **Tested by breaking it** section with the run's numbers and the three bugs in a line each, and an honest **Status** listing what is not done — purpose-built GPS and LoRa handhelds still being designed, no cases for the access points, no field testing yet, ranges estimated not measured, and no binaries published while keys are compiled in.
- **The network diagram is a file**, `assets/network-diagram.svg`, not markup inside the page: GitHub strips inline SVG from a README, so the site template and the README now show the same drawing from one source. `tools/gen_site.py` copies it.
- **Stale claims removed** from the site and the diagram: LoRa is in use (D74), not "firmware being written"; the handheld link row says six display boards rather than two. The site's "Exploring next" card is replaced by "Tested by breaking it", linking the chaos write-up, and a "Still to do" card matching the README.

### Added (the chaos engineering write-up, with the run behind it)
- **`docs/chaos-engineering.md`**: how the self-healing claim was tested by attacking the grid for hours, and the three bugs that came out of it — the banner timer use-after-free, the presence gap that made a restarted AP refuse valid messages, and grid time that no device could restore. Each with what was wrong, how it was found and what fixed it. Also what the framework randomizes, why the serial console is not a backdoor (physical cable only, no grid-wide changes, self-restoring radio hooks), how the three testing layers divide the work (on-device suite, boot self-test, chaos), what it still cannot find, and what field testing needs.
- **`docs/chaos/2026-09-18-night-run.md`**: the framework's own report from the ten-hour run, kept verbatim with a header explaining how to read it, so every number in the article can be checked against its source. 55 experiments, 55 recovered; backbone whole again in a median 3.7 s; 738 of 782 messages delivered; the 44 that were not are the presence bug. `chaos-runs/` stays gitignored, so this lives under `docs/chaos/`.
- Noted honestly in both: the boot self-test runs on every one of those injected restarts and the framework does not yet check the result, which is the cheapest improvement available.

### Added (D76: public, under GPL-3.0-or-later, with the README, site and CI in the tree)
- **A licence.** `LICENSE` holds the GNU GPL v3 text; LocalGrid is `GPL-3.0-or-later`, the same as the owner's Braino, so work can move between the two projects (D76). Contributions are accepted under it with no contributor licence agreement and no sign-off. `CONTRIBUTING.md`, `README.md`, `THIRD_PARTY.md`, `docs/BOARDS.md`, `docs/lora.md` and the `new-board` skill no longer say the project is closed, and `docs/OPEN_SOURCE.md` records the choice as made rather than proposed.
- **The README, the project site and the CI workflows are in git.** They had been written but never committed, so a public repository had no README at all and the site existed only in one worktree's untracked files. Added: `README.md`, `site/index.template.html` and `tools/gen_site.py` (the landing page with the network diagram, plus the real admin page answering from a simulated AP in the browser), `SECURITY.md`, `CODE_OF_CONDUCT.md`, `docs/OPEN_SOURCE.md`, `docs/ADS.md`, `.github/workflows/ci.yml` and `pages.yml`, and structured issue templates.
- `tools/build.py --budget PCT` fails a build whose app uses more than PCT% of its app partition; CI uses 85 (design review 38). CI depended on this flag, which had never been committed either.

### Removed
- `.github/workflows/build.yml` and `.github/ISSUE_TEMPLATE/new_board.md`, both superseded: `ci.yml` does everything build.yml did and adds the layer check, the tracked-secrets check, the site generation and the partition budget, and its `verify` job is the one required check; `board_port.yml` replaces the board request template with a structured form.

### Changed
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
