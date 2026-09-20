# Grid watch: see the grid from a laptop (D68, D70)

`tools/grid_watch.py` shows how an offline network is doing from any laptop with Bluetooth, without
joining the grid's Wi-Fi. It has two layers:

- **The beacon (D68), always on.** Every AP broadcasts a small sealed status over BLE; the laptop
  listens and shows it on a local web page. This is the live layer: it updates every second and
  needs no password.
- **The admin link (D70), after logging in.** The tool connects to a nearby AP over BLE and asks
  for everything the AP's admin page shows: the map with everyone's coordinates, the groups and
  announcements, the availability history, what happened to each AP, and the traffic and
  performance counters. Every request is read-only (`docs/ble-link.md`).

The page has the admin page's own tabs, wording and colours.

| Tab | What is on it | Where it comes from |
|---|---|---|
| **Overview** | alert banner, AP cards, handhelds, event log, a short "traffic now" summary, and one LoRa line per AP that has a module | the beacon, plus traffic |
| **Map** | everyone's position, on real map tiles when this laptop has Internet, on a drawn plan when it has not | the link |
| **Network** | the APs table, availability over the last 2 hours, and what happened to the APs | the link |
| **Handhelds** | groups, who may announce, and every handheld the grid has seen | the link |
| **Traffic** | message rates and totals by class, drops and faults, backbone links, the LoRa backbone, handheld sessions, and each AP's performance | the link |

Each pulled section says which AP answered, **by name**, and how long ago. **Refresh** asks again
now. Without the admin password the page shows the beacon only, and the other tabs say so.

- **An alert banner:** an active SOS or urgent message (who, near which AP, how long ago, read by
  how many), or "all clear" when the sender marks themselves safe.
- **Each AP:** heard or not and signal strength, uptime, backbone links to the other APs, handhelds
  connected, time source (GPS, carried, or unset), GPS fix and satellites, lowest free memory,
  restarts, brownouts, and why it last restarted.
- **Each handheld:** chosen name, online or not, which AP it is on, and battery.
- **An event log:** APs lost and heard again, restarts, alerts, time source changes.

Positions are never broadcast in the beacon: they only cross the admin link, after a login.

## The admin password

The password is typed into the page, which posts it to this tool over `127.0.0.1`. From it the tool
works out the PBKDF2 hash the AP stores and sends only an HMAC proof of it over the link
(`docs/ble-link.md`). The password is:

- never written to disk, the event log, or the console;
- never in `state.json` or any page the tool serves;
- never sent to the AP — the AP receives a proof, and a fresh challenge makes each proof single-use;
- forgotten when you press **Log out**, or when the tool stops.

Logging out also hides everything the login unlocked, positions first. A wrong password is refused
by the AP, which rate-limits attempts exactly as its admin page does.

## The map, online and offline

The grid has no Internet. The watching laptop may, and the Map tab uses it if it is there:

- **With Internet:** OpenStreetMap tiles, with a marker for every AP and handheld. Tiles are
  fetched by this tool (not by the browser) so it can send a proper `User-Agent` and take only the
  squares the open map needs — no API key, no bulk downloading, nothing written to disk.
- **Without Internet, or when tiles fail:** a plan drawn on this laptop, with everyone placed
  relative to MAIN, north up, with distances and bearings, plus the coordinates as text and a
  copyable link to open the position in a map later.

**No coordinate ever leaves this laptop.** A tile is asked for by its z/x/y grid square, there is
no query string, the page sends no referrer, and the map link is text to copy, not a link the page
follows. `--no-map-tiles` switches tile fetching off altogether; the plan is then always drawn.

## Traffic and performance

The Traffic tab shows, per AP and totalled across the APs it has reached: message rates now and
over five minutes, totals by class (1:1, group, broadcast and urgent, voice in/out/dropped,
presence, announcements, positions, time, acks), drops and faults, per-link frames, bytes and
failures, handheld sessions and bytes, and the AP's own heap, queue, stack, loop time and radio
errors. Anything over a sensible limit is red and says in words what it means ("MAIN is dropping
voice frames: push-to-talk will sound broken").

**Counts and sizes only.** No message text and no audio exist on this link at all.

### The LoRa backbone (D71)

Each AP's card on the Traffic tab ends with a **LoRa backbone** panel. It says one of three things,
and never confuses them:

- **"No LoRa module on NORTH."** The AP answered and has no module. It works exactly as it does
  without one, and the other APs are unaffected.
- **"SOUTH said nothing about LoRa."** The AP is running firmware from before D71. The record it
  sends simply stops after its backbone links, and the tool says nothing about a radio it was told
  nothing about.
- **A fitted module**, with everything below.

A fitted module leads with the number the admin page leads with, because it is the one that says
whether the radio earns its keep: **frames that arrived by LoRa and that Wi-Fi had not already
delivered** ("19 of the 388 frames that arrived by LoRa got here first, before Wi-Fi had them"). If
that rises while the Wi-Fi backbone is broken, LoRa is doing the job it was fitted for.

Then the module and its settings — configured or not, address, network ID, SF9/BW125 at 22 dBm,
and whether it broadcasts to every AP at once or sends one transmission per peer — a note when
there is no room for long payloads, and a red line when a chaos hook has the radio switched off.

Then **one row per peer**: the link up or down, the last RSSI and SNR, and how long ago that peer
was heard. Then the counters: the last signal heard, frames and parts in and out, retries, parts
dropped, frames given up half-arrived (the 10 s reassembly timeout), frames that could not be
authenticated, the send queue now and at its highest with anything it threw away, payloads refused
for being too large, airtime used, and module restarts.

**What turns red, and what it means.** A peer link down, nothing heard for longer than 95 s (three
missed 30 s heartbeats and a little), a frame that could not be authenticated, a queue that filled
to its four small slots or threw a frame away, and drops or reassembly timeouts well above what a
busy round normally leaves behind. Each one is also said in words above the cards, in the tool's
usual voice: *"NORTH has not heard SOUTH over LoRa for 4 minutes."*

**Where the numbers come from.** Everything except the per-peer signal is in the traffic record,
so it is shown for every AP the tool has counters for. The per-peer RSSI, SNR and age are only in
the AP's own `/api/status`, which the link pulls from the AP it last connected to, so those three
columns fill in for that AP and show `--` for the others.

**On the Overview**, an AP with a module gets one line: LoRa up with the best peer's signal, or
down with which peer it cannot hear and for how long. An AP with no module, or one from before
D71, puts nothing there at all.

These counters are asked for only while the Traffic tab is open (once every 30 seconds) or when you
press Refresh; the rest of the time the tool pulls status and history once a minute, one short
connection at a time, so a small AP is left alone.

## What you need

- This repository, with the **same `firmware/common/lg_secrets.h` your APs were built from**. The
  tool reads the grid key and grid ID from it. Each grid makes its own with
  `python tools/gen_secrets.py`; the example file's all-zero key is refused.
- Python 3.10 or newer and two packages:

      pip install bleak cryptography

- A laptop with Bluetooth, within BLE range of at least one AP (tens of metres, less through walls).
- APs running firmware with D68 (the scan-response status). Older APs still show as heard, with
  "discovery advert only". The tabs beyond Overview need an AP with D70 (the admin link) and the
  admin password.

## Run it

    python tools/grid_watch.py

The page opens at `http://127.0.0.1:8768/`. It exists only while the tool runs; stop it with
Ctrl+C. The page refreshes every second, and the first handheld names and alert state fill in
within about a minute.

| Option | What it does |
|---|---|
| `--port N` | serve the page on another port |
| `--no-browser` | do not open a browser |
| `--secrets PATH` | read another grid's `lg_secrets.h` |
| `--log FILE` | append events as JSON lines |
| `--seconds N` | listen for N seconds, print a summary, and exit |
| `--no-map-tiles` | never fetch map tiles; the Map tab always draws the plan |
| `--self-check` | test the decoders and the link offline against a mock AP; no hardware |
| `--pair` | show the QR code that pairs the Android app (needs `pip install segno`) |

There is no option for the admin password: it is typed into the page, so it never reaches a shell
history or a script.

## What the watcher can and cannot do (owner, 2026-09-19)

It monitors, very well, and takes no part in the grid:

- it never sends a grid message, never joins the grid's Wi-Fi, and has no device ID;
- it never asks for, receives or shows message text — no 1:1 message, no group message, no
  announcement text — because no opcode on the link carries one;
- it never touches push-to-talk audio: voice frames stay on the Wi-Fi side, and the link has no
  audio opcode;
- it cannot change anything, even with the admin password: every opcode is read-only. Settings,
  groups, the time and the password stay on the AP's admin page over Wi-Fi.

## On an Android phone (D69)

The LocalGrid Watch app (`android/grid-watch`, see its README) shows the same page on a phone and
notifies you of an SOS even with the screen off.

1. Install `LocalGrid-Watch.apk` on the phone (allow installing from your file manager or browser
   when Android asks).
2. On the laptop that has this grid's `lg_secrets.h`, run `pip install segno`, then
   `python tools/grid_watch.py --pair`.
3. In the app, tap **Pair** and scan the QR code, then allow Bluetooth scanning, notifications and
   the camera (the camera only for the QR code).
4. Tap **Start watching**. For reliable alerts with the screen off, set the app's battery use to
   **Unrestricted** (Samsung: Settings, Apps, LocalGrid Watch, Battery).

The QR code carries only the key that reads the status, not the grid's secret.

## Security

- **Only this grid can read it.** The status is encrypted and authenticated with a key derived from
  the grid's backbone secret (ChaCha20-Poly1305, `docs/ble-status.md`). A stranger's scanner sees
  random bytes, and nobody without the key can fake an "all fine". Anyone holding `lg_secrets.h`
  can read the status, so share that file as carefully as the grid itself.
- **The admin link is sealed too** (D70): `K_link = HKDF(K, "LG-BLE-LINK-1")`, ChaCha20-Poly1305
  with a full 16-byte tag, a nonce of direction, session and counter, and a connection that closes
  on a bad tag or a counter out of order. Only a paired watcher can even attempt the login.
- **The laptop never joins the grid's Wi-Fi and carries no grid message** (D25). Since D70 it does
  write to an AP's BLE characteristic to ask its read-only questions; that is not grid traffic and
  changes nothing.
- **The page is served on 127.0.0.1 only**, from one self-contained file with nothing fetched from
  the Internet. Other machines on your network cannot open it.
- **No BLE addresses** are printed, logged, or served (D21).

## Platforms

Tested on Windows 11. `bleak` also supports macOS and Linux; on Linux, active scanning (needed to
receive scan responses) can require running with Bluetooth permissions. Windows passes on a scan
response only every few seconds, so frame types arrive slowly but steadily.

## Gaps are usually this laptop, not the grid

Bluetooth shares its radio, and usually its antenna, with Wi-Fi. Measured on the bench: adverts
normally arrive every 0.25 s, but every AP goes quiet together for up to 15 s while the laptop is
busy on Wi-Fi. Because all the APs fall silent at the same moment, this is the receiver, not the
grid. That is why an AP is only called "not heard" after 45 s; the quiet spell is still visible as
"last heard N s ago". An AP that has really gone stops for good, not in bursts.

## If something looks wrong

| You see | Meaning |
|---|---|
| No APs heard | Bluetooth off, out of range, or APs not powered |
| APs heard, "discovery advert only" | the APs run firmware older than D68 |
| "wrong key?" in the event log | this `lg_secrets.h` is not the one the APs were built from |
| An AP not heard for 45 s | it is off, restarting, or out of range |
| Battery blank | that handheld has no battery sense, or runs firmware older than D68 |
| After erasing an AP's flash, its status stops | its boot counter went back; restart the tool |
| "has no admin link service" | that AP runs firmware older than D70 |
| "did not answer in time; it may be busy" | an AP takes one watcher at a time; try again |
| The Map tab shows the plan, not tiles | this laptop has no Internet, or `--no-map-tiles` is on |

## Details `docs/ble-link.md` does not spell out

The client follows `firmware/node/main/ble_link.c` and `traffic.c` where the spec is silent:

- **The service UUIDs** are written as `4c47-0001-…`. The client matches a service or a
  characteristic by those first eight hex digits, whatever tail the firmware uses.
- **The session** arrives first, in the one message that is not sealed (`0x80`, a 4-byte header and
  the session as a `u16`), sent the moment the watcher subscribes to the reply characteristic. The
  client waits for it, builds every nonce from it, and refuses a sealed message that arrives before
  it.
- **`HELLO_OK`** is 59 bytes: version, AP, boot, whether a password is set, the 16-byte salt, the
  iterations, and then the 32-byte login challenge the proof is made from. `LOGIN_FAIL` carries the
  seconds to wait as a `u32`.
- **Chunking:** each chunk is its own sealed message with its own counter and its own 4-byte
  header, with `flags` bit 0 set on every chunk but the last; the client joins the bodies in order
  and keeps its requests inside the negotiated MTU.
- **The `TRAFFIC` record** has no byte table in the spec yet. The layout this tool decodes is the
  one `traffic.c` writes, and it is written out at `decode_traffic` in `tools/grid_watch.py`: a
  24-byte header, 12 bytes per message class in `lg_traffic_class_t` order, ten `u32` fault
  counters, a 28-byte handheld section, a 40-byte performance section, 4 bytes per rate bucket
  (oldest first) and 28 bytes per backbone link, all little-endian. A record in another layout, or
  one counting message classes this tool does not know, is refused rather than guessed at; trailing
  bytes a newer AP adds are ignored.
- **The LoRa section (D71)** is 64 bytes appended after the last link entry, decoded at
  `decode_lora`. It is appended rather than numbered in, so the layout byte stays 1 and a record
  that ends after its links still decodes cleanly — that is an AP flashed before D71, and the tool
  shows no LoRa panel for it. A section cut short is treated the same way, never half-decoded. The
  fitted bit tells "no module here" from "fitted and silent"; an AP with no module sends the
  section with flags 0 and every counter 0.
- **The AP's LoRa address** is `1 + its AP index`, not its index: `docs/ble-link.md` still says
  "its AP index", but `LORA_ADDR_AP` in `firmware/node/main/lora.h` adds one so that address 0
  stays free as the broadcast address, and the bench measured 1-3 on the three APs. The firmware
  wins.
