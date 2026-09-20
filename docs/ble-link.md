# BLE admin link (D70)

The beacon (D68) is a trickle: it carries alerts, AP health, who is online and batteries, and
nothing larger. To show everything the admin page shows — the map with everyone's coordinates,
groups, announcements, the availability history and what happened to each AP — a watcher
(`tools/grid_watch.py` or the Android app) **connects to a nearby AP over BLE** and asks for it.

This changes D68's "the laptop only listens". The watcher now transmits, but only to an AP's BLE
service: it still never joins the grid's Wi-Fi, never carries a grid message, and cannot change
anything. Every request is read-only.

## Who may see what

| Data | Who |
|---|---|
| Beacon: alerts, AP health, handhelds online, batteries, names | anyone paired (the `LGW1:` code) |
| Everything else, including **positions** | paired **and** logged in with the **admin password** |

The admin page shows positions to anyone with the admin password, and the watcher now matches that
(owner, 2026-09-19). A paired phone that is lost shows status but no coordinates.

## The service

One GATT service on every AP. The service UUID is **not advertised**: the advert and the scan
response carry exactly the bytes D68 sends and nothing more, so a stranger sees no new data.

| UUID | Use |
|---|---|
| `4c470001-6c67-4772-6964-42544c453031` | service: LocalGrid admin link |
| `4c470002-6c67-4772-6964-42544c453031` | request characteristic (write, no response), client to AP, sealed |
| `4c470003-6c67-4772-6964-42544c453031` | reply characteristic (notify), AP to client, sealed |

A client finds the service by connecting to an AP it already hears beaconing and discovering
`4c470001-…` by UUID; nothing needs to be advertised for that.

The AP accepts **one client at a time**, drops the connection after 60 s idle, and refuses to
connect while it is short of memory (below 24 KB of free heap it simply stops advertising
connectably, and a connection that slips in anyway is dropped at once). A connection never delays
grid traffic: the AP's own link task builds replies from the snapshot the core task publishes, and
the radio work happens on the NimBLE host task. The core task is never blocked by a watcher.

**MTU.** A chunk carries `MTU - 3 - 4 - 16` bytes of body, so the AP needs an ATT MTU of at least
**64** and asks for 247. A client that will not go above the 23-byte default is refused.

### How the advert changes (implementation note)

While the AP will accept a link it advertises `ADV_IND` instead of `ADV_SCAN_IND`; the advert
payload and the scan response are byte for byte the same either way, and both PDUs are scannable,
so the D68 beacon never stops. As soon as a watcher connects, the AP goes back to the
non-connectable advert, which both keeps the beacon going and makes "one client at a time" true by
construction. It becomes connectable again when the watcher leaves.

## Sealing

The link key comes from the same key the pairing code carries, so a paired watcher needs nothing
new:

    K_link = HKDF-SHA256(salt = "LG-BLE-LINK-1", ikm = K, info = "admin link", L = 32)

Every message is sealed with ChaCha20-Poly1305 and a **full 16-byte tag** (there is no 27-byte
advert limit here).

- **Nonce:** `dir (u8: 0 client to AP, 1 AP to client), session (u16 LE), 0x00, counter (u32 LE),
  0x00 0x00 0x00 0x00`. The session is a random number the AP picks when the connection opens;
  each direction counts its own messages from 0.
- **AAD:** the 4-byte plaintext header below.
- A message whose tag fails, or whose counter repeats or goes backwards, closes the connection.

## Messages

Each message: `opcode (u8), flags (u8), length (u16 LE)`, then the body, sealed as above and split
into GATT-sized chunks. A reply's `flags` bit 0 means "more chunks follow"; the client joins the
chunks in order.

| Opcode | Direction | Body |
|---|---|---|
| 0x01 `HELLO` | client to AP | protocol version (u8) |
| 0x81 `HELLO_OK` | AP to client | version, AP index, boot, whether a password is set, the login salt (16 bytes) and PBKDF2 iterations (u32) |
| 0x02 `LOGIN` | client to AP | proof (32 bytes), see below |
| 0x82 `LOGIN_OK` / 0xC2 `LOGIN_FAIL` | AP to client | on failure: seconds to wait |
| 0x03 `GET_STATUS` | client to AP | — |
| 0x83 `STATUS` | AP to client | the same bytes as `GET /api/status` (JSON), including positions |
| 0x04 `GET_HISTORY` | client to AP | — |
| 0x84 `HISTORY` | AP to client | the same bytes as `GET /api/history` (packed records, D49) |
| 0x05 `GET_TRAFFIC` | client to AP | — |
| 0x85 `TRAFFIC` | AP to client | packed counters and rates, below |
| 0xC0 `ERROR` | AP to client | code (u8) and a short reason |
| 0x80 `SESSION` | AP to client | the session number (u16 LE); **the only message not sealed** |

`ERROR` codes: 1 not logged in, 2 busy (ask again), 3 bad request, 4 unsupported version, 5 the AP
could not build the reply.

**Chunks are sealed one by one.** The 4-byte header travels in the clear, because it is the AAD
and because its "more follows" flag has to be readable before the chunks are joined; the tag
covers it, so a changed header fails to open. Each chunk carries its own header, whose `length` is
that chunk's body length, and spends one counter. A reply is the chunks' bodies joined in order,
ending with the first chunk whose bit 0 is clear.

**`SESSION`.** Both directions' nonces need the session number, so the AP sends it the moment the
client subscribes to the reply characteristic, before anything is sealed. It is nonce material,
which is public by design, and it is random, so nothing is given away. Every later message is
sealed.

`GET_TRAFFIC` before a successful `LOGIN` answers `ERROR` "log in first", like the other two.

`GET_STATUS` and `GET_HISTORY` before a successful `LOGIN` answer `ERROR` "log in first". Nothing
in this link writes: there is no setup, no settings, no groups, no time setting, no password
change. Those stay on the admin page over Wi-Fi.

The bodies are **the admin page's own responses**, so the watcher and the admin page cannot drift
apart, and a new field on the admin page appears in the watcher as soon as it knows how to show
it.

## Logging in

The AP already stores the admin password as PBKDF2-HMAC-SHA256 (salt, iterations, 32-byte hash).
`HELLO_OK` gives the salt and iterations; the AP then sends a fresh 32-byte challenge with it.

    hash  = PBKDF2-HMAC-SHA256(password, salt, iterations, 32)     (what the AP stores)
    proof = HMAC-SHA256(key = hash, message = challenge || "lg-ble-admin")

The AP compares in constant time. The password itself never crosses the link, and the whole
exchange is already sealed with the grid's key, so only a paired watcher can even attempt it.
Wrong tries are rate limited the same way the admin page limits them, and the AP closes the
connection after five failures.

## What the watcher shows

The laptop dashboard and the Android app gain the admin page's tabs, from these replies:
Overview, Map, Network (APs, availability, what happened), Handhelds (groups, announcements,
handhelds). Settings stay on the admin page, because this link is read-only.

**The map.** The watching laptop or phone may have Internet even though the grid does not (owner,
2026-09-19). With Internet it draws the same map the admin page draws; without it, it falls back
to a drawn plan with distances and bearings, and the coordinates as text, so it always works
offline. Coordinates are never sent anywhere: the map is drawn locally and tiles are the only
thing fetched.

## Traffic and performance (`TRAFFIC`, owner 2026-09-19: "packet flows or rates or performance")

How busy the grid is, and how well it is coping. Counters are per AP, kept in RAM since that AP
booted, and each also carries a rate. Packed binary (D49); the watcher does the arithmetic and the
wording.

Rates come from a small ring of buckets (suggested: 10 buckets of 30 s, so a 1-minute and a
5-minute rate), not from long-lived text or history.

**Messages**, counted by what they are, both ways:

| Class | Counted |
|---|---|
| 1:1 text | in, out, relayed |
| group text | in, out, relayed |
| broadcast and urgent | in, out, relayed |
| voice (push-to-talk) | frames in, out, **dropped** (the AP drops voice rather than block) |
| presence, announcements, positions, time | in, out |
| acks (delivered, read) | in, out |

Only counts and sizes: **never any message content**, sender-recipient pairs beyond the device
numbers already in the status reply, or audio.

**Drops and faults:** duplicates suppressed, table full, unknown recipient, failed to decrypt,
TTL expired, send queue full, send timeouts.

**Backbone (ESP-NOW), per link:** frames sent, frames received, send failures, bytes both ways,
last time a frame arrived.

**Handheld side (Wi-Fi/TCP):** sessions open now, registrations, disconnects, bytes both ways,
slowest session's send wait.

**Performance of the AP itself:** free heap now and lowest since boot, largest free block, core
task queue depth and its high-water mark, stack headroom of the core and radio tasks, main-loop
longest pass in ms, Wi-Fi and ESP-NOW error counts, and (where available) CPU busy percent.

### The `TRAFFIC` bytes, layout 1

Little-endian, 580 bytes at most, built once a second on the core task and copied out as it
stands. Sections follow one another with no padding, in this order.

**Header, 24 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | layout, 1 |
| 1 | u8 | this AP's index |
| 2 | u8 | link entries that follow (0 to 8) |
| 3 | u8 | message classes, 10 |
| 4 | u8 | rate buckets, 10 |
| 5 | u8 | CPU busy percent, 255 = not measured |
| 6 | u16 | one bucket's length in ms (30000) |
| 8 | u32 | this AP's uptime, seconds |
| 12 | u32 | grid time, Unix seconds (0 unset) |
| 16 | u32 | messages taken in, all classes |
| 20 | u32 | messages sent and relayed, all classes |

**Messages, 10 classes x 12 bytes** (`u32 in`, `u32 out`, `u32 relayed`), in this order:

`0` 1:1 text, `1` group text, `2` broadcast and urgent, `3` voice, `4` acks (delivered and read),
`5` presence and registration, `6` announcements (groups, grid state, names), `7` positions,
`8` time and time zone, `9` everything else (hellos, pings, diagnostics, errors).

*in* is what the AP took from a handheld or a neighbour, *out* what it sent of its own or
delivered, *relayed* what it passed on for someone else. Nothing here records a body.

**Drops and faults, 10 x u32**, in this order: duplicates suppressed, table full, unknown
recipient, failed to decrypt, TTL expired, send queue full, send timeouts, voice frames dropped,
malformed frames, frames refused.

**Handheld side (Wi-Fi/TCP), 28 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | sessions open now |
| 1 | u8 | of those, registered |
| 2 | u16 | reserved |
| 4 | u32 | sessions accepted since boot |
| 8 | u32 | registrations |
| 12 | u32 | disconnects |
| 16 | u32 | bytes in |
| 20 | u32 | bytes out |
| 24 | u32 | the slowest send wait, ms |

**Performance, 40 bytes**

| Offset | Size | Field |
|---|---|---|
| 0 | u32 | free heap now |
| 4 | u32 | lowest free heap since boot |
| 8 | u32 | largest free block |
| 12 | u16 | core task queue depth now |
| 14 | u16 | its high-water mark |
| 16 | u32 | core task stack headroom, bytes |
| 20 | u32 | admin link task stack headroom, bytes (0 = not running) |
| 24 | u32 | longest main-loop pass, ms |
| 28 | u32 | average main-loop pass, microseconds |
| 32 | u32 | ESP-NOW send failures |
| 36 | u32 | Wi-Fi side errors (replays dropped, connections refused) |

**Rate buckets, 10 x 4 bytes**: `u16 messages in`, `u16 messages out`, oldest first, newest last.
Each covers 30 s; the last one is still filling. Ten of them give a one-minute rate (the last two)
and a five-minute rate (all ten). Counts saturate at 65535.

**Backbone links, 28 bytes each**, one per neighbour this AP has ever heard:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | the neighbour's AP index |
| 1 | u8 | flags: bit 0 the link is up |
| 2 | i8 | RSSI |
| 3 | u8 | reserved |
| 4 | u32 | frames sent to it |
| 8 | u32 | bytes sent to it |
| 12 | u32 | frames received from it |
| 16 | u32 | bytes received from it |
| 20 | u32 | send failures |
| 24 | u32 | ms since a frame last arrived from it (0xFFFFFFFF: never) |

**LoRa backbone (D71), 64 bytes**, once, after the last link entry:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | flags: bit 0 a module is fitted, bit 1 it is configured, bit 2 the broadcast address is in use, bit 3 a chaos hook is holding it off, bit 4 there is room to send a long payload |
| 1 | u8 | this AP's LoRa address (its AP index; handhelds would be 100 + device) |
| 2 | i8 | RSSI of the last part received |
| 3 | i8 | SNR of the last part received |
| 4 | u32 | frames sent |
| 8 | u32 | frames received and authenticated |
| 12 | u32 | parts transmitted |
| 16 | u32 | parts received |
| 20 | u32 | parts dropped (malformed, duplicate, oversized, or no reassembly slot) |
| 24 | u32 | incomplete frames thrown away after the 10 s reassembly timeout |
| 28 | u32 | frames that could not be sealed, opened, or authenticated |
| 32 | u16 | send queue depth now |
| 34 | u16 | its high-water mark |
| 36 | u32 | airtime used, ms |
| 40 | u32 | part retries |
| 44 | u32 | frames the send queue could not hold (never an alert already queued) |
| 48 | u32 | ms since a frame last arrived (0xFFFFFFFF: never) |
| 52 | u8 | peers heard: bit n is AP n's heartbeat being current |
| 53 | u8 | times a wedged module was reset and reconfigured |
| 54 | u8 | the network ID the modules share |
| 55 | u8 | reserved |
| 56 | u32 | of the frames received, the ones Wi-Fi had not already delivered |
| 60 | u32 | payloads refused for being larger than this AP carries |

The field at offset 56 is the one that says whether the radio earns its keep: frames that arrived
over LoRa and that Wi-Fi had **not** already delivered. If it rises while the Wi-Fi backbone is
broken, LoRa is doing the job it was fitted for.

An AP with no module sends this section with flags 0 and every counter 0, which is how a watcher
tells "no LoRa here" from "LoRa fitted and silent".

**This section is appended, not numbered in.** The layout byte stays 1: everything before it keeps
its offsets, and the link entries are still found from the link count in the header, so a decoder
written before D71 reads what it knows and ignores the rest. Anything added later goes at the end
in the same way; nothing already here ever moves.

No hardware address appears anywhere in this record (D21), and no field holds any part of a
message.

A watcher shows these as "now" rates and totals, and colours a value red when it crosses a
sensible limit (voice drops rising, a link failing, the queue near full, heap falling).

## A watcher monitors, and nothing else (owner, 2026-09-19)

A watching laptop or phone must be very good at monitoring and must never take part in the grid.
This holds by construction, not by politeness:

- **No message content exists on this link.** The two replies carry facts about the grid — APs,
  links, handhelds, batteries, positions, groups, who may announce, availability and reset
  history. They carry no 1:1 message, no group message, and no announcement text. The AP has no
  opcode that returns any message body, and 1:1 messages are sealed end to end between handhelds
  anyway (an AP cannot read them either).
- **No push-to-talk.** Voice frames (`LG_T_VOICE`) never leave the Wi-Fi side. There is no opcode
  for audio, and the beacon carries none.
- **Read-only.** No opcode writes anything, so a watcher cannot send, announce, change settings,
  add a group, set the time, or raise an alert. The grid's only inputs stay the handhelds and the
  admin page over Wi-Fi.
- **Not a member.** The watcher has no device ID, never registers with an AP, never holds a
  grid identity key, and is never shown on a handheld or the admin page as a participant.
- **A lost paired phone** shows beacon status only, until someone types the admin password.

A future opcode must keep this rule: the link answers questions about the grid's health and
whereabouts, and never carries what people said to each other.

## What the firmware does differently, and why (2026-09-19)

The AP side is implemented as written above except for these, which are the spec as it now
stands:

- **The link has its own small task**, rather than being served by the core task "between its
  normal work". A reply is several chunks long and each one may have to wait a few milliseconds
  for a radio buffer; the core task must never wait for a watcher, so it does not. The link task
  works only from the snapshot the core task publishes, so it still touches no core state.
- **Chunks are sealed one by one, with the 4-byte header in the clear.** Sealing a whole reply
  and then cutting it up would hide its own "more follows" flag from the client, and would need
  the whole reply in RAM at once, which an AP with 26 KB of free heap does not have. The header is
  the AAD, so it is still authenticated.
- **`SESSION` (0x80) is not sealed.** Both sides' nonces are built from the session number, so it
  has to arrive before anything can be sealed. It is random nonce material, not a secret.
- **The advert becomes `ADV_IND` while the AP will accept a link** (see above); its bytes are
  unchanged.
- **The free-heap floor is 24 KB**, measured when the connection arrives and once a second while
  advertising. It is below MAIN's observed low-water mark so the link is actually usable there,
  and well above the point at which an AP is in trouble.
- **The client's counter must be exactly the next one.** The header is not trusted for it: the AP
  opens each request with the counter it expects, so a repeat, a gap, or a reordered chunk simply
  fails to open, and the connection closes. This is stricter than "repeats or goes backwards" and
  needs no state beyond one number.
- **An MTU of at least 64 is required.** At the 23-byte default a chunk would carry no body.
- **Failed logins are limited per connection**, not against the admin page's shared lockout: five
  tries end the connection, and each wrong try pauses the one client that is connected. Sharing
  the page's counter would let anyone within BLE range lock the admin out of the page over
  Wi-Fi, which is worse than what it would prevent. The link is one client at a time and every
  attempt is already sealed with the grid's key.

## Privacy

- Positions stay out of the beacon (D68); they only cross the link after an admin login.
- No BLE addresses are shown or logged (D21).
- The link is read-only, so a watcher cannot change the grid, even with the admin password.
