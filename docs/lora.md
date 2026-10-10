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
- Addressing: every AP sets `AT+ADDRESS=<AP index>`, and `AT+NETWORKID=18`, the module's factory value.
  A grid used to take a network ID of its own (3..15) so that a stray module was ignored by the radio;
  on those IDs the module's receiver silently loses about one packet in six (D113, measured 2026-10-09),
  so a stray module's packets are now refused when they fail to open with the grid's key instead.
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

**A GPS on a handheld is wired the same way, and is fed 5 V** (D65). On the Freenove FNK0104B the
4-pin UART connector carries RXD, TXD, GND and **5 V** - 5 V is the only supply pin it has, which is
also why the RYLR998, a 3.3 V part, takes its power from the I2C connector instead. So a handheld's
GPS gets the same supply the AP's does, and a handheld that will not lock while the AP does is **not**
a supply difference between the two.

### When a GPS never locks

Read `gps` on the console, which reports what the module *hears* and not only what it used:

```
GPS: no fix yet, 0 satellites used, 246255 sentences, 1 bad checksums
GPS: 21 in view, 21 with a signal, best 25 dB-Hz
```

- **Sentences arriving with no bad checksums means the wiring, pin and baud are all correct.** That part
  of the install is proved; the problem is upstream of the UART, in the antenna or what reaches it.
- **Decoding a satellite's orbit needs about 30 dB-Hz**, well above the ~25 it takes merely to notice a
  carrier. A best figure stuck in the twenties never becomes a fix, however long it is left - measured on
  both Freenove handhelds across eleven hours of uninterrupted reception on 2026-10-01.
- **More than about fourteen "in view" is not a sky view.** No more horizon holds that many GPS
  satellites. A module claiming twenty-two is listing the PRNs it is searching for and reporting its own
  noise floor against each, which means nothing usable is reaching it at that moment.
- The firmware says all of this itself now: the console names the fault, and the Status screen's **Lock**
  row says `no: signal too weak to use, check the antenna` rather than "looking for satellites".

**The handheld's own LoRa module costs it about 3 dB**, measured on Channi on 2026-10-01 by holding one
port open, sampling `gps` every 15 s and alternating `lora on` and `lora off 200` three times, 30 samples
each: best strength averaged **24.2 dB-Hz with LoRa on and 27.4 with it off**, peaking at 28 against 31.
At this margin that is decisive, because decoding an orbit takes about 30. The RYLR998's antenna sits
centimetres from the GPS antenna on these boards and transmits 22 dBm. It is not LoRa alone, though:
**MAIN carries LoRa too (D71) and locks**, so it takes a marginal antenna *and* a close transmitter.

Alternate the switch rather than throwing it once. A first version of that test switched LoRa off a
single time part way through and appeared to show it tripling the satellites in view; alternating showed
the count barely moved and only the strength did. Anything that drifts over a few minutes - the satellites
themselves included - imitates a one-way switch perfectly.

What is left to check is physical, in this order: **a view of the sky** (the dominant term, and a window
gives only a narrow slice of it plus reflections off the glass and frame), **the antenna seated** in its
connector, and **distance between the GPS and LoRa antennas**. `lora off <seconds>` and
`wifi off <seconds>` measure the last of those for free, before anyone reaches for a meter. For a module
or antenna still suspected after that, the decisive test is a **swap** with one known to work.


**Powering both.** The GPS is happy on 5 V because it has its own regulator. The RYLR998 must have
3.3 V, and it draws over 100 mA while transmitting, on the same regulator that feeds the ESP32's
own Wi-Fi bursts. These boards already record brownouts (17 to 27 each on the bench), so put a
100-470 uF capacitor across the module's 3.3 V and GND, as close to the module as possible. If
brownouts rise after fitting it, give the module its own 3.3 V supply and join the grounds.

**Never power the module without its antenna.**

## Frames on the air

A backbone frame (up to 288 bytes today, `BB_FRAME_MAX` allows more) is sealed exactly as it is
for ESP-NOW, then split into slices of at most **85 bytes**. Each slice gets a six-byte header,
and header and slice are base64-encoded **together** into the one token `AT+SEND` carries:

| Field | Size | Meaning |
|---|---|---|
| magic | 1 | `0x4D`. It was `0x4C` while the header was five bytes; the two formats refuse each other at the first byte |
| sender | 1 | kind in the high nibble (0 an AP, 1 a handheld), index in the low nibble |
| id | 1 | message number, so parts of different payloads never mix |
| part | 1 | part number, 0-based |
| of | 1 | part count, 1 to 73 |
| for | 1 | who it is for: 0 everyone, 1 + index an AP, 100 + device number a handheld |
| — | — | the six bytes above plus the slice are base64: at most **124 characters**, exactly one `AT+SEND` |

**Every part goes to the module's broadcast address** (2026-10-09), and the sixth byte says who it
is for. The module hides from a radio whatever is addressed to another, and has no carrier sense,
so an AP could not know its two neighbours were mid-frame and transmitted over them. Now every
radio hears every part: it keeps the ones that are for it, and for the rest it stays off the air
while the frame and its answers go by (`Heard 1/2 of 15 from peer 0x00 for 0x02` in an AP's log).

**A part is 124 characters, because a packet must be short on the air, not short in characters**
(measured 2026-10-09, network ID 18, each packet sent after three seconds of silence, SF9 at
125 kHz): 120 characters (0.69 s) arrived 28 of 28, 140 (0.80 s) 31 of 32, 160 (0.88 s) 55 of 56,
200 (1.07 s) 1 of 32 and 240 (1.26 s) 1 of 56 - the rest arriving broken, and the same at 22 dBm
as at 5. Sent back to back with no silence between them, the long ones arrive, so a burst test
does not show the limit: parts were 240 characters for two hours on the strength of one, and one
in six of them arrived. `LG_LORA_AIR_CEILING_MS` now stops a build whose longest part would be
more than 0.8 s on the air at its radio setting. A sealed text is two parts, a compact voice
packet three.

**The parity part.** A message of two parts or more that nobody acknowledges - a handheld's
frames go to every AP and none of them answers at the link - is followed by one more part, the XOR
of its slices, so a receiver that lost exactly one part rebuilds it. A frame that is acknowledged
(AP to AP, D104; AP to a handheld, D109) takes none: it is sent again until it is answered.

**Four things here differ from the first sketch, and this is the record of why.**

1. **The header is base64-encoded with the slice, not sent beside it.** `AT+SEND` takes text that
   ends at a newline and separates its fields with commas, and a raw header byte can be either.
   One base64 token keeps every byte on the air text-safe and keeps the fields exactly as listed.
   124 characters decode to 91 bytes: 6 of header and 85 of payload; the parity part is one byte
   longer and the same 124 characters.
2. **The sender is a whole byte** (owner, "Ready for handhelds later"), so a handheld peer needs no
   format change. The low nibble limits a handheld's index to 0-15; devices above that would need
   a second byte, which is a change to make before handhelds get modules, not after.
3. **The part number and the count are whole bytes**, so the format carries 73 parts, which is
   6205 bytes: the owner's 6 KB voice-note ceiling with room to spare. Nibbles would have capped it
   at 15 parts and made voice notes a breaking change.
4. **The destination is in the part, not in the module's address** (above).

A receiver joins the parts and hands the result to the same code that handles an ESP-NOW frame. A
part that arrives twice is ignored; a payload whose seal fails is dropped and counted. A set is
given up when **no new part has arrived for 10 s** - an idle timeout, not ten seconds from the
first part, because 73 parts are nearly a minute of airtime - or after 180 s in total.

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

## Acknowledged, and sent again (D104)

Until 2026-10-06 a frame between access points went on the air once. Three radios share the channel
and the module cannot listen before it transmits, so some frames collide and are gone; with the Wi-Fi
backbone off, two 90 minute runs lost four and five messages between registered handhelds, and none of
them to a full queue.

What a person wrote, the word that it arrived, and an alert are now answered:

| Step | What happens |
|---|---|
| The frame arrives | The receiving access point queues an acknowledgement: `L`, `A` and the frame's outer sequence number, six bytes, sealed with the backbone key like any frame. |
| Whose turn | A frame may have gone to every access point at once. Each answers in a turn of its own, 500 ms apart, counted from the sender's index, so two answers do not collide. |
| The sender listens | For as long as the last turn takes, it transmits nothing but alerts and answers it owes: the module cannot hear while it transmits. |
| No answer | The frame goes out again from its first part, addressed to the one access point still to answer when there is only one. Three times on the air in all, then it is given up and counted. |
| It arrives twice | The same sealed frame a second time means the answer was lost, not the frame: it is answered again and not handled again. |

Voice is not acknowledged, because a second copy is too late to be heard, and routine traffic is not,
because it is repeated within a minute anyway. The hop between an access point and a handheld on LoRa
alone is unchanged: the handheld already retries, and the delivery report is end to end.

`lora` on an access point's console says how many frames every access point acknowledged, how many
went out again, and how many were given up.

## Taking over a message ESP-NOW could not deliver (D105)

LoRa stays quiet while ESP-NOW is carrying traffic to an access point (D74), and an access point
needs six seconds without a HELLO to decide it is not. A message sent in those six seconds used to
go to ESP-NOW alone and nowhere. Now, when a send fails - the radio reports no acknowledgement, the
queue is full, or ESP-NOW is off - a text or a delivery report is handed here as the bytes ESP-NOW
had sealed for that access point, which is the format this backbone carries anyway, and goes out
acknowledged and repeated (D104). If the ESP-NOW copy did arrive and only its acknowledgement was
lost, the receiver has seen that sequence number: it answers and handles nothing twice.

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
  LocalGrid module uses network ID 18, the module's factory value (D113).
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

### How it gets tested, settled before it is built

The owner's rule, 2026-10-02: future work still needs a test designed the same way as the one that
exists. Live push-to-talk got that test on the same day - `tools/chaos.py`'s `voice` round, which drives
a talk from the serial console and counts frames at **both ends** under a shared talk id, because a talk
carries no token and no acknowledgement. A note is a message rather than a stream, so the shape changes
but the method does not:

- **A `note` console command**, as `demo` is for live talk: record a known number of seconds from the
  generated voice (`hh_voice.c`, `synth`) rather than a microphone, so the same bytes are produced on
  every board and on every run, and the receiver can compare what arrived with what was sent. Without a
  console handle the harness cannot start one, which is the exact reason voice went untested until now.
- **Measured at both ends, as a payload rather than a count of frames**: bytes encoded and offered at the
  sender, bytes reassembled and played at the receiver, and the **airtime** the module reports for it.
  A note that arrives corrupt must be told apart from one that never arrives, so the receiver reports a
  checksum of what it decoded.
- **The interesting path is LoRa only.** A note crossing a healthy grid proves little, because Wi-Fi
  carries it. The round must put the receiver's Wi-Fi out - the `no_net` round's method - so the note has
  no choice, and then check it still arrives within the airtime the table above predicts.
- **An alert must pre-empt it.** D72 and D74 both say so, and a 30 s transfer is the longest window in
  which an SOS can be delayed by something unimportant. The round sends an SOS mid-note and checks the
  alert's latency against the ordinary alert figure, not just that it eventually arrived.
- **A note over the 6 KB ceiling must be refused with a reason**, never truncated and never silently
  dropped, and the refusal must be visible on the console the way `Talk not sent` already is.
- **Quiet hours apply** (D87): a note plays out loud on arrival, so the round obeys `--no-voice` and the
  audible window exactly as the live-talk round does.

The older text below explains why live voice cannot work at all:

What can work is a **voice note**: a few seconds of speech, compressed by a codec built for this
(Codec2 at 1.2-3.2 kbit/s rather than 32), sent as a message and played when it arrives. Ten
seconds of speech is about 2-5 KB, which is 9-24 s of airtime at long range. A message, not a
conversation. Nothing of it is built yet, and the speech codec is a handheld matter (note the
licence question: Codec2 is LGPL, which LocalGrid's GPL-3.0-or-later can carry - see THIRD_PARTY.md).

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

## A handheld on LoRa alone, measured (2026-09-30)

The handhelds have modules now, and the leg they add is the one the radio exists for: a handheld out of
Wi-Fi range still reaching the grid. Measured on the bench with all three access points on **battery**,
no serial cable to any of them, and one handheld's Wi-Fi switched off with `wifi off <seconds>` (D78).

| | |
|---|---|
| Message out and acknowledgement back | **2.7 s** |
| One-way delivery, median | **1.3 s** (1.0-2.0 s) |
| From Wi-Fi off to an access point answering on LoRa | 1.5-4.7 s |
| A 12-byte 1:1 message, sealed and on the air | 45-60 bytes, 1 part |
| RSSI / SNR, bench distances | -21 to -41 dBm, SNR 7-10 |
| **Delivery per attempt** | **about three quarters** |
| **Delivery with retries working** | **9 of 9 outbound** (same round, same seed, same boards) |
| A message recovered by a retry, end to end | 11.3-11.9 s (second attempt), 22.5 s (third) |

That last figure is the important one and the least comfortable. Nothing acknowledges at the LoRa link
layer, so a frame that is not heard is simply gone until something sends it again. Over two chaos rounds,
34 messages to and from a handheld with no Wi-Fi: **25 delivered**. The firmware's own counters agree -
about 20 frames out per round, no queue drops, no reassembly timeouts - so nothing was refused or
dropped locally. They were not heard.

The answer is retries, and the first attempt at pacing them was wrong in an instructive way. A 30-second
resend was chosen to keep a full 16-slot outbox from flooding a radio that carries roughly one message
every three seconds (D74). But the measurement above shows the air was not the constraint at all - a
message crossed in 1.3 s - while a dropped one waited the full thirty seconds, which for an SOS is the
wrong trade by a wide margin. The rate is now bounded by **messages per pass** rather than by the
interval: one re-offer per tick, oldest sequence first, every 10 s
(`LG_RESEND_LORA_MS`, `LG_RESEND_LORA_MAX_PER_TICK`). The offer rate is then the same whether one message
is waiting or sixteen, and the interval is free to be short.

The retry chain is visible in the latency distribution rather than inferred from it: a cluster at
**1.3 s** (first attempt), five messages at **11.3-11.9 s** (the +10 s second attempt) and one at
**22.5 s** (the third). That last one lost two frames in a row and still arrived; before tonight it was a
silent loss.

It took four gates to let that retry happen, and the fourth was the instructive one. Three were in
`lg_core` (refuse to build a frame without a session, attempt only when registered, retry only when
registered). The fourth was in `hh_service`, which returns early when Wi-Fi is off - above the line that
drives the outbox. So the retry was written, built, flashed, and never ran. It was caught by arithmetic:
the round reported the same 6 of 9 as before **and the same 21 frames on the air**, and 21 is exactly nine
sends plus six acknowledgements plus the announcements, leaving no room for a retransmission. With the
tick moved above the return, the same round delivered **9 of 9** and the air carried **28** frames.

That also settles what the 66%-outbound against 83%-inbound asymmetry was, and it was not a directional
radio. A message *to* a LoRa-only handheld is sent by a **registered** handheld, whose outbox resends
every 3 s over Wi-Fi; each resend makes an access point transmit over LoRa again, so inbound quietly had
several attempts all along. Outbound had one. One side had retries and the other did not.

**What is still true after it:** a handheld cannot know whether LoRa works until it has already lost
Wi-Fi, because an access point learns the path only from an announcement and answers only one. Losing
that single frame costs up to one announcement interval before anything can be delivered to it.

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
AT+NETWORKID=18         the module's factory value; 3..15 lose one packet in six (D113)
AT+BAND=915000000       the region's band: LG_LORA_REGION in firmware/common/lg_lora_wire.h, North America by default (Europe: 868500000)
AT+PARAMETER=9,7,1,12   SF9, BW 125 kHz, CR 4/5, preamble 12
AT+CRFOP=22             22 dBm; 14 or 5 if the device was told `lora power medium|low` (D98)
AT+MODE=0               transceiver
AT+ADDRESS?  AT+NETWORKID?  AT+PARAMETER?    read back, and log what is really set
```

At these settings one full part (124 characters) is **about 0.67 s on the air**, so a 288-byte
frame is four parts, roughly **2.7 s** on the air, with a pause of 0.15 s and a random start of up
to 0.3 s between them; that is about **90 bytes a second** of sealed frame, or 0.7 kbit/s. A 6 KB
payload would be 73 parts, near a minute and a half. Airtime is added up per transmission and
reported.

**Pacing** (`firmware/common/lg_lora_wire.h`, measured 2026-10-09 with `lora burst` and `lora echo`).
A receiver needs no quiet between packets: the same sender's arrive back to back, and an answer
sent 20 ms after a packet is heard by everyone, the radio that had just finished transmitting
included. (The second and more of quiet left after every packet until then answered a loss the
network ID was causing, D113.) What the firmware leaves is for the boards and for each other:
150 ms after each of its own packets; before starting a frame, one answer's length (about 0.5 s)
for every radio that will answer the packet it just heard; and housekeeping only into air that has
been silent for seconds. Every wait that depends on how long a part or an answer is on the air is
computed from `lora_airtime_ms()`, which follows the radio setting, so a faster setting needs no
retuning by hand. An alert waits for a frame in flight and its answers too, but four seconds at
most, and then goes whatever the air is doing. A frame that is acknowledged goes up to three times
(twelve for an alert), each AP taking its turn before a resend so that two frames that collided
do not collide again (D114).

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
