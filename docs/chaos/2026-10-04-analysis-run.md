# 2026-10-04, thirty minutes with every access point on a cable: what the theories look like now

Run `chaos-runs/20261004-003820`, seed 21, 00:38 to 01:09. All nine boards on USB and on one build - the
firmware gate checked it before anything was injected. Five rounds, all recovered. No firmware was changed;
this run exists to replace inference with what the access points say on their own consoles.

No alert or voice round ran: it was after midnight, inside the quiet hours of D87.

## 1. An access point invites a handheld it will always refuse - confirmed, with the numbers

Round #5 held Everest and Denali in reset for 338 s, leaving Fuji alone with six handhelds.

- Two were already on Fuji and stayed. Two more got in within 10 s. **Big Pepa and Jolly never got in**:
  35 and 36 join attempts each, one every ten seconds for the whole window, 358 s and 362 s without Wi-Fi.
- Fuji's beacon said **"4 attached, 1 free"** on 14 of the 17 scans those two made.
- Fuji's console refused them **67 times**: `No heap to serve another handheld (13 free KB); refusing
  connection`. Free heap at the refusal: 13 KB (45 times), 11 KB (17), lower five times.

The mechanism is arithmetic, not chance. `node_station_room()` is `(free - 10240) / 4608`:

| Moment | Free heap | Formula | Outcome |
|---|---|---|---|
| Idle with four attached | 16 KB (the commonest reading) | 1.3, so **1** | beacon says "1 free" |
| A fifth has associated and opened its socket | 13 KB | 0.6, so **0** | door says "no heap", closes it |

The arriving handheld costs about 3 KB before the door looks, and that is exactly the margin the invitation
was made on. With four attached, a fifth is always invited and always refused.

**Measured free heap by handhelds attached** (all three access points, one-second samples):

| Attached | 0 | 1 | 2 | 3 | 4 | 5 (briefly, before refusal) |
|---|---|---|---|---|---|---|
| Median free | 22 KB | 24 KB | 21 KB | 19 KB | 16 KB | 13 KB |
| Lowest seen | 20 | 16 | 8 | 10 | 11 | 12 |

So a session costs about 2.5 to 3 KB at rest, not the 4.6 KB the formula charges - but Fuji's own
low-water mark fell to **2.9 KB with only four attached**, so the transient dips are far larger than the
resting cost. Six on one access point is not safe on today's heap: the memory has to be found before the
formula is loosened.

**The handheld does not back off.** It logs "AP 1 passed over for 120 s" and rejoins the same access point
ten seconds later, because nothing else is in range: 28 and 29 such cycles.

## 2. No panic, no brownout, no restart that was not asked for

Thirty minutes is too short to reproduce Fuji's crash of 2026-10-03, and it did not happen. All three
access points held on USB with nine boards on the hub. Fuji's 2.9 KB low-water mark, with four attached, is
the same neighbourhood as the 1 KB it reported before that crash.

## 3. The "session churn" is mostly deliberate: the handhelds rebalance

Seven `[ROAM] Balancing: Fuji has N other handheld(s), Everest has N; moving` lines in thirty minutes.
This is what Marshall did at 62 s on the bench earlier. 23 registrations against 5 faults, of which about
8 are the faults themselves, 7 the balancer, and the rest the stranding above. Extrapolated to twelve hours
that is the order of the 118 registrations of 2026-10-03.

Whether a balancing move costs a message is not answered: no loss in this run coincides with one.

## 4. LoRa

**Access point to access point - works, for the first time on record.** Round #2 switched ESP-NOW off on
Everest and Denali and LoRa off on one of them. Echoes crossed Fuji to Denali and back over LoRa in about
1.0 s, and 7 frames arrived over LoRa before Wi-Fi delivered them.

**But the first 40 s after ESP-NOW died carried nothing.** An echo at +1 s and a group message at +5 s
were both lost; the first crossing was at +40 s. LoRa takes over only once the Wi-Fi link is judged down
(D74), and nothing bridges the gap.

**A handheld on LoRa alone**: 12 of 12 sent from it arrived, 11 of 12 direct messages to it. Counting
group messages too, two were lost inbound. Too few to say more than "still the weaker direction".

## 5. Failover, measured for real

| Round | Moves |
|---|---|
| #1, Everest restarted | 7.4 s, 7.8 s |
| #4, Fuji held 13 s | 7.2 s, 23.1 s |
| #5, two held 338 s | 7.4 s, 9.6 s, then **358 s and 362 s** (the two that were refused) |

A handheld that can get in moves in 7 to 10 seconds.

## 6. Group messages, tested for the first time

15 of 19 delivered. All four losses have a cause that is not the group path: one in the 40 s LoRa gap
above, one to a handheld on LoRa alone, two to the handhelds stranded in round #5.

## 7. Every loss in the run, accounted for

| Time | Message | Why |
|---|---|---|
| 263 s | group, Jolly to Big Pepa | backbone switching to LoRa (section 4) |
| 696 s, 742 s | direct and group, to Channi | Channi on LoRa alone (round #3) |
| 1262 s | direct, Big Pepa to Bluey | 5 s after two access points were cut |
| 1439 s, 1568 s | group, to Big Pepa and Jolly | both refused by Fuji (section 1) |

47 of 48 messages with both ends registered and no fault arrived.

## The instruments

- The firmware gate passed on nine boards and said so.
- The harness made the group it needed and used it.
- "5 run of 5 reached", real failover times, and a backup headline giving both counts all read correctly.
- **One gap found:** a LoRa round is not counted as a fault on the access points it touches, so the 263 s
  loss was reported as "lost while both were registered" with no fault. It should name the round.

## What this changes in the plan

The capacity fix is not one change but three, and the order matters:

1. **Stop inviting a handheld that will be refused** - advertise from the same figure the door uses, less
   the cost of arriving. Removes the 10-second hammering without admitting anybody new.
2. **Make the handheld back off** when the only access point in range has turned it away.
3. **Find the memory**, measured, before raising how many an access point admits - because the transient
   low with four attached is already 2.9 KB.

And one new item: **the backbone's switch to LoRa loses about 40 s of traffic.**
