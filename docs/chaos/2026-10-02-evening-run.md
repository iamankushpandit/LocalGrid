# 2026-10-02, seven and a half hours on battery: the backup audit earns its keep

Run `chaos-runs/20261002-143623`, seed 51134, started 14:36, **stopped by hand at 22:01 after 7 h 24 min**
so the night's new work could be flashed and a fresh run started. The batteries did not run out — all
three access points were still beaconing when it was stopped — so this is not a battery figure.

Six handhelds (`fnk0104b`, `fnk0104b_c`, `hosyond`, `e28`, `e28b`, `e40`) and three access points on
battery, armed over BLE with their own self-faulting chaos build (D82, D85). 30 experiments, **23
recovered, none left unrecovered**, 32 findings.

This was the first long run with the backup audit and the outbox fix in place, and it is the first run
whose most useful output is not the delivery rate.

## The headline: zero refusals, and a backup that was leaned on when it should not have been

| Measure | Value |
|---|---|
| Messages with both ends registered throughout | **644 of 670 delivered (96.1%)** |
| Latency, those messages | median 0.3 s, p95 1.7 s, max 23.1 s |
| Messages where an end lost its access point | 95: delivered 66, **refused 0**, lost 26 |
| Messages that crossed with an end on the backup radio | **164**, of which 137 delivered |
| Of those, ones Wi-Fi could have carried | **6** |
| Episodes with no Wi-Fi session | 55, **2 of them unjustified** past the 45 s threshold |

**Zero refusals over seven and a half hours.** The outbox leak that produced 176 refusals, three mute
handhelds and a refused SOS on 2026-10-01 did not recur once. That fix is holding.

**Six messages crossed on LoRa while Wi-Fi was available.** This is the number the owner asked for on
2026-10-01 — *"if it is using a backup we need to make sure that there was a good reason to use the
backup, because in some of these handhelds we won't have those radios"* — and it is the first run that can
answer it. Each of those six is a message a handheld without a second radio would have had **no way to
send**, on a grid that looked perfectly healthy from the outside because the traffic arrived anyway.

The audit also caught brief unjustified outages on boards that have **no** second radio at all —
`hosyond`, `e28`, `e28b`, `e40` — ten seconds each, with every access point up and no fault injected.
Ten seconds is short, but it is ten seconds of a handheld being unreachable for no reason anybody can
point at, and nothing before this audit would have shown it.

## Open questions, for the next run to answer

### 1. Inbound over LoRa is the weak direction now, and it used to be the other way round

Ten handheld LoRa rounds, every one recovered. Split by direction:

| Direction | Messages | Delivered | Lost |
|---|---|---|---|
| **From** a handheld on LoRa alone | 71 | **71** | 0 |
| **To** a handheld on LoRa alone | 71 | 56 | **15 (21%)** |

Sending from a LoRa-only handheld — the direction that was broken until 2026-09-30 — is now perfect
across 71 messages. Receiving is losing a fifth. The module's own counters point at reassembly:
`fnk0104b_c` ended the run with **1,558 parts in, 25 dropped and 34 reassembly timeouts**, against 190
parts out with nothing dropped. A part lost in a multi-part frame loses the whole message, and inbound is
where the parts pile up. Worth reading the part counters per round rather than only at the end.

### 2. Fuji reached 1 KB of free heap

| AP | Restarts | Lowest heap |
|---|---|---|
| Everest | 14 (its own chaos) | 4 KB |
| Fuji | **0** | **1 KB** |
| Denali | 13 (its own chaos) | 3 KB |

Fuji ran 7 h 50 min without a single restart and got down to **one kilobyte**. It never dropped its
sessions, so this is not the Denali failure of 2026-10-01 repeating — but it is the thinnest margin
measured on this bench, and it is the case `docs/capacity-and-memory.md` describes: capacity is checked
when a session is accepted and never again. Everest and Denali never got that low, and both restarted
repeatedly, which is the uncomfortable reading — **the access point that stayed up is the one that ran
out of memory.**

### 3. No access-point-to-access-point LoRa round ran

The LoRa section's backbone table is empty: all ten rounds were handheld rounds. "Backbone whole again
after an AP fault: none", "links kept by keepalive ACKs: 0" and "AP-to-AP echoes: 0 of 0" are all
consistent with that, so they are probably not measurement failures — but the backbone leg of D71 went
untested for the whole run and should be forced next time.

### 4. The 15-minute outage that cannot be read

`fnk0104b_c` shows one unjustified episode of **900 s**, far longer than anything else. Its serial port
**disappeared 36 seconds before it started** (`COM22 disappeared`, t=22523). A vanished port means the
harness stopped seeing the board's own logs, so the grid state it was judged against is the only evidence
there is. This is recorded as contaminated rather than as a fifteen-minute firmware fault, and the entry
immediately below it — `fnk0104b`, 50 s, every access point up with room — is the one worth chasing.

## Harness notes

- **The headline and the table disagree.** The summary says "2 unjustified" while the table shows
  unjustified seconds on 18 rows. Both are true: an episode becomes a *finding* only past
  `FALLBACK_GRACE_S` (45 s), and shorter ones are tabulated so they can be seen. The wording should say so.
- **A message is still counted "clean" when the receiver's access point is under a fault**, which is why
  almost every finding in the list reads `lost while both were registered ... faults on <AP> (its own
  chaos)`. Those 26 losses are mostly correct behaviour reported as findings. Carried over from the last
  run and still worth fixing.
- Two access points restarting 13 and 14 times is their armed chaos build doing its job (D82); the journal
  on the board says which restarts were deliberate, and the laptop cannot.

## What this run could not test

**Voice.** The harness had no way to start a talk, so nothing here says whether anybody could be heard
on this grid. That gap was closed while this run was in flight — a `talk`/`demo` console command, frame
counts at both ends, and a `voice` chaos round over 1:1, group and everyone — along with a fix for
`hh_service_voice_send` refusing every broadcast voice frame, which means **the spoken announcement of
D88 had never worked at all**. None of it is in the firmware this run was carried out on. The next run
is the first that can report on voice.
