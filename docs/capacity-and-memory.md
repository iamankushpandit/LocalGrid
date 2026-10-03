# What limits an access point, and the ways out of it

An access point on this bench runs at **3 to 7 KB of free heap** with handhelds attached. That number is
the binding constraint on the whole product: how many people a grid holds, whether a message arrives,
and what happens as access points run out of battery. This is the record of what the limit actually is,
what was measured, which designs were weighed against it, and what was decided — including the ones
deliberately not built, because the reasoning is worth more than the conclusion.

Nothing in the "ways out" section is implemented. This is a design discussion, written down.

## What a handheld costs an access point

A session is **rent, not usage**. A handheld asleep in a pocket costs the same as one being used
constantly, because the cost is in state held open on its behalf:

| What | Where it lives |
|---|---|
| Wi-Fi association state | inside the ESP32's Wi-Fi driver, spent before our code runs |
| A TCP connection | lwIP: a control block, plus send and receive windows |
| The session table entry and any message in flight | ours, and the smallest part |

The configured TCP windows are the dominant term. `firmware/node/sdkconfig.esp32` sets
`LWIP_TCP_SND_BUF_DEFAULT=2880` and `LWIP_TCP_WND_DEFAULT=2880` — **5,760 bytes per connection** of
ceiling, for a protocol whose text messages are about **200 bytes**. These are limits rather than
preallocations, so the heap is not spent until traffic demands it; but the worst case is exactly when
every handheld is talking, which is exactly when the access point has nothing left.

`node_station_room()` turns that into an advertised number:

```c
spare = (free_heap - 10240) / 4608     /* 10 KB kept back, ~4.5 KB per station */
room  = min(spare, LG_PROTO_MAX_STATIONS - clients)
if (room == 0 && clients == 0) room = 1   /* an idle AP stays joinable whatever its heap says */
```

## What the runs measured

**Capacity falls as access points die, not rises.** The intuition that fewer access points means less
relaying, and therefore more headroom, is true about *work* and wrong about *memory*. Losing a peer
frees perhaps 1–2 KB of backbone state; absorbing its four orphans costs about 18 KB. The relay saving
is small and mostly CPU, the session cost is large and persistent. **An access point is memory and
coverage, not bandwidth** — you add one because there is nowhere to put the next person, or because
somebody walked out of range, never to go faster.

**But the ceiling is softer than the formula says.** On the night of 2026-10-01, as the batteries ran
down:

```
 1.7 h   3 APs                 6 attached   AP0:2  AP1:2  AP2:2
 7.2 h   2 APs (Everest gone)  6 attached   AP1:3  AP2:3
 8.6 h   1 AP (only Fuji)      6 attached   AP1:6
 9.7 h   1 AP                  6 attached   AP1:6
10.1 h   Fuji dying            1 attached
```

Nobody was orphaned. All six crowded onto the last access point and stayed there for an hour and a
half. An earlier note in this repository said four attached and two were stranded; that was a snapshot
taken during a bad moment and it was wrong — the same error as reading the final row above, where Fuji
was minutes from flat, as if it described the steady state.

**Attached is not the same as served.** Fuji held six at a 3 KB low-water mark. On the following night,
with the same enforcement in place, messages *to* handhelds on Everest were lost far more often than
anywhere else:

| Receiver's AP | Messages | Lost |
|---|---|---|
| Everest (0) | 132 | **16 — 12.1%** |
| Fuji (1) | 177 | 1 — 0.6% |
| Denali (2) | 167 | 3 — 1.8% |

Sending *from* Everest was fine at 1.6%, so it is inbound delivery that suffers. Twelve of those sixteen
happened while Everest was mid-fault from its own armed chaos, which is legitimate; **four were not**.
Four is not a proof, and it is recorded as a suspicion rather than a finding: an access point that
accepted its sessions when it was comfortable and is now at 7 KB has nothing left to buffer an arriving
message with.

## The gap underneath all of it

`node_station_room()` is evaluated **when a session is accepted, and never again**. Capacity is enforced
at the door and not afterwards. An access point that had room at the moment each handheld arrived can be
squeezed as those sessions fill, and it has no way to respond: no mechanism sheds a session, degrades
one, or refuses to *serve* rather than refusing to *join*.

That is what killed Denali on 2026-10-01 — it advertised no room, took a fifth handheld, and then
**dropped every session at once**, twice in forty minutes, rather than refusing one. Admission control
without load shedding turns a gradual squeeze into a cliff.

**Reliability, as the owner defines it, is being able to use the system even when it is degraded.** A
system that can only accept or refuse has no way to be degraded-but-usable; it is either fine or it has
dropped everybody.

## When is a grid degraded?

Not "one access point left". The useful definition is about slack:

> **A grid is degraded when losing one more access point would leave somebody with nowhere to go.**

With three access points that triggers at two, which matches the intuition that anything below the full
complement is degraded. It keeps working with four or ten, it is computable from what the beacon already
carries, and it catches the case a count misses — three access points where two are full is also
degraded. It also tells the handheld what to display: not "one AP left" but **"no spare capacity,
messages may be delayed"**.

A handheld that hides its degradation is not trustworthy. The same reasoning fixed `No LocalGrid AP in
range`, which was said while an access point sat in range at -47 dBm and merely full.

## Ways out, weighed

### 1. Take turns: rotate the sessions

Hold four sessions and cycle the rest through them. Disconnecting genuinely frees the memory — lwIP
releases the control block and windows, the driver releases the station state — so the premise holds.
The arithmetic is strongly in favour:

| | Memory on the AP |
|---|---|
| One attached handheld | **~4,500 bytes** |
| One queued text message | **~200 bytes** |

**About 20:1.** Parking a dozen messages costs less than one idle session.

**Prerequisite:** something must hold messages for whoever is off-slot, and D13 says there is no
store-and-forward. This is therefore not a scheduling feature with queuing as a detail — it is
**store-and-forward with scheduling as a detail**.

**Costs.** Six handhelds through four slots leaves each off a third of the time; on a 60 s cycle a
message waits 20–40 s. Acceptable for text, **fatal for live voice**, which needs a held session.
Reassociation itself is cheap: the logs show join-to-registered in about **0.6 s**, and fixed addresses
mean no DHCP, so a 0.6 s cost on a 60 s cycle is about 1% overhead. An SOS must pre-empt the rotation
and take a slot immediately — the same rule the outbox already uses, where an alert takes the oldest
slot because raising an alarm outranks a delivery receipt.

**Where the decision lives:** the access point advertises the rotation (the discovery beacon already
carries a free-slots byte), and handhelds self-schedule against it with jitter. Policy central, action
local, no new control protocol.

### 2. Sessions for conversations, not for existence

Rather than scheduling a scarce resource, make it unnecessary. A handheld that is not talking drops its
session but stays registered in a presence record of a few dozen bytes, and takes a session only when a
conversation starts — reconnecting on demand in 0.6 s rather than waiting its turn.

We already have proof the shape works: **a handheld on LoRa holds no Wi-Fi session at all** and still
sends, receives and raises an SOS. It is slow, but it is reachable at nearly zero per-device cost.

LoRa cannot be the general mechanism — the radios cost real money and most handhelds will not have one —
but it demonstrates that reachability does not require a session.

### 3. Signalling before payload

Send a small "I want to talk to X" and move the payload only once the recipient is known to be there: a
control plane / data plane split, as SIP is to RTP, or SMTP's `RCPT TO` to its `DATA`.

**It does not fix the held sessions.** The access point holds sessions so it can *deliver*; a sender
needs a connection only while sending. This changes the sending side, which was never the expensive one.

**And it only pays when the payload dwarfs the signal:**

| Payload | Size | Worth a handshake? |
|---|---|---|
| Text message | ~200 bytes | **No** — the handshake is a sizeable fraction of the message, and costs a round trip |
| Voice note, 30 s (Codec2 1200 bit/s, D72) | ~4,500 bytes | **Yes** — the intent is about 1% of it |
| Live push-to-talk, 30 s (IMA ADPCM, today) | ~120,000 bytes | Not applicable — live talk holds a session anyway |

So: text *is* its own notification and should simply be queued; the **voice note** earns the handshake,
because buffering 4.5 KB for somebody who may never appear is what an access point with 3 KB free cannot
do.

The two voice numbers are different things and the distinction matters. **Live push-to-talk** is what is
built: IMA ADPCM, 100 ms of 8 kHz mono per frame, 400 bytes (`LG_VOICE_DATA_MAX`), so 4,000 bytes a second
and about 120 KB for thirty seconds. It holds a session for its whole duration by nature, so a handshake
would add nothing. The **voice note of D72** is the unbuilt one: Codec2 at 1200 bit/s, about 4.5 KB for
thirty seconds, recorded then sent as a message, and *that* is the payload worth announcing before it
travels — and the only one small enough to cross LoRa at all (`docs/lora.md`, "Sizes, settled").

**Decided, 2026-10-02: not built yet.** The voice case is agreed to be right and is deliberately
deferred; the current behaviour stays.

### 4. Paging by broadcast — the idea worth keeping

Following the signalling idea one step further gives the one asymmetry that actually helps:

> **A session costs the access point memory per device. A broadcast costs it nothing per device.**

Sessions are O(N); broadcast is O(1). The access point already broadcasts a discovery beacon that every
handheld reads **while scanning**, including handhelds that are not associated at all. A **"these devices
have mail" bitmap** would be about **2 bytes for 15 devices** and would reach everyone in range at zero
per-device cost. A detached handheld sees its bit, associates, pulls, and leaves.

This is the only option discussed that reduces access point memory **without adding a mode, a schedule,
or a second code path**.

**Open question:** a broadcast list of who has mail is readable by anyone scanning, which sits awkwardly
with the care taken elsewhere about not leaking who is where (D21). Device indices rather than names
helps; it needs thinking about before it is built.

### 5. Two modes in parallel

Keeping today's always-connected behaviour and a cheaper mode beside it, with the admin choosing.
Feasible, and probably better **per device and automatic** than as a global switch: a handheld doing
push-to-talk needs a held session, one in a pocket does not, and the access point already knows its own
pressure. **Not decided.** The honest cost is that two modes are two code paths and twice the test
surface, including the transition between them, which is where the bugs would be — three of the four
bugs found in the week of 2026-09-30 were something stuck in a state nothing cleaned up.

## What does not help, and why

**Fewer access points do not free memory.** Covered above: the saving is backbone state, the cost is
sessions, and sessions win by an order of magnitude.

**Dropping TCP does not save battery.** The handheld already sleeps its radio between beacons —
`esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` in `hh_service.c` — so an idle socket costs almost no power. And an
access point must beacon continuously whether it serves one handheld or six, so it cannot sleep at all.
**Lazy registration buys headroom, not runtime**; they are different scarce resources and conflating them
leads to the wrong work.

The real handheld battery win is sleeping for *seconds or minutes* rather than milliseconds between
beacons, and the only thing preventing that is that a sleeping handheld misses messages — which the
access point would have to hold. **Store-and-forward again.** The battery payoff comes from the sleeping,
not from the disconnecting.

## The cheap wins that need none of this

These reduce memory without changing any behaviour, and should be measured before any design above is
built:

- **The TCP windows are sized for a different protocol.** 2,880 bytes each way per connection, for
  200-byte messages. Lowering them bounds the worst case — which is precisely the moment an access point
  collapses — and on a local link the throughput cost is irrelevant to text and tolerable for a voice
  note.
- **AMPDU aggregation is on** (`ESP_WIFI_AMPDU_TX_ENABLED`, `_RX_`). Aggregation exists to batch large
  frames; our messages are 200 bytes and never fill one. Its buffers are not free.
- **`LWIP_MAX_SOCKETS=24`** for a grid that serves six handhelds and one listener.
- **Task stacks are unmeasured on the access point.** The handheld reports its worst-case headroom
  (`UI task: 4204 bytes of stack unused at its worst`); the access point's tasks — `lg_core` at 10,240
  bytes, plus `lg_blelink`, `gps`, `lora` — do not. Trimming against a measured high-water mark is free
  memory with no behavioural change at all.
- ~~**The web admin is resident and used perhaps once a week.**~~ **Done, D91 (2026-10-03).** It was
  measured first: resident, Denali sat at 14,004 bytes free with one handheld attached and advertised
  room for nobody, and six handhelds cycled through join / accepted / closed. The page now starts only
  in setup mode or on request (`web on`, or `OP_WEB` over BLE) and stops itself when its window closes.
- **D73 decided the GPS would be sampled rather than resident** — its UART driver and buffers released
  between readings — and it is **not implemented**: `gps.c` still runs a resident task. A decision already
  taken, with the memory still being spent.

## Where this leaves the product

The queues are the foundation. Store-and-forward is needed for rotation, for on-demand sessions, for
handhelds that sleep properly, and it is independently the biggest functional gap in the product — D13
rejects a message to somebody who is merely out of range, which is the first thing anybody will try.

So the order is: **measure and take the free memory first**, then build the queues, then choose between
rotation and on-demand sessions with real numbers rather than arguments. Paging by broadcast is the piece
to keep from the signalling discussion. The voice handshake is agreed and deferred.
