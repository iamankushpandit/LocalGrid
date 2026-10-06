# 2026-10-05 and 06, six runs: making a message survive on LoRa alone

Six cabled runs over two days, all nine boards on USB, the LoRa radios at low power (5 dBm, D98) from
the third run on. They began as a check of D102 and ended four fixes later. Every round in every run
reached steady state again; what moved was how many messages between registered handhelds arrived
while the Wi-Fi backbone was switched off.

The raw runs are in `chaos-runs/` (not in git). Run folders are named for their start time.

## The runs

| Run | Seed | Length | Rounds recovered | Messages, both ends registered | Findings |
|---|---|---|---|---|---|
| `20261005-122444` | 68438 | 3 h | 15 of 15 | 265 of 267 | 3 |
| `20261005-161946` | 86676 | 1 h 15 | 7 of 7 with an outcome (an eighth, `no_net`, has none) | 99 of 99 | 2 |
| `20261005-221330` | 80569 | 45 min | 4 of 4 | 63 of 63 | 0 |
| `20261005-232511` | 32610 | 1 h 30 | 6 of 6 | 109 of 122 | 5 |
| `20261006-013041` | 32610 | 1 h 30 | 6 of 6 | 110 of 122 | 5 |
| `20261006-122257` | 32610 | 1 h 30 | 6 of 6 | 113 of 122 | 1 |

The last three are the same schedule: seed 32610, the `lora` scenario, seven rounds planned and six
run (the seventh could not finish before the end). They are the comparison. No alerts were raised in
any of the six; that path is not measured here.

Backbone whole again after an access point fault: median 1.3 to 2.2 s in every run. No crash,
watchdog or brownout that the harness had not caused.

"Messages, both ends registered" counts every message whose sender and recipient both held a session
throughout. In a `lora_blind` round one access point has ESP-NOW *and* LoRa off, so a message to a
handheld on it cannot arrive and is not a finding. That is what most of the gap between 113 and 122
is taken to be; the individual messages were not each checked.

## What each run found

### 1. `20261005-122444`: a full queue threw out messages (before D102)

Two direct messages lost in a `lora_only` window. The three access points' LoRa queues had dropped
5, 12 and 8 frames: a full queue made room by dropping its oldest entry, whatever it was, so a
presence update pushed out a person's message. D102 ranks them: alert, message, routine.

### 2. `20261005-161946`: no message lost, two link findings

99 of 99. Two findings about LoRa links not coming back after a window; not reproduced since.

### 3. `20261005-221330`: the heartbeat starved

No message lost, but Fuji's LoRa links were called dead by both neighbours for the three minutes it
carried a LoRa-only handheld. A heartbeat is routine traffic, so D102 let a queue full of messages
refuse it, and it then waited a minute for its next turn; three in a row is a dead link. A heartbeat
now asks whether there is room and, with none, comes back in half a second. In the three runs since,
no LoRa link dropped that a fault had not taken down.

### 4. `20261005-232511`: an access point claimed a handheld it did not have

One direct message lost **with no fault active**. The recipient had moved between access points
early in the run and, in the half second without Wi-Fi, announced itself over LoRa. That frame is a
second on the air and arrived after it had registered again; the access point that heard it claimed
the handheld as reachable by LoRa through itself, no other access point agreed, and nothing settled
it. For ninety minutes that access point sent the handheld's direct messages by LoRa alone: 28
arrived, one did not. Afterwards the boards still disagreed about where the handheld was.

Fixed in D80: a session beats a frame heard over LoRa, which beats nothing, and an access point does
not claim a handheld that has a session on one that is up.

### 5. `20261006-013041`: frames sent once, and gone

With the presence fix, nothing was lost outside a fault window. Five messages were still lost inside
`lora_only` and `lora_blind` windows. A new counter said the queues had dropped 16, 24 and 30 frames
and that **none of them was a message**; no heartbeat had waited. The messages had been transmitted
and had not arrived: a frame between access points went on the air once, on a channel three radios
share without listening first.

D104: a message between access points is acknowledged, and sent again until it is.

### 6. `20261006-122257`: one left, and not a radio loss

One finding, and it was the same message in all three runs of this schedule: number 30, sent 1.5 s
after the backbone is switched off. For six seconds after ESP-NOW stops, an access point still
believes its links are up, so LoRa is not offered the message, and the ESP-NOW copy goes nowhere.

D105: a message ESP-NOW could not deliver is handed to LoRa at once.

## Same schedule, three builds

| | Before D80 fix | With it | With D104 too |
|---|---|---|---|
| Run | `20261005-232511` | `20261006-013041` | `20261006-122257` |
| Delivered, both ends registered | 109 of 122 | 110 of 122 | 113 of 122 |
| Findings: lost with no fault active | 1 | 0 | 0 |
| Findings: lost in a LoRa window | 4 | 5 | 1 |
| Slowest delivered message | 1.9 s | 2.8 s | 9.4 s |
| Routine frames dropped by the queues (Everest, Fuji, Denali) | 19, 29, 28 | 16, 24, 30 | 20, 41, 52 |
| LoRa airtime (Everest, Fuji, Denali) | 147, 154, 168 s | 122, 167, 178 s | 144, 189, 206 s |
| AP-to-AP echoes that reached every AP | 18 of 59 | 15 of 59 | 11 of 59 |

## What acknowledging cost

From the `lora` counters of `20261006-122257`, per access point (Everest, Fuji, Denali):

| | Everest | Fuji | Denali |
|---|---|---|---|
| Frames every awaited AP acknowledged | 14 | 13 | 22 |
| Sent again | 21 | 31 | 36 |
| Given up after three times on the air | 5 | 13 | 10 |
| Acknowledgements sent | 22 | 34 | 31 |
| Acknowledgements received | 17 | 17 | 24 |

- **Given up.** Half the rounds are `lora_blind`, where one access point's LoRa is off on purpose and
  can never answer. Those are believed to be most of the give-ups; they were not separated from
  genuine failures.
- **Acknowledgements lost.** Between a quarter and a half did not reach the sender. The channel is
  crowded: the three radios were on the air for 144 to 206 s of roughly 1,080 s of windows.
- **Routine traffic gives way.** Queue drops of routine frames rose, and the harness's own echoes,
  which are routine, crossed less often. That is the ranking doing what D102 says.

## Not measured, and still open

- **D105 has no chaos run.** It was checked on the bench: backbone off on all three access points, a
  group message one second later, all five other handhelds received it. The comparison run was
  stopped at the owner's word to finish implementation first.
- **A neighbour that acknowledges at the radio while its firmware takes nothing in** still looks
  healthy for six seconds. That is what `bb off` looks like from the *other* side, so the harness can
  still show a loss of this kind; an access point that is off or out of range does not behave so.
- **Alerts** were switched off in all six runs.
- **Between an access point and a handheld on LoRa alone** nothing is acknowledged at the link; the
  handheld's own retry and the delivery report cover it.
- The stopped run (`20261006-155301`) raised one finding in its first two minutes, before any fault:
  a handheld with a LoRa module had no Wi-Fi session for 50 s with all three access points up. Not
  investigated.
