# Prototype milestone 8: the second radio carries a handheld's own traffic

Status: **verified on hardware.** A handheld with its Wi-Fi switched off sends and receives messages
over LoRa, and the acknowledgement comes back the same way. Measured on the bench on 2026-09-30 with
all three access points on battery and no serial cable between them and this laptop.

This milestone is the handheld half of D71's second radio. The access-point half (LoRa between APs,
D71/D74) was already built; what was missing was the device in someone's hand, which is the case the
radio exists for: a handheld out of Wi-Fi range that still has to reach the grid.

## What was wrong

The radio worked in one direction only, and the wrong one was broken.

With a handheld's Wi-Fi switched off (D78), a message sent *to* it arrived over LoRa, was decoded, was
shown on its screen and sounded its notification. Its own reply never reached the air: **one frame went
out in 150 seconds, and that one was its announcement.** The reply waited in the outbox and left over
Wi-Fi seventy seconds later, the moment a session came back — which in a message list looks exactly
like success.

Three gates in `lg_core`, each of them correct for a socket:

| Gate | Where | Why it was wrong here |
|---|---|---|
| `if (!c->connected) return false;` | `send_frame()` | A handheld on LoRa has no session and never will — it is not a backbone peer (D77). On that radio "not connected" is the normal state. |
| `if (c->registered) transmit(...)` | `lg_client_send_text()` | Registration is one of the types LoRa refuses (an AP would have to trust a handheld about it), so a LoRa-only handheld is unregistered by definition. |
| `if (!c->registered) return;` | `lg_client_tick()` | Nothing was ever retried either, and one LoRa attempt is not a delivery. |

`lg_client_lora_announce` had already been carved out of the first gate when it hit the same wall. That
carve-out should have been read as evidence that the gates were in the wrong place.

## What changed

| Area | Change |
|---|---|
| `components/lg_core/src/lg_client.c` | `send_frame()` offers the frame to `io.send` whether or not there is a session, and still returns false unless a session took it. `transmit_ex(c, i, may_reject)`: an attempt with no session cannot reject the message, because a missing peer key before registration is the normal state. `lg_client_tick()` retries with no session. |
| `components/lg_core/include/lg_client.h` | `LG_RESEND_LORA_MS` 10 s and `LG_RESEND_LORA_MAX_PER_TICK` 1: the retry rate is bounded by messages per pass, oldest sequence first, not by the interval. |
| `tests/target/main/sim.{c,h}` | `sim_lora(s, client, node)` gives a detached handheld a LoRa path, and `offers_detached` counts what the core handed to the glue. The simulator does what an AP's glue does, in the same order: mark the device reachable over LoRa *first*, then hand the frame over on a session named by the device. |
| `tests/target/main/test_messaging.c` | Four scenarios: a LoRa-only handheld originates; its acknowledgement goes back; a handheld with **no** module is unchanged; retries are paced. |
| `tools/chaos.py` | The `lora_handheld` round, the handheld `lora` status block in the parser, and a run that works with no access point on USB. |
| `firmware/handheld/main/ui/ui_overlay.c` | A notification is no longer covered by the screen saver it just woke (owner report, same day). |
| `firmware/node/main/node_main.c` | The advertised free-slot estimate re-measured: 4.5 KB per station, 10 KB floor. Built, **not yet on the access points** — they are on battery. |

Nothing in the wire format changed. Identity is still (origin_id, origin_boot, origin_seq), and the
outbox keeps each message's stored `(boot, seq, grid time)`, so a later Wi-Fi attempt at something LoRa
already delivered is a duplicate the grid drops by identity rather than a second message. A scenario
fails if that stops being true.

## Build and flash

```
. C:\esp\v6.1\esp-idf\export.ps1
python tools/build.py --role H
python tools/flash.py --role H
python tools/flash.py fnk0104b_c --firmware tests      # the on-board tests, on a board with a module
```

## Test procedure

**1. The on-board tests.** `LG_TESTS_RESULT: PASS`, 0 failures. The four LoRa scenarios are in
`test_messaging.c`; they fail if any of the three gates above is put back.

**2. By hand, with two handhelds that have modules.** Hold both ports open so the counters are not
reset by reopening:

```
python tools/serial_capture.py --ports COM9 COM22 --seconds 150 \
  --send "COM9@8:wifi off 110" \
  --send "COM9@30:send 9 reply over LoRa please" \
  --send "COM22@70:msgs 2" --send "COM9@85:lora"
```

Expected on the handheld whose Wi-Fi went off:

```
[NET] Wi-Fi switched OFF on request (D78); LoRa carries whatever it can
[MSG] Announced over LoRa (no Wi-Fi session)
[MSG] Sent to device 9: 1:1 boot 597 seq 12, 22 bytes
[MSG] Frame over LoRa from AP 1, 45 bytes
[MSG] Delivered to the other handheld: outbox slot 0
```

and on the other, `from Channi (device 1) [received]: reply over LoRa please`. The delivery receipt
coming back is the part that matters: it is the difference between delivering blind and the sender
learning its message landed (D80).

**3. Repeatedly, under chaos.** This round needs no access point on USB, so it is the one that works
with the access points on battery:

```
python tools/chaos.py --hours 0.5 --live --lora --lora-share 1.0 --no-alerts --min-gap 1 --max-gap 2
```

Each round switches one handheld's Wi-Fi off for 90–300 s and sends messages **both ways**, requiring
delivery. Both ways matters: receiving was never the broken direction.

## Measured

| | |
|---|---|
| Round trip, message out and acknowledgement back | **2.7 s** (2.7–2.9 s typical) |
| Outbound delivery, retries working | **28 of 28** over three rounds on both boards |
| Inbound delivery over the same rounds | 25 of 28 |
| Whole-run delivery, ordinary traffic included | 95% |
| A message recovered by a retry | 12.85 s end to end |
| One-way delivery, median | **1.3 s** (range 1.0–2.0 s) |
| Time from Wi-Fi off to the first access point answering on LoRa | 1.5–4.7 s |
| Radio settings | SF9, BW125, CR4/5, 868.5 MHz, 22 dBm, preamble 12 |
| RSSI and SNR on the bench | −21 to −41 dBm, SNR 7–10 |
| A 12-byte 1:1 message on the air | 45–60 bytes sealed, 1 part |

## Known limitations

- **Single-attempt delivery is about three quarters; with retries, outbound was 28 of 28.** Nothing acknowledges
  at the LoRa link layer, so a lost frame is simply lost until something retries. The same round, same
  seed, same boards: 6/9 outbound with 21 frames on the air, twice over, then **9/9 with 28 frames** once
  the retry pass actually ran. One message is visible being recovered — sent at 251816 ms, receipt at
  264666 ms, 12.85 s, which is a first attempt lost and the +10 s retry landing.
- **A handheld cannot know whether LoRa works until it has already lost Wi-Fi**, because an access
  point only learns the path from an announcement and only answers one. Losing that single frame costs
  up to one announcement interval.
- **A message that waits more than two minutes can never be delivered**, by any path. D6's clock rule
  (`absdiff(ap_clock, frame.grid_time) <= 120 s`) against the invariant that a retransmission reuses its
  original grid time: the stamp can only get older, so the refusal is permanent. Six messages hit this in
  the first real chaos run, every one on the Wi-Fi retry of something queued during a LoRa window. The
  owner's decision is open - a message that arrives possibly twice, or not at all - and until it is
  settled the 10 s retry interval matters more than it looks, because it is the only interval whose
  second and third attempts still fall inside the tolerance.
- **The asymmetry has reversed, and the remaining side is the next thing to fix.** Outbound is now 100%
  and **inbound 89%** (25 of 28), because only outbound got a retry budget of its own. A message *to* a
  LoRa-only handheld still depends on the sending handheld's Wi-Fi resend driving the access point to
  transmit again, which helps only while that sender keeps trying. Giving the access point its own retry
  for a handheld that has not acknowledged is a well-defined change and an owner decision, since it means
  an access point holding state about an unacknowledged frame.
- ~~Outbound from a handheld is lossier than inbound to it, 66% against 83%.~~ **Explained and fixed:**
  one direction had retries and the other did not. A message *to* a LoRa-only handheld is sent by a
  registered handheld whose outbox resends every 3 s over Wi-Fi, and each resend makes an access point
  transmit over LoRa again. Outbound had a single attempt, behind four stacked gates — three in `lg_core`
  and a fourth in `hh_service`, which returns early when Wi-Fi is off, above the line that drives the
  outbox. The fourth was found by arithmetic rather than by reading: the round reported the same 6 of 9
  **and the same 21 frames on the air**, which is nine sends plus six acknowledgements plus announcements
  with no room for a retransmission.
- ~~The handheld still reports `SEARCHING` while it is carrying traffic over LoRa.~~ **Fixed the same
  day:** `HH_LINK_LORA` is reported by `publish()` when the module has heard an access point recently,
  so the console says `ON LORA (no Wi-Fi)` and the launcher says "<name> on LoRa". The service's own
  state machine never enters it, so nothing inside the service changed meaning.
- **No voice over LoRa.** Live push-to-talk is impossible (D71) and the 10-second note (D78) is still
  compiled off: `LORA_LONG_PAYLOAD` is 0.
- **Range is untested.** Every measurement here is from one bench with every board in sight of every
  other. The distances on the project site remain estimates.
- ~~The access-point capacity fix is not on the boards.~~ Flashed by cable on 2026-10-01 and verified:
  advertised room went from `1 attached, 6 free` to `2 attached, 3 free`.
- **Advertised capacity is advice, not a limit, and the night proved it matters.** With one access point
  left, Denali advertised `0 free`, accepted a fifth handheld anyway, and **dropped every session at once**
  twice in forty minutes at a 0 KB heap floor - without restarting. `build_discovery` advertises what the
  heap can honour; `sessions.c` still accepts up to `LG_PROTO_MAX_STATIONS` (15). Refusing a session beyond
  what the heap can serve is the fix, and it is the owner's call because it decides how many handhelds an
  access point will carry. The measured ceiling on these boards is **four**.
- **Battery, measured to the end** (2026-09-30/10-01, 5000 mAh packs): Everest **9 h 38 min** with a GPS,
  Fuji **11 h 34 min**, Denali **12 h 42 min**. An estimate of 28-45 h was wrong by three to five times.
  Pack-to-pack spread was about an hour, so the GPS's two-hour cost is real but not cleanly separable from
  it without swapping packs between boards.
