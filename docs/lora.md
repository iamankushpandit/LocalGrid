# LoRa backbone (D71)

APs that cannot hear each other over Wi-Fi can still carry the grid's traffic to each other over
LoRa. The first hardware is a **Reyax RYLR998 on all three APs** (owner, 2026-09-20), so MAIN
carries a LoRa module **and** its GPS; the two never share a pin.

LoRa is a second backbone, not a second network. The core's routing, duplicate suppression, sticky
state (D48) and end-to-end sealing of 1:1 messages are unchanged: a frame that arrives over LoRa
is the same frame that would have arrived over ESP-NOW.

## What it is, honestly

| | ESP-NOW backbone | LoRa (RYLR998, SF9 BW125) |
|---|---|---|
| Range | tens to a couple of hundred metres | hundreds of metres to kilometres |
| Throughput | hundreds of kbit/s | about 1.5 kbit/s, minus AT and encoding overhead |
| One 288-byte frame | under a millisecond | roughly 2 s of airtime, split into 3 transmissions |
| Voice (D61) | yes | **never** |

So LoRa carries text, alerts, presence, positions, time and grid state. Voice frames
(`LG_T_VOICE`) are never offered to it.

## The module

The RYLR998 speaks AT commands over a UART; there is no SPI driver to write. Facts that shape the
design:

- `AT+SEND=<address>,<length>,<data>` takes **text**, ends at a newline, and separates fields with
  commas, so raw binary cannot be sent. Frames are **base64** (about a third larger) and split.
- A received frame arrives unsolicited as `+RCV=<address>,<length>,<data>,<rssi>,<snr>`.
- Addressing: every AP sets `AT+ADDRESS=<AP index>` and a shared `AT+NETWORKID`, so a stray module
  from another network is ignored before we even look at the bytes.
- The module's own AES (`AT+CPIN`) is **not** relied on. LocalGrid seals the backbone itself, with
  the same key and nonce rules as ESP-NOW (`docs/DESIGN_REVIEW.md`), so the link is safe even if
  the module's encryption is weak, absent, or misconfigured.
- 3.3 V logic and supply. **Never power it without its antenna.**

### Wiring (as built, owner 2026-09-20)

**The pins are not the same on every AP.** MAIN also carries a GPS, and its LoRa module is wired
to a different set of pins; NORTH and SOUTH were wired first and keep theirs. The firmware picks
the pins by AP index, so each board is flashed with the same build.

**NORTH and SOUTH** (no GPS):

| Module pin | AP (ESP32) |
|---|---|
| VDD | 3.3 V |
| GND | GND |
| TXD | GPIO 4 |
| RXD | GPIO 5 |
| RST | GPIO 13 |

**MAIN** (LoRa and GPS together):

| Wire | Goes to | Note |
|---|---|---|
| LoRa VDD | 3.3 V | never 5 V, on power or on its RX line |
| LoRa GND | GND | one ground for everything |
| LoRa TXD | GPIO 32 | crossed over: its transmit is the ESP32's receive |
| LoRa RXD | GPIO 33 | |
| LoRa RST | GPIO 25 | optional; lets the firmware restart a wedged module |
| GPS VCC | VIN (5 V) or 3.3 V | it has its own regulator |
| GPS GND | GND | |
| GPS TXD | GPIO 16 | |
| GPS RXD | nothing | the AP never transmits to the GPS |

Both LoRa wirings use UART1; MAIN's GPS keeps UART2. On MAIN the two radios share only power and
ground.

**Powering both.** The GPS is happy on 5 V because it has its own regulator. The RYLR998 must have
3.3 V, and it draws over 100 mA while transmitting, on the same regulator that feeds the ESP32's
own Wi-Fi bursts. These boards already record brownouts (17 to 27 each on the bench), so put a
100-470 uF capacitor across the module's 3.3 V and GND, as close to the module as possible. If
brownouts rise after fitting it, give the module its own 3.3 V supply and join the grounds.

**Never power the module without its antenna.**

## Frames on the air

A backbone frame (up to 288 bytes today, `BB_FRAME_MAX` allows more) is sealed exactly as it is
for ESP-NOW, then split into slices of at most **130 bytes**. Each slice gets a five-byte header,
and header and slice are base64-encoded **together** into the one token `AT+SEND` carries:

| Field | Size | Meaning |
|---|---|---|
| magic | 1 | `0x4C` ('L') |
| sender | 1 | kind in the high nibble (0 an AP, 1 a handheld), index in the low nibble |
| id | 1 | message number, so parts of different payloads never mix |
| part | 1 | part number, 0-based |
| of | 1 | part count, 1 to 48 |
| — | — | the five bytes above plus the slice are base64: **180 characters**, exactly one `AT+SEND` |

**Three things here differ from the first sketch, and this is the record of why.**

1. **The header is base64-encoded with the slice, not sent beside it.** `AT+SEND` takes text that
   ends at a newline and separates its fields with commas, and a raw header byte can be either.
   One base64 token keeps every byte on the air text-safe and keeps the fields exactly as listed.
   180 characters decode to 135 bytes with no padding: 5 of header and 130 of payload.
2. **The sender is a whole byte** (owner, "Ready for handhelds later"), so a handheld peer needs no
   format change. The low nibble limits a handheld's index to 0-15; devices above that would need
   a second byte, which is a change to make before handhelds get modules, not after.
3. **The part number and the count are whole bytes**, so the format carries 48 parts, which is
   6240 bytes: the owner's 6 KB voice-note ceiling with room to spare. Nibbles would have capped it
   at 15 parts and made voice notes a breaking change.

A receiver joins the parts and hands the result to the same code that handles an ESP-NOW frame. A
part that arrives twice is ignored; a payload whose seal fails is dropped and counted. A set is
given up when **no new part has arrived for 10 s** - an idle timeout, not ten seconds from the
first part, because 48 parts are nearly a minute of airtime - or after 180 s in total.

**What an AP has room for.** The format carries 6 KB; the memory to hold one is not free. Every
buffer is taken **once, the first time a module answers**, and never again, so the heap cannot
fragment under traffic and an AP with no module holds no frame buffers at all:

| Taken | Size | For |
|---|---|---|
| 2 reassembly + 4 send slots of 512 B | 3.0 KB | every frame the grid sends today |
| 1 reassembly slot of 6240 B | 6.1 KB | receiving a long payload: a peer decides what it sends |
| 1 send slot of 6240 B | 6.1 KB | sending one - **only if 24 KB of heap would still be free** |

So an AP with a module spends 9.1 KB, or 15.2 KB where there is room. MAIN, which runs at about
24 KB free, will normally skip the send slot and say so at `[LORA]`; it can still receive a long
payload, and nothing produces one yet. A payload larger than the slots available is refused at its
first part, logged and counted - never truncated, and never allowed to fill memory.

**An AP with no module** pays for the reader task's stack and the serial port's buffers, and
nothing else: no frame buffers, no transmission, no timer, no second log line, and no delay at
start-up, because the one look for a module happens on the task rather than in the boot path.

**Relaying is store-and-forward, not cut-through.** An AP in the middle reassembles a payload,
authenticates it, and only then re-floods it, because nothing can be authenticated until all of it
has arrived and LocalGrid does not forward bytes it has not checked. That is why the ceiling is a
memory question rather than a format one.

## Always in parallel, without wasting airtime (owner, 2026-09-20)

The owner chose to use LoRa **in parallel** with Wi-Fi rather than only as a fallback. Sending
everything twice would fill the air, so "in parallel" is applied with a queue that knows what
matters:

1. **Urgent broadcasts, SOS and all-clear go on both radios, always.** This is the case the radio
   exists for, and duplicate suppression already makes a second copy harmless.
2. **Everything else goes on LoRa when the ESP-NOW link to that AP is down, or when the frame has
   not been acknowledged over Wi-Fi.** When both APs can hear each other perfectly, LoRa carries
   only the heartbeat below, and the air stays clear for the traffic that needs it.
3. **A heartbeat every 30 s**, staggered by AP index so three APs never speak at once, keeps the
   link alive and measured (RSSI and SNR are reported with
   every `+RCV`), so the admin page and the watcher can show "LoRa link up, −104 dBm" even when
   nothing else is being sent.
4. **Voice is never queued.** Nor is anything already carried by an ESP-NOW link that is up,
   except the alerts in rule 1.

The send queue is bounded and **drops the oldest low-priority frame** when full, never an alert.
Airtime is tracked so the AP can report how busy the radio is, and so a region's duty-cycle limit
can be respected later.

**Three APs share one channel**, and LoRa has no carrier sense in this module, so two APs that
transmit together are both lost. Every transmission therefore waits a short random time first,
a frame that was not acknowledged is retried with a longer random wait, and the heartbeats are
staggered by AP index.

**The broadcast address, and what is still unverified.** The RYLR998's AT command guide documents
`AT+SEND` to address **0** as reaching every address on the network ID, so the firmware uses
address 0 for a frame meant for every AP and defaults to that. Nobody has yet confirmed it on the
modules we actually bought, so `lora broadcast off` on the AP console switches to one transmission
per peer, and `lora at AT+SEND=0,2,hi` on one AP with the others watching their logs is the test
that settles it. **Write the answer here once the bench has it.**

Address 0 is also MAIN's own address, because the owner's address plan makes an AP's address its
index. That is harmless: a unicast to MAIN is simply heard by every AP, they all hold the same
backbone key, and the envelope decides whom the frame is for. Duplicate suppression handles the
extra copies.

**"Unacknowledged" over Wi-Fi** has no end-to-end acknowledgement to consult, so the firmware reads
it as: the ESP-NOW link to that AP is not confirmed, or neither its HELLO nor a MAC-level ACK of a
unicast to it has arrived within the 6 s link timeout. A link that has gone quiet counts as
unacknowledged and LoRa starts carrying for it.

**A unicast is only sent to an AP LoRa has actually heard.** Transmitting at an AP whose heartbeat
has never arrived would be seconds of airtime spent on nothing.

## What the watcher and the admin page show

The AP's status gains the LoRa link: fitted or not, the last RSSI and SNR, frames sent and
received, parts dropped, seals failed, queue depth and airtime used. These ride on the existing
traffic record (D70), so both the dashboards and the admin page show them without a new API.

## Optional, always (owner, 2026-09-20)

**No AP ever expects a LoRa module.** At start-up an AP asks the module to identify itself; if
nothing answers within a short timeout it says so once at `[LORA]`, marks the radio "not fitted",
and behaves exactly as an AP with no module at all — same behaviour, same timings, no retry storm,
no log noise, nothing that can fail. The module may be added, removed, or die mid-run: the AP
notices, reports it, and keeps working. A grid may mix APs with and without modules, and nothing in
routing depends on a peer having one.

## Ready for handhelds later (owner, 2026-09-20)

Two handhelds will get modules, both with a GPS. Nothing about that is built yet, but the AP side
must not need a breaking change when it happens, so two things are settled now:

- **Address plan.** The module's address identifies the peer: **1-16 are APs** (1 + the AP index; address 0 stays free as the broadcast address, which the bench proved on 2026-09-20 is not shareable with a real peer),
  **100 + device number are handhelds** (so device 1 is 101), and the rest are reserved. Every
  LocalGrid module shares one network ID derived from the grid's discriminator.
- **The part header carries a peer byte, not a nibble**: `kind (high nibble: 0 AP, 1 handheld) |
  index (low nibble)` becomes a full byte, so a handheld peer needs no format change.

What a handheld over LoRa would send is deliberately small: its position (D65), presence, an SOS or
urgent broadcast, and short text. Not voice, not registration, not the shared state. The open
question, to answer before that work starts: **which key seals a handheld's LoRa frames**, since
handhelds do not hold the APs' backbone key. Probably a LoRa key derived from the grid secret and
held by both, but that is a decision, not an assumption.

## Push-to-talk over LoRa: notes, never live (owner asked 2026-09-20)

Live push-to-talk cannot work on this radio, and no firmware can change that. Our voice is 32
kbit/s (8 kHz IMA ADPCM, `LG_VOICE_DATA_MAX` 400 bytes per 100 ms). At a useful range this module
carries about 1.8 kbit/s, roughly eighteen times too slow, and the AT interface adds a third again
for text encoding. LoRa's fastest settings reach tens of kbit/s only by shortening the range to
about what Wi-Fi already covers, which defeats the purpose of fitting it. So `LG_T_VOICE` is never
offered to LoRa, on any AP, in any configuration.

What works instead is a **voice note**, which the owner has asked for (2026-09-20, D72): hold the
button, speak, release, and the speech is delivered and **played once** when it arrives - over
Wi-Fi when the grid can, over LoRa when that is the only way.

It **looks like push-to-talk and nothing else**: the same button and the same screen as D61, with
the speech simply arriving after a delay when it travelled by LoRa. Nothing is saved and nothing
can be replayed: the clip lives only while it travels, plays once, and is gone. There is no inbox,
no history and no store-and-forward, so a handheld that is offline misses it exactly as it would
miss live talk. That also keeps it off every device's flash and out of an AP's memory.

**Sizes, settled** (Codec2, notes up to 30 s):

| Mode | 30 s of speech | Over LoRa at long range |
|---|---|---|
| Codec2 1200 bit/s | about 4.5 KB | roughly 30 s on air (base64 included) |
| Codec2 3200 bit/s | about 12 KB | roughly 80 s on air - Wi-Fi only |

So a note sent over LoRa is encoded at the low rate, and the LoRa payload ceiling is **6 KB**. A
larger note is carried over Wi-Fi or refused for LoRa with a clear reason; it is never truncated.

The older text below explains why live voice cannot work at all:

What can work is a **voice note**: a few seconds of speech, compressed by a codec built for this
(Codec2 at 1.2-3.2 kbit/s rather than 32), sent as a message and played when it arrives. Ten
seconds of speech is about 2-5 KB, which is 9-24 s of airtime at long range. A message, not a
conversation. Nothing of it is built yet, and the speech codec is a handheld matter (note the
licence question: Codec2 is LGPL, and LocalGrid has no licence yet - see THIRD_PARTY.md).

**What the AP side must be ready for now**, because these are awkward to retrofit:

- **Payloads much larger than one frame.** A clip is roughly 25 transmissions, not 3, so
  reassembly must handle long multi-part payloads with their own timeout, and the part header must
  number them adequately.
- **An alert must interrupt a clip.** A long low-priority transfer is pre-empted mid-way by an
  urgent broadcast or SOS, which goes out at once; the clip resumes or is abandoned, and either
  way nothing is corrupted and the alert never waits.
- **Relay parts as they arrive.** An AP in the middle must not need the whole clip in memory:
  MAIN's free heap is about 24 KB. Parts are forwarded as they come, or the clip is refused with a
  clear reason.

## Optional on handhelds too (owner, 2026-09-20)

When handhelds get modules, the module is optional there in exactly the way it is on an AP: a
handheld with no module behaves as it does today, and the grid never assumes a handheld can be
reached that way. An AP must therefore keep working with any mixture - some handhelds with a
module, some without, some whose module has just died.

## Memory, measured (2026-09-20)

The long-payload slots for D72's voice notes are **off** (`LORA_LONG_PAYLOAD 0`). With them on, an
AP that ran at 22-28 KB free fell to 9-12 KB with a **1 KB low-water mark** once handhelds joined,
and MAIN reached 0 - the grid was one allocation from failing, and the admin link (which wants 24
KB) would have stopped being offered. Checking the heap when the slots are taken is not enough,
because at boot there is plenty and the squeeze comes later.

With them off, and LoRa otherwise running on all three APs: **NORTH and SOUTH 22 KB free, 13-15 KB
low-water; MAIN about 17 KB.** LoRa itself costs roughly 8.5 KB (its task, the UART buffers and the
small slot pool). Nothing sends a long payload yet, and the wire format still carries 48 parts, so
turning the slots on later changes nothing on the air - but it needs a board with more RAM, or
PSRAM, or a smaller ceiling. **Voice notes cannot ship on these APs as they stand.**

## Seeing that it works

A radio nobody can see is a radio nobody trusts, so LoRa reports itself everywhere the grid is
already visible:

- **The AP's own status** (`/api/status`), so the **admin page** shows, per AP: fitted or not, the
  module's version, the address and network ID, link up or down to each peer with the last RSSI and
  SNR and when it was last heard, frames and parts in and out, parts dropped, reassembly timeouts,
  seals failed, queue depth, retries and airtime used.
- **The traffic record** (D70), so `tools/grid_watch.py` and the Android app show the same on their
  Traffic tab, and colour it red when the link is down, airtime is high, or drops are rising.
- **The console**: `lora` prints the same facts, `lora send <text>` puts a test frame on the air
  whatever the policy would say, and `lora at <command>` asks the module something directly.
- **One number that proves it is earning its keep**: frames that arrived over LoRa **and** were not
  already delivered by Wi-Fi. If that number rises while the backbone is broken, LoRa is working.

## What the firmware actually sends the module

At start-up, once, and again whenever the module is reset or restarts by itself. Every command is
checked for its `+OK`; the first refusal resets the module and tries again after a backoff that
doubles to a minute.

```
AT                      three times, 1 s apart: is anything there at all?
AT+VER?                 for the log and the admin page
AT+ADDRESS=<AP index>   0 for MAIN, 1 NORTH, 2 SOUTH
AT+NETWORKID=<3..15>    3 + (first byte of the grid discriminator mod 13)
AT+BAND=868500000       change this one number for a 915 MHz region
AT+PARAMETER=9,7,1,12   SF9, BW 125 kHz, CR 4/5, preamble 12
AT+CRFOP=22             22 dBm
AT+MODE=0               transceiver
AT+ADDRESS?  AT+NETWORKID?  AT+PARAMETER?    read back, and log what is really set
```

At these settings one full part (180 characters) is **about 0.93 s on the air**, so a 288-byte
frame is three parts, roughly **2.8 s** plus a random backoff of up to 0.3 s before each; that is
about **140 bytes a second** of sealed frame, or 1.1 kbit/s. A 6 KB payload would be 48 parts, near
45 s. Airtime is added up per transmission and reported.

## Chaos testing it

`tools/chaos.py` (the `chaos` skill) must be able to exercise LoRa, which means the firmware has to
offer the hooks:

- **`bb off <seconds>` / `bb on`** on the AP console: stop using the ESP-NOW backbone for a while,
  so anything that still crosses between APs went by LoRa. It restores itself after the timeout
  even if the tool dies, and it is console-only.
- **`lora off <seconds>` / `lora on`**: the mirror image, to prove the grid falls back to Wi-Fi.
- **`lora reset`**: pretend the module wedged, and check the AP recovers it.

As built, the console takes exactly these. Seconds default to 60 and are capped at 3600, so a typo
cannot take a radio out for a day, and both hooks restore themselves from the core task's own
clock. Neither is reachable from the BLE admin link or the admin page.

```
bb                        is the ESP-NOW backbone on?
bb off [seconds]          stop using it; logs at [BB] when it engages and when it restores
bb on
lora                      the whole LoRa state: module, peers, counters
lora send [text]          one echo on the air whatever the policy would say
lora off [seconds]        stop using LoRa; logs at [LORA] both ways
lora on
lora reset                reset and reconfigure the module
lora broadcast on|off     one transmission to address 0, or one per peer
lora at <AT command>      ask the module directly, for bring-up; the reply is logged at [LORA]
```

The chaos run then adds LoRa rounds: break the Wi-Fi backbone between two APs, send text and an
SOS, and count what arrived and how long it took; break LoRa instead and check nothing is lost;
pull the module's power and check the AP carries on. The report says how many frames crossed by
LoRa, the time they took, and the RSSI and SNR at which it stopped working.

## Proof of concept: done when

1. All three APs report their module present and configured, and MAIN keeps its GPS fix and grid
   time while its LoRa module is fitted and busy.
2. With the two APs deliberately out of Wi-Fi range of each other (MAIN may or may not bridge
   them), a text from a handheld on one reaches a handheld on the other, and an SOS does too.
3. The serial logs show the frame crossing by LoRa (`[LORA]`), with RSSI and SNR.
4. Nothing regresses when the modules are removed: an AP with no module behaves exactly as today.
5. Measured and written down: time for a message to cross, size after encoding, parts per frame,
   and the range at which it stopped working.
