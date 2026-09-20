# LocalGrid Watch (Android, D69, D70 and D71)

Watch an offline network from an Android phone, without joining its Wi-Fi.

The app listens for the sealed BLE status beacon every AP sends (D68, `docs/ble-status.md`) and
shows what `tools/grid_watch.py` shows on a laptop: the alert banner, each AP's health, the
handhelds heard and their batteries, and an event log. It keeps listening in the background and
sounds a notification when a new SOS arrives.

Since **0.2.0** it can also ask a nearby AP for the bigger picture over a short BLE connection
(D70, `docs/ble-link.md`), behind the AP's admin password: the map with everyone's coordinates,
the groups and announcements, every handheld the grid has seen, the availability of each AP for
the last two hours, what happened to each one, and the traffic and performance counters. The
phone shows the same tabs the AP's admin page shows, from the admin page's own replies.

Since **0.3.0** it also shows the **LoRa backbone** (D71, `docs/lora.md`): whether an AP has a
module, how well it hears the others, and whether the radio is earning its keep. See below.

**A watcher monitors, and nothing else** (owner, 2026-09-19). This holds by construction, not by
politeness:

- **Read-only.** The five requests the app can make are `HELLO`, `LOGIN`, `GET_STATUS`,
  `GET_HISTORY` and `GET_TRAFFIC`. Not one writes, so the phone cannot send, announce, change a
  setting, add a group, set the time or raise an alert.
- **No message content, ever.** Nothing on this link carries a 1:1 message, a group message or an
  announcement's text. The AP has no request that returns a message body, and 1:1 messages are
  sealed end to end between handhelds, so not even the AP can read one.
- **No push-to-talk.** Voice frames never leave the Wi-Fi side. There is no request for audio.
- **Not a member.** The phone has no device ID, never registers with an AP, never holds a grid
  identity key, and never appears on a handheld or the admin page as a participant. It never
  joins the grid's Wi-Fi.
- BLE addresses are never shown, logged or stored (D21). A lost paired phone shows the beacon
  view and nothing else until someone types the admin password.

Being exact about the radio: the phone scans actively, so it sends generic BLE scan requests, and
the admin link means it writes to one AP's BLE characteristic. Neither is grid traffic, and
neither changes anything on the grid (D25).

## The tabs

| Tab | What | Needs the password |
|---|---|---|
| Overview | Alerts, each AP's health, handhelds heard, batteries, events, a traffic-now summary, and one LoRa line per AP that has a module | no (the summary does) |
| Map | Everyone's coordinates: real map tiles when this phone has Internet, a drawn plan when it has not | yes |
| Network | Every AP the grid knows, availability for the last 2 hours, what happened to each AP | yes |
| Handhelds | Groups, who may announce, every handheld the grid has seen | yes |
| Traffic | Rates and totals by class, voice drops, faults, per-link and handheld counters, the LoRa backbone, each AP's heap, queue, stack, main loop and radio errors | yes |

**Refresh** in the top bar asks an AP now. Otherwise the app asks by itself about once a minute
while it is on screen, and every 30 s while the Traffic tab is open. Each section says which AP
answered, by name, and how long ago.

**Battery.** The link is only opened while the app is on screen or when you press Refresh. The
background service goes on doing beacon-only watching and SOS notifications, exactly as in 0.1.0,
and never connects to anything.

## The LoRa backbone (D71)

Some APs carry a second radio — a Reyax RYLR998 on the bench — that reaches kilometres at about
1.5 kbit/s, for when two APs cannot hear each other over Wi-Fi (`docs/lora.md`). **No AP ever
expects one**, and a grid may mix APs with and without, so the app shows LoRa only where there is
something to show:

- **Overview** gains one short line per AP that has a module: up or down, and the best peer's
  signal. An AP with no module contributes nothing at all, and the section disappears entirely
  when no AP has one.
- **Traffic** gains a LoRa panel per AP. It leads with the number that says whether the radio
  earns its keep — **frames that arrived over LoRa that Wi-Fi had not already delivered** — then
  the module (configured or not, its version, address and network, and the fixed radio settings
  SF9 / BW 125 kHz / CR 4/5 / 868.5 MHz / 22 dBm), then each peer's link with its **last RSSI and
  SNR and how long ago it was heard**, then frames and parts in and out, parts dropped,
  reassembly given up, frames that could not be authenticated, queue depth and drops, retries,
  airtime and module restarts.

It goes **red** when a peer link is down, when frames cannot be authenticated, when the send
queue drops a frame or is full right now, when payloads are refused for being too large, or when
a fitted module has heard nothing — each with a line in plain English saying what it means.
Losing parts, giving up on reassembly, resetting a wedged module and a queue that merely reached
its four slots are amber: a LoRa frame is seconds of airtime, so a short queue is normal.

Three states are told apart rather than lumped together, because they mean different things:

| What the app says | What it means |
|---|---|
| "No LoRa module on MAIN" | The AP looked for one and none answered. Not a fault; the AP behaves exactly as it always did. |
| "…has a LoRa module and has never heard another one" | Fitted and silent. Check the antenna, the network ID, and that another AP has a module. |
| "This AP does not report LoRa" | The AP's firmware is older than D71 and sends no LoRa section at all. Nothing is guessed from its silence. |

Two details worth knowing. The packed traffic record carries one last RSSI and SNR for the radio
plus a bitmask of which peers' heartbeats are current — not a line per peer; the **per-peer
signal and the module's version string come from `/api/status`**, which the phone fetches from
one AP, so the full per-peer table appears for that AP and a "peers heard" summary for the
others. And the LoRa section is **appended** to the traffic record rather than numbered into it
(the layout byte stays 1), so nothing before it moved: an AP built before D71 decodes exactly as
it always did, and a record cut short inside the section is read as having none rather than half
of one.

## The admin password

The Map, Network, Handhelds and Traffic tabs show what the AP's admin page shows, so they need
the same password. The app asks for it when you open one of them.

The password never crosses the air: the phone derives PBKDF2-HMAC-SHA256 with the salt and
iteration count the AP sends, and proves it with one HMAC over the AP's fresh challenge. The
whole exchange is sealed under a key derived from the pairing key.

By default the password is held **in memory only** and is gone when the app stops. The
**Remember on this phone** switch stores it in app-private storage sealed with an AES-GCM key in
the Android Keystore — the same protection as the pairing key, with backups and device-to-device
transfer off. **Log out** at the bottom of any pulled tab forgets the password, deletes a
remembered one, and throws away everything pulled with it, coordinates included.

## The map, and the Internet permission

The watching phone often has mobile data although the grid never does. With Internet the Map tab
draws real OpenStreetMap tiles; without it, it falls back to a plan drawn on the phone with
relative positions, distances and bearings from MAIN, north up. Either way the coordinates are
shown as text, and a link you can open later on a device with Internet can be copied to the
clipboard (it is text; the app never opens or fetches it).

This is why 0.2.0 asks for the **INTERNET** permission, which 0.1.0 deliberately did not have.
It is used for **map tiles only**:

- The only address the app ever builds is `https://tile.openstreetmap.org/z/x/y.png` — a tile's
  grid square. No coordinate, no name, no query string, no referrer, nothing about this grid.
  A unit test builds the tiles for real positions and fails if any part of a coordinate appears
  in a URL, and reads the app's own sources to make sure no other URL has crept in.
- Only the squares the open map needs, at most two at a time, nothing prefetched, kept in a small
  memory cache and never written to disk.
- **Grid data never leaves the phone.** The APs, handhelds, positions, groups and counters are
  shown and drawn locally; nothing is uploaded anywhere, and the grid itself never touches the
  Internet.
- The **Map tiles from the Internet** switch on the Map tab turns tiles off entirely. With it
  off the app makes no network request at all.

## Build

Needs the Android SDK (platform 36) and JDK 17 or newer (Android Studio's bundled JBR works).

    cd android/grid-watch
    echo sdk.dir=C\:/Users/<you>/AppData/Local/Android/Sdk > local.properties
    gradlew.bat testDebugUnitTest   # JVM unit tests (100): RFC 8439 vectors, beacon frames,
                                    # link framing and chunking, the login proof, the decoders,
                                    # and the LoRa section with and without a module
    gradlew.bat lintDebug
    gradlew.bat assembleRelease     # app/build/outputs/apk/release/app-release.apk

The tests read vectors in `app/src/test/resources/vectors/`, generated from `tools/grid_watch.py`
— the laptop watcher, which is the reference implementation for this link — with a **test** key
(HKDF over the bytes 0..31), never a real `firmware/common/lg_secrets.h`. The LoRa vectors
(`traffic_lora*.hex`, and the `lora` object in `status.json`) are the same records with the D71
section appended, filled with the bench's own numbers; `traffic.hex` and `status_no_lora.json`
are deliberately left as they were, so every run also proves an AP older than D71 still decodes.

Release signing reads `keystore.properties` beside this README (gitignored, as is `keystore/`):

    storeFile=keystore/localgrid-watch.jks
    storePassword=...
    keyAlias=localgrid-watch
    keyPassword=...

Make a keystore once with `keytool -genkeypair -keystore keystore/localgrid-watch.jks
-storetype PKCS12 -alias localgrid-watch -keyalg RSA -keysize 3072 -validity 10000`. Keep it:
an update installs over the old app only when signed with the same key. Without
`keystore.properties`, `assembleRelease` makes an unsigned APK; use `assembleDebug` instead.

## Install on a phone (sideloading)

1. Copy `LocalGrid-Watch.apk` to the phone (USB cable, Quick Share, or any file transfer).
2. Open it in the Files app. Android asks to allow installing from that app: allow it once.
3. Samsung phones may show "Auto Blocker" or a Play Protect prompt for an app from outside the
   store: choose to install anyway (Auto Blocker must be off to sideload).

Or with USB debugging on: `adb install -r LocalGrid-Watch.apk`. 0.3.0 installs over an earlier version and
keeps the pairing, because it is signed with the same key.

Android 12 or newer is required (minSdk 31), so the app never needs the location permission.

## Pair with a grid

On the laptop that holds the grid's `firmware/common/lg_secrets.h`, run

    python tools/grid_watch.py --pair

and in the app tap **Scan the QR code**, or paste the `LGW1:...` text. The code holds the grid
ID and the status key only (never the backbone secret): a phone with it can read the grid's
status but cannot join, send, or forge anything. The admin link key is derived from that same
key, so a paired phone needs nothing new — only the admin password. Treat the code as a secret
anyway. To revoke every paired phone, regenerate the grid's secrets. **Grid > Forget this grid**
deletes the key from the phone.

The key is kept in app-private storage, encrypted by a key in the Android Keystore; backups and
device-to-device transfer of app data are off. The QR scanner (ZXing, bundled) works offline.

## Permissions and why

| Permission | Why |
|---|---|
| Nearby devices (`BLUETOOTH_SCAN`, `neverForLocation`) | Hear the APs' beacon. No location is derived or requested. |
| Nearby devices (`BLUETOOTH_CONNECT`) | Ask one AP for the admin page's own status, history and counters (D70). Asked only when you log in with the admin password. |
| `INTERNET` | **Map tiles only** (see above). Grid data never leaves the phone; turn tiles off and nothing is fetched. |
| Notifications (`POST_NOTIFICATIONS`) | The ongoing "Watching" notification and the SOS alert. |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE` | Keep listening with the screen off. |
| Camera (`CAMERA`) | Read the pairing QR code; asked only when the scanner opens. |

Not requested: location, `BLUETOOTH_ADVERTISE`, anything about accounts or storage.

## Notifications

- **Watching** (low, silent): "Watching LocalGrid: 3 APs heard", with a Stop button.
- **SOS alerts** (high, alarm sound and vibration): a new active SOS or urgent alert, who and
  near which AP. Change its sound in Android's notification settings for the app.
- **All clear** (normal): "X is safe", when the sender marks themselves safe.

## When an AP will not answer

Every AP carries the same answers, so the app works down the list of APs it can hear, loudest
first, and only reports a problem when all of them have failed — naming what happened to each.

While it is talking to an AP, **the beacon scan stands aside**. Bluetooth has one radio: with a
scan running, a Galaxy S24+ settles on the 23-byte default ATT MTU, and the AP then closes the
link rather than answer a request it has no room to reply to. The scan is stopped for the one
connection and started again straight after (at most one stop and start per pull, so well inside
Android's five scan starts per 30 s).

Every step is written to a **connection log** you can read in the app, at the foot of any tab
that needs the password: which AP was tried and how loud it was, the MTU agreed, and the GATT
status of anything that failed. There is nothing private in it — no Bluetooth address, no
password, no grid content. If Android has not been given the Nearby devices permission to
*connect* (as opposed to scan), the app says exactly that and offers a button to grant it,
rather than reporting a sulking AP.

## Quiet spells: why "not heard" takes 45 seconds

Bluetooth shares the radio and the antenna with Wi-Fi, so a watching phone or laptop misses
adverts in bursts even when every AP is transmitting normally. Measured on the bench over 150 s:
adverts from each AP arrived every 0.25 s as a rule, but there were 10 to 13 quiet spells of 5 s
or more per AP, the longest 14.6 s, and 10 to 14 of them fell at the same moment on two
different APs — three boards do not stop transmitting in unison, so the gaps are the receiver.

So an AP is called **not heard** only after **45 s** (about three times the worst measured gap),
and the AP card shows "last heard N s ago" the whole time, in amber past 15 s, so a real quiet
spell is visible long before that. A handheld's presence reaches the phone only through an AP's
rotating beacon — one handhelds frame about every 4 s per AP — so a blackout can swallow four in
a row; a handheld therefore stays in the list for **90 s** after the last frame that placed it.

**An SOS never waits on either timer.** An alert frame raises its notification the moment it
arrives, even if the AP that sent it has been marked not heard for minutes.

## Battery

Scanning is low latency while the app is on screen and balanced in the background, filtered in
the Bluetooth chip to this grid's company ID. The admin link adds one short connection a minute
while the app is on screen and nothing at all when it is not. The battery cost of watching all
day has not been measured yet. Samsung's battery management can pause background apps: if
watching stops with the screen off, set **Settings > Apps > LocalGrid Watch > Battery** to
**Unrestricted** (the app links there). Stop watching when you do not need it. Watching does not
restart by itself after the phone restarts; open the app to resume.
