# 2026-10-03, twelve hours to three flat batteries: findings

Run `chaos-runs/20261003-101848`, seed 33252, 10:18 to 22:31. Three access points on battery, reached
only over BLE (D90); six handhelds on USB. It ran until every access point had gone silent.

**Nothing was changed during the run or because of it.** The owner's instruction was to list findings
until the batteries died, and to check the standing decisions against what the grid actually did. This is
that list. Each finding says what was seen, what it was checked against, and how sure the reading is.

## The run in numbers

| Measure | Value |
|---|---|
| Length | 12 h 12 min |
| Rounds executed | 35 of the 62 reached (17 skipped for want of an AP console, 8 not started, 2 outside audible hours) |
| Faults injected | 35: 11 backbone-off and 6 restarts over BLE, 3 holds and 5 pulses on handhelds, 3 handheld-on-LoRa, 3 no-backup, 4 alert rounds |
| Messages, both ends registered throughout | **940 of 944 delivered (99.6%)**; median 0.2 s, p95 1.0 s, max 22.4 s |
| Messages where an end lost its AP | 91: 35 delivered, 55 lost, 0 refused |
| Alerts | 4 announcements, 4 SOS, 4 all clears: **12 of 12 delivered** |
| Handheld crashes or brownouts | **none** |
| Outbox refusals (the 2026-10-01 leak) | **none** |

### How long the batteries held

| AP | Went silent | Battery life | Restarts in the run |
|---|---|---|---|
| Everest | about 19:46 | **9 h 38 min** of uptime | 0 |
| Fuji | about 20:45 | about **10 h 25 min** into the run (5 h 33 min since its last restart) | 2: one crash, one asked for |
| Denali | about 22:21 | **12 h 12 min** | 0 |

Everest's 9 h 38 min is the same figure, to the minute, as on 2026-10-01. It is the AP carrying both the
GPS and a LoRa module.

## Findings about the grid

### 1. The last access point advertised room it did not honour, and stranded two handhelds for 1 h 46 min

**The most important finding of the run.** When Fuji died, its four handhelds needed Denali. Two got in.
`fnk0104b` (Channi) and `e28b` (Marshall) never registered again: their last registrations are at
t=33,962 s and t=33,425 s, on Fuji, and the run ended at t=43,921 s.

For that whole time Denali's beacon said **"4 attached, 1 free"**. Each stranded handheld joined it, was
given a session, and was dropped at once, then retried ten seconds later:

```
[NET] Heard AP 2 "Denali" -58 dBm, 4 attached, 1 free, backbone no, grid time yes
[NET] Joining Denali (node 2, -58 dBm) as 192.168.4.101
[NET] AP 2 passed over for 120 s after this
[NET] Session receive error; searching again in 10 s
```

Across the run the handhelds logged **900 `Session receive error`** and **250 `The AP closed the
session`**, most of them these two against Denali.

- **Against D84** ("an access point advertises the room its memory can honour, and a handheld passes over
  one that turned it away"): the first half failed - the free-slot byte said 1 for 1 h 46 min while every
  attempt to use it was closed. The second half half-worked: the handheld logs "passed over for 120 s" and
  then rejoins the same AP ten seconds later, because it is the only one there.
- **Against D91** (admin page on request, to give the heap back to sessions): it raised the ceiling from
  one handheld to four. It did not reach six. On 2026-10-01 a single AP held all six, on the chaos build
  that had no web server compiled in at all.
- **Against `docs/capacity-and-memory.md`**: this is the gap it names - "capacity is enforced at the door
  and not afterwards" - seen from the other side: the door says yes and something after it says no.
- Marshall has no LoRa module and was simply offline. Channi has one and kept sending over it (finding 6).
- What closes the session is not visible: the AP has no cable, so its own reason (`sessions.c`: peer
  closed, receive error, no registration, idle timeout) was never read. **That log line is the next thing
  to get.**

### 2. Fuji crashed on its own, at 1 KB of free heap

About 14:30, with nothing asked of it, Fuji restarted with reset reason **"crash (panic)"**. Its lowest free
heap had reached 1 KB beforehand. In the twenty minutes before (14:09 to 14:21), every handheld had a
10-20 s stretch with no Wi-Fi session while all three APs were up and advertising room.

Reading: running out of memory is the likely cause and the burst of drops is the squeeze before it, but the
panic text was never seen - no cable - so this is a strong suspicion, not a diagnosis. After Everest died
and Fuji took four handhelds, its low-water mark was back at 1 KB within the hour.

### 3. Everest and Denali reported 0 KB lowest heap for the whole run and never restarted

Either both sat at the floor for twelve hours without falling over, or the beacon reports this field
wrongly (Fuji's moved: 2, 1, 9, 7, 5, 4, 1 KB). Not resolvable from this run. If it is real, finding 2
says how close each was.

### 4. Handhelds lose their sessions far more often than they are faulted

**118 registrations and 107 `handheld_down` events** across six handhelds, against 35 injected faults of
which only 8 touched a handheld directly. Roughly twenty re-registrations per handheld in twelve hours. This
is the `Session receive error` the owner saw on 2026-10-02 before the run; D91 reduced it and did not
remove it. Most of the excess is finding 1, but the churn was already there at five hours (65 registrations
against 12 faults) with three healthy APs.

### 5. Unexplained losses with both ends registered and no fault on either AP

Four of the 944 clean messages were lost. One fell during a fault round elsewhere. Three had nothing to
explain them:

- `e28 -> fnk0104b_c` at t=29,175 s and `e28b -> fnk0104b_c` at t=30,989 s: the receiver was on
  **Everest** both times. Everest was on the pre-D91 firmware with one session slot, and it is where the
  unexplained inbound losses clustered on 2026-10-02 (16 of 132, against 1 and 3 on the others).
- `fnk0104b -> e40` at t=33,046 s, APs 1 and 2.

Three is a small number. Two of the three landing on the same handheld on the same AP, twice in a row and
again on the AP that showed it before, is a pattern worth one targeted test.

### 6. The backup radio carried traffic that Wi-Fi should have: 48 of 114 messages

114 messages crossed with an end on LoRa; 59 arrived. **48 of them were sent while an access point was up
and advertising room.** Nearly all are Channi during finding 1: it could not get a session on Denali, so
it used LoRa, and from the outside nothing looked wrong. This is the case the audit was built for
(owner, 2026-10-01: "if it is using a backup we need to make sure that there was a good reason"). Marshall,
in the same position without a module, shows what that looks like with no backup: offline.

### 7. Receiving over LoRa is still weaker than sending

Three rounds with a handheld on LoRa alone:

| Direction | Messages | Delivered |
|---|---|---|
| From the LoRa-only handheld | 25 | **25** |
| To the LoRa-only handheld | 25 | 19 |

Same direction as 2026-10-02 (71 of 71 out, 56 of 71 in). The handheld's own counters point at
reassembly again: 14,823 to 26,954 ms of airtime per round and parts counted in but not completed.

### 8. Eleven messages refused "(time)" in the final minutes

`Refused by the AP (time)` appears 11 times, on five handhelds, clustered at the very end of the run. D6
makes an AP refuse ordinary messages from a handheld more than two minutes off its clock, or while its time
is unset. The likeliest reading is Denali restarting on a dying battery and coming back without its clock
(D53, D60), but its restart counter never moved in the beacons, so this is unconfirmed. If the AP did lose
time, the refusals are D6 working; if it did not, they are a fault.

## Decisions checked, and how they held

| Decision | Held? | Evidence |
|---|---|---|
| D6, time rule | probably | finding 8 |
| D13, no store-and-forward | as designed | the 55 losses where an end lost its AP are this decision's cost, measured |
| D56 / D88, announcements and voice to everyone | **yes** | spoken announcement from Jolly: 51 frames, heard 51/51 by all five listeners; no handheld played a talk it was not addressed in |
| D66 / D79, SOS and all clear | **yes** | 4 of 4 and 4 of 4 delivered |
| D71, voice never on LoRa | not exercised | no voice round fell in a LoRa window |
| D74, alerts on both radios | not measurable | no AP-side LoRa counters without a cable |
| D84, advertised room is honoured | **no** | finding 1 |
| D87, quiet hours | **yes** | alert and voice rounds before 14:00 and after 21:00 were skipped and say so |
| D90, faults over BLE | **yes** | Fuji and Denali acknowledged every fault; Fuji's restart confirmed by its boot counter, 1092 to 1093 |
| D91, admin page on request | partly | four per AP instead of one; not the six of 2026-10-01 |
| Touch-bus fix on the E40 | **yes** | Big Pepa: no assertion, no restart, in twelve hours |
| Outbox fix of 2026-10-01 | **yes** | zero refusals for a full outbox |

## What this run could not test

- **Everest was on the old firmware** (its USB was dead when the others were flashed). It refused all nine
  faults asked of it, held one handheld, and the harness went on asking it after its battery was flat.
- **The AP-to-AP LoRa leg: untested for the third run running.** All 17 AP-side LoRa rounds were skipped
  because they type at a serial console the APs do not have on battery. Once, five in a row (#25 to #29)
  meant an hour with no fault injected at all. `OP_FAULT` can take an AP's LoRa down; the harness does not
  yet route those rounds through it.
- **Group messages and group voice.** The harness found no group with every handheld in it and left group
  traffic out. Only one talk ran in the whole run, and it was an announcement; 1:1 voice was verified by
  hand on 2026-10-03 and group voice has never been exercised on hardware.
- **A degraded grid under fault.** After Fuji died the harness refused eight rounds as "grid still not
  steady", because two handhelds were stranded. The run therefore measured the one-AP grid at rest, not
  under attack.

## Harness faults, so the report is not read wrongly

- **The failover figure is not a measurement.** The summary says "23 moves, median 2,127 s, max 10,954 s";
  `events.jsonl` holds no failover event at all. The times track elapsed run time: a marker set by the BLE
  fault round is never cleared.
- **The backup audit's headline undercounts.** "2 unjustified" counts only episodes past 45 s; the table
  beside it lists the short ones. Finding 1's 1 h 46 min is in there as one row.
- **"Experiments: 35"** excludes skipped rounds, which made a healthy run look stalled at two hours.
- **The battery table says "still up at the end"** for Everest, which had been silent for hours; only the
  last two APs to die are marked flat.
- **"Backbone whole again after an AP fault: none"** and "AP-to-AP echoes: 0 of 0" are the absence of a
  cable, not a result.

## What to look at first

1. **Why Denali closes a session it has just accepted** - one AP on a cable, six handhelds, read
   `sessions.c`'s close reason. Finding 1 and most of finding 4 are probably this.
2. **Fuji's panic** - the same bench, with the AP on a cable, until it reproduces. Findings 2 and 3.
3. **Everest onto the current firmware**, then repeat the Everest-bound traffic of finding 5.
4. **Route the AP-side LoRa rounds through `OP_FAULT`**, so the backbone leg of D71 is finally tested on
   battery.
