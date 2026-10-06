# Chaos-testing an offline mesh network

How LocalGrid's self-healing claim was tested by attacking it for hours at a time, and the three
bugs that came out — none of which a code review or a unit test could have found.

Evidence for every number here: [the 10-hour run of 2026-09-18](chaos/2026-09-18-night-run.md).

## What the network is

LocalGrid is a messaging network that works with no internet and no phone signal at all. The
infrastructure is a handful of ESP32-based access points you place around an area; they find each
other and form a self-healing network on their own. People carry CYD-based touchscreen handhelds and
text each other one-to-one, in groups, or broadcast to everyone.

Three radios are in play, each with a job. Wi-Fi carries normal traffic. LoRa is a long-range backup
link between access points for when Wi-Fi cannot reach (D71, D74). Bluetooth lets a laptop or phone
watch the network's health without joining it (D68, D70). Emergency alerts go out over Wi-Fi *and*
LoRa simultaneously, so an alert never depends on one radio being up.

## No single point of failure

Originally one access point was in charge: it held the network's settings and served the admin page.
When it was down, nobody could set the network's time — and without a valid network time, every
non-urgent message was blocked. A textbook single point of failure, in a system whose entire selling
point is surviving things dying.

So the master was removed (D45). Every access point now holds a complete replica of the shared
state: settings, groups, who is currently online, the network time, and the health history. Every
record carries a version and an author, so when two replicas disagree the newest wins —
last-write-wins eventual consistency. Records re-announce themselves whenever a link comes up *and*
on a timer after that (D48). The result is that the network tolerates losing any node, and survives
a split: two halves keep working independently, and when the link returns they reconcile on their
own with nobody intervening.

That is the promise. **If an access point loses power and comes back with nothing, it recovers its
entire state from the nodes that stayed up.**

The trade-off is real and deliberate. There is no consensus, no ordering guarantee between different
records, and if two people edit the same setting on two access points during a split, one edit loses
silently. Availability was chosen over consistency because the failure actually worth fearing was
"the network is up but nobody can send a message" — which is exactly what the master-node design
produced.

## Built for kilobytes, not gigabytes

One decision shaped everything downstream. These devices have around fifty kilobytes of free memory,
and the access points had been holding a permanent six-kilobyte buffer just to format their own
status as text. So the rule became: **devices store and exchange packed binary records, and the
decoding into words happens off the device** (D49).

State lives as bit fields, small integers and numeric codes, both in memory and on the wire. Turning
that into names, dates, durations and graphs happens in the browser on the admin page, or on the
laptop in the tooling. Text on a device exists only for the instant it takes to print a console line.
That is what made a per-minute availability history affordable on a device with no room to spare —
and it is why a node that crashed can still say exactly what happened while it was gone.

## The devices test themselves

A rule set early: all testing runs on the real hardware (D25). No PC pretending to be a node, no
emulated handheld — the laptop only builds, flashes, and reads logs. That forced the tests onto the
devices, which turned out to be worth more than it cost.

Every handheld runs a self-test **at every boot** (D24): twenty-five checks in under a tenth of a
second, covering the message envelope, duplicate detection, message bodies, text handling, and
known-answer vectors for all three cryptographic primitives. It logs its result and the device's own
Status screen displays it, so a handheld will tell its user it is unwell without anyone plugging in a
cable. The full suite — including a simulated three-node network running entirely on one chip — also
runs on the device, but it needs a 136 KB block that a classic ESP32 cannot spare once Wi-Fi is up,
so it is a deliberate separate build.

That gives three layers, each testing something the layer above cannot. The full on-device suite
proves the protocol logic is correct in isolation, deterministically. The boot self-test proves the
code on *this* chip, with *this* radio, is still the code that passed. And chaos testing proves the
recovery paths work when reality interferes — which is the only layer that can, because recovery code
only executes during failure.

There is a bonus that only became obvious afterwards. A single overnight run restarts devices dozens
of times, and **every one of those restarts re-runs the self-test on real hardware** — including
after power was cut mid-operation, which is exactly when stored data gets corrupted. The framework
does not yet read those results, and wiring it to insist that every boot's self-test passed is the
cheapest improvement available: it would turn a silently-passing check into an asserted one, and
catch storage damage that no recovery-time measurement would ever reveal.

## Why chaos testing

Self-healing was the core claim, and it was a promise, not a fact. It had never been tested properly,
because the only way to test it is to break things *while traffic is flowing* — and you cannot do
that by hand for long enough, or unpredictably enough, to matter.

## Preparing for it

What made it feasible was a decision made much earlier for completely different reasons: **every
device has its own command line** (D28).

All of them sit on a workbench, each plugged into a laptop by cable, and any of them can be given
commands and read back from. A handheld can be told to send a text to another handheld. An access
point can be asked who is currently online, or how much memory it has left, or to switch its LoRa
radio off for two minutes. A **handheld** can be told to switch its own Wi-Fi off for a set number of
seconds (D78) — added so the second radio can be tested in a field rather than only on a bench with
the backbone broken by hand, and useful here as a fault the framework could not previously inject:
losing a handheld's network without resetting the handheld. Any device can be asked for its supply
voltage. All of that was built just
to make development bearable — but it meant a full control and observability surface already existed
across every node, with no new hardware and no changes to the firmware.

**The control surface exists only on the physical USB cable.** That sentence was true when this was
first written, stopped being true on 2026-09-30, and is true again as of 2026-10-05 (D103). The stretch
in between is worth recording rather than tidying away.

The access points browned out on the bench's USB supply, and the plan was to move them to battery, which
takes their serial cable away. So, with the owner's decision each time and a register of what had been
conceded ([docs/TEMPORARY.md](TEMPORARY.md)), four things were built: an AP build that broke itself on a
schedule, arming it from the admin page over Wi-Fi, arming it over BLE, and finally an opcode that made
any AP perform one fault on request over BLE. Each lowered the bar from "a hand on a cable" to "the admin
password", and the last was in every image. `tools/check_release.py` refused a release while any of it
was in the tree, and the owner's rule was that the prototype would not be left with the backdoors it
opened to test its own resilience.

Then the power supplies were changed, the brownouts stopped, and the access points stayed on the bench.
On 2026-10-05 three chaos runs, eleven hours in all, were driven entirely over USB and used none of it.
That was the condition every one of the four had named for its own removal, so all four were removed -
deleted, not put behind a build option, because code that is switched off still ships.

**What is left, and why it is not a backdoor:**

- Every fault the framework injects is typed at a serial console or made on a reset line. Nothing over
  Wi-Fi or BLE can take a radio or an access point out.
- The BLE admin link is read-only again, but for asking an AP to serve its admin page for a few minutes
  (D91); the page still needs the grid's Wi-Fi passphrase to reach and the admin password to use.
- An access point replaces its firmware only with an update signed by the release key it was given
  (D97), so the admin password alone no longer amounts to "run anything".
- A device that **cannot prove its identity refuses every command** except "who are you" (D75).

**What that costs:** an access point with no cable cannot be chaos-tested. A soak on battery with
faults needs cables run to the access points, or the removed code brought back from history.

- The radio-off commands **restore themselves on a timer**, so a crashed test harness cannot leave
  the network degraded. The handheld's Wi-Fi switch has both forms, deliberately told apart: `wifi
  off <seconds>` is the self-restoring one, capped at an hour and forgotten across a restart, while
  a bare `wifi off` is sticky and survives a reboot — that one is for carrying a handheld into a
  field, and a chaos run must never use it.

The cable, **where there still is one**, gives one more thing: control of each board's reset line.
Every board on the bench is on USB, so this applies to all of them. A brief pulse is a crash and
instant restart. Holding it down means the device is simply gone for as long as it is held. One
honest caveat: holding reset is not identical to a true power cut, because the chip's always-on
memory survives it — which is precisely why the full-blackout scenario below matters.

## What the framework does

[`tools/chaos.py`](../tools/chaos.py) drives every one of those command lines at once and attacks the
network on purpose, unattended, for hours. Nothing about the boards is written into it: at startup it
opens every USB serial port, resets it, and reads each board's own identity banner to learn what it
is. (The first version hard-coded port numbers and broke the first time a board was re-plugged.)

Everything it does is randomized, and all of it from a single seeded generator:

| Randomized | Range |
|---|---|
| Which fault | 70% the device is taken out entirely, 30% a crash-and-instant-restart |
| Who gets hit | any access point or handheld; sometimes two access points at once |
| For how long | three bands — 5–30 s, 30–120 s, or 2–10 min |
| When | a gap of 3–15 minutes between faults |
| Which radio round | Wi-Fi off between two nodes (proving LoRa carried it), LoRa off (proving nothing stalls without it), or one node fully isolated — for 90–300 s |
| The traffic underneath | a random sender, a random recipient, a random type (direct, group or broadcast), on a jittered interval |

Because it is seeded, **any night can be replayed exactly**, which is the difference between a war
story and a reproducible test case.

Three properties keep it from being merely destructive:

- **It measures rather than observes.** Recovery times, delivery rates, latency percentiles, and
  restarts nobody asked for — with their cause, read from the record each node keeps of why it last
  died.
- **It gates on steady state.** No new fault begins until every live access point is linked to every
  other, every live handheld is registered, and the replicas agree on the settings version. Every
  result is therefore attributable to one cause.
- **It bounds the blast radius.** Never more than all-but-one access point down at once unless a
  blackout is asked for explicitly, never every handheld, no experiment started that cannot finish,
  and every held board released with its reset lines low on any exit — including a crash or a
  Ctrl-C. It also takes the flashing lock, so nobody can reflash a board mid-run.

## Seven bugs: five it found, one it found by reading the same log, and one it would have missed entirely

### One: a use-after-free the crash reports could never point to

The handhelds kept crashing, and the crash reports were perfectly accurate — and completely useless.
Every one of them pointed inside the memory allocator: the part of the system that hands out and
takes back memory. Once it was the networking thread that died there, once the screen-drawing thread.
Both reports were correct. Neither told us anything, because a crash report tells you who tripped
over the damage, never who caused it.

The pattern was the diagnosis. Two unrelated threads dying in the same place means nobody there is at
fault: something corrupted memory earlier and walked away clean. So the firmware was rebuilt with
memory checks on, which fill freed memory with a recognisable pattern and shout when someone reads
it. Then it failed on demand and pointed at the culprit.

It was the message banner. When a message arrives, the handheld shows a banner and asks the system for
a reminder: *wake me in fifteen seconds so I can hide this again.* Once that reminder fires, the
graphics library deletes it and hands the memory to whatever needs memory next — and our code had
kept a note of where the reminder used to be. So the next message arrives, the code follows that
note, and writes "restart the reminder" into memory that now belongs to something else. Like posting
a letter to a friend's old address: it still gets delivered, just to whoever lives there now.

Nothing crashed at that moment. The damage sat there until another part of the program read its own
data, found nonsense, and fell over — which is why every crash report landed in the allocator. And if
two messages arrived within fifteen seconds of each other it worked perfectly, because the reminder
was still alive and the note still good. It only broke when messages were spread out, which is why it
looked like random bad luck and why it took hours of random failure to surface.

**Fixed** by telling the library not to destroy a finished reminder, so it pauses and the reference
stays valid. The same mistake was in a second place — the keypad's typing pause — with a
double-delete on top. The instrumented build also exposed two UI operations running from the wrong
thread without holding the display lock, which were fixed at the same time.

### Two: a node came back from the dead and started refusing good messages

This one was worse, because nothing looked broken at all.

The replication rule says announce on link-up *and* on a timer. The presence code did the first half
and skipped the second: when a handheld joined, every access point was told once, and never told
again.

So the framework killed an access point. While it was gone, a handheld joined elsewhere, and the
announcement went out to everyone still alive. Then the dead node came back — and nobody ever told
it. It returned knowing nobody, and started refusing perfectly valid messages: telling you the person
you were texting was not around, when they were sitting right there. **Forty-four times in a single
overnight run, in clusters after a restart, with no error logged anywhere.**

The design rule already said exactly what to do. The code did half of it, and no code review or unit
test ever caught the gap.

**Fixed** by re-announcing presence every sixty seconds, so a returning node is told again within a
minute — the rule, actually implemented. The underlying hole was closed too: an authenticated message
from a known peer now confirms that link from this side as well, instead of being discarded. And the
handheld now logs the refusal, so this failure can never again be silent. A regression test covers
it.

### Three: the self-healing promise was not true

If every access point went down at the same moment, the network lost track of the time — and the code
that was supposed to recover it from another device could never have worked at all. A handheld was
meant to carry the clock back, but a handheld that had also just restarted reported an empty clock.
Dead code guarding the network's most fundamental shared value.

There is a sting in the tail: the framework had been hiding this. After a blackout it used to set the
time from the laptop's clock immediately, which papered over the failure perfectly. It was changed to
wait, and to record *which device* restored the time — and the answer was nobody.

**Fixed** (D60) by moving the clock into the chip's always-on timer, which keeps counting through any
reset that does not cut power, with a marker so a restored value is known to be genuinely ours. A
node restores it as "carried, accuracy unknown", so any node with a better source corrects it
immediately. A GPS on one access point later gave the network a real time source needing no human at
all (D63). And the framework now reports where time came back from after every blackout; a blackout
that recovers from nothing is a recorded failure.

### Four: a slow handheld could restart the whole access point

The bench came back on 2026-09-30 — new power supplies, zero brownouts on all three APs, all nine
boards answering on USB — and the very first chaos run found this in its first ten seconds, before
it had injected a single fault.

`SEND_WAIT_MS` bounds how long **one** handheld may hold up the core task: two seconds, on a
non-blocking socket, which is the right shape. It was read as bounding the task. It does not. A pass
can send to every session, so three handhelds with full sockets cost six seconds, and the task
watchdog panics at five.

Everest printed `[UP] 2 s`, then nothing at all for 5.7 seconds — two sessions closing with
`send failed` two seconds apart — and then `task_wdt: lg_core` aborted it. It restarted, the
handhelds piled back on, and it did it again. **The per-send cap was never the dangerous number; the
missing per-pass one was**, and no amount of staring at `write_frames` would show it, because
`write_frames` is correct in isolation.

The fix is a budget the whole pass shares (`sess_pass_begin`), reset next to the watchdog feed it
exists to protect. When it runs out, a frame that has not started going out is dropped and the
session is kept — the trade the voice path already makes (D61) — because a dropped message costs one
message, and a watchdog panic costs every handheld on that AP.

This is the fourth bug found the same way as the first three: **in a serial log, from a board, under
load nobody would produce by hand.** It is also a reminder of what the wireless run in D83 gives up.

### And one the run found by refusing to start: three endpoints that did not exist

While reading the same boot log: `httpd_register_uri_handler: no slots left`, three times.
`max_uri_handlers` was written down as 12 beside a route table that had grown to 15, so every AP was
serving an admin page **without D83's chaos arming or its journal** — the three newest routes, at the
end of the array, silently dropped. Nothing failed and nothing was refused. `tools/check_release.py`
went on reporting the concession as present, correctly, because the routes were all there in the
source; the feature was missing only on the hardware. The limit is now counted from the table and a
route that fails to register is an error-level log.

### Five: the load balancing made a broken access point the most attractive one

This is the one worth reading twice, because every part of it was working as designed.

D54 balances handhelds across APs: among every AP heard at a good signal, add 1000 and subtract 100
for each handheld already attached, so the emptiest wins by a margin that deliberately swamps RSSI.
Sound rule. Then Everest ran out of heap — its Wi-Fi began refusing every association with reason 5
(`ASSOC_TOOMANY`) while its beacon went on advertising **"0 attached, 15 free"**, which was true of
its own tables and a lie about what it could do.

So Everest was the emptiest AP on the grid, and the balancing rule did exactly what it was told:

| AP | signal | attached | score |
|---|---|---|---|
| Everest | **-52 dBm** (weakest) | 0 (because it refuses everyone) | **948** |
| Fuji | -40 dBm (strongest) | 1 | 860 |
| Denali | -44 dBm | 1 | 856 |

Four of six handhelds chose Everest over two healthier APs with **stronger** signals, were refused,
waited ten seconds, and chose Everest again. For the whole run. `hosyond` did it fourteen times in
the log. **The emptier it got, the more attractive it became** — and it got emptier by failing.

Nothing logged an error. Every component was correct in isolation: the beacon reported its own
tables honestly, the scoring implemented D54 faithfully, and the retry backoff did what backoff does.
The bug lived in the space between them, and it turned one AP's memory problem into four handhelds
offline — on a network whose first claim is that it has no single point of failure.

The fix is small and belongs on the handheld: an AP that refuses or drops this handheld is passed
over for a while, doubling from 15 s to a 2-minute cap, cleared the moment a session with it actually
works. The penalty is sized to lose to any healthy AP while staying far above the disqualifiers, so
an avoided AP that is the **only** one left is still chosen — being briefly unwanted must never
become being unreachable.

What this says about D54: load balancing must never be the whole of a choice, because "empty" and
"broken" look identical from the outside. **Liveness has to beat tidiness.**

And there was a second half to it, in the beacon rather than the scoring. The free-slot byte was
`LG_PROTO_MAX_STATIONS - clients` — room in the table, nothing about whether the AP could serve it.
That constant is 15, the SoftAP limit, where the memory-limited figure on these boards is two or
three, so **every AP advertised about five times its real capacity** and the number both the
balancing and the handhelds' choice rest on was never true. An AP now derives it from free heap: a
6 KB floor for itself, about 3 KB per further handheld, both measured here, never more than the table
allows — with the one floor that an AP holding no handhelds always offers at least one slot, because
if every AP were honest enough to advertise zero at the same moment, `node_usable` would reject all
of them and strand the grid. Both halves are recorded as D84.

Measured afterwards, on the same board that had been crashing: **minimum free heap 5796 bytes where
it had been 400**, flat at 10 KB for the whole window, and no crash of any kind on any AP through a
full run. The crashes were as much a cause of the memory collapse as a symptom of it — each restart
brought every handheld back at once, and that storm was what emptied the heap.

### Six: the second radio could receive a message and could not send one — and chaos would never have said so

The handhelds' LoRa modules worked, in one direction. On 30 September 2026, with one handheld's Wi-Fi
switched off, a message sent *to* it arrived over LoRa, was decoded, was shown on its screen and sounded
its notification. Its own reply never reached the air. **One frame went out in 150 seconds, and that one
was the announcement.** The reply waited in the outbox and left over Wi-Fi seventy seconds later, the
moment a session came back — which in a message list looks exactly like success.

The cause was three gates in `lg_core`, each of them correct for a socket: refuse to build a frame unless
`connected`, attempt the first send only `if (registered)`, and retry nothing unless registered. A
handheld on LoRa has no session and never will, because it is not a backbone peer (D77), so on that radio
"not connected" is the normal state and precisely the moment speaking matters. The announcement had
already been carved out of the first gate when it hit the same wall; that carve-out should have been read
as evidence that the gates were in the wrong place, and was not.

**The part that matters for this document is how it was found: by hand, not by chaos.** Every LoRa round
the framework had took ESP-NOW away *between access points*, which exercises the access points' radios
and never touches a handheld's. A chaos night would have reported a clean grid for as long as the bug
existed, and the report would have been true about everything it measured.

The round that closes it is `lora_handheld`: switch one handheld's Wi-Fi off for a window, then send
messages **in both directions** and require delivery. Both directions matters — receiving was never
broken. It is typed at a handheld, not an access point, which also makes it the only LoRa round that can
run while the access points are on battery with no cable; a run with no access point on USB now says so
and carries on rather than refusing to start. The findings it raises are deliberately *not* excused by
the victim "having lost its access point": it lost it because the round took it away, and carrying its
traffic anyway is the entire claim under test.

After the fix, on the boards: a message sent with Wi-Fi off reached the other handheld, and the
acknowledgement came back over LoRa — **a 2.7-second round trip at SF9** — with Wi-Fi not returning for
another forty-eight seconds, and no duplicate when it did.

### Seven: a handheld would not use the only access point on the grid, and the backup hid it

The best finding so far, and the one the framework **did not** find. It came out of chasing a vague line
in a console report, and it changed how the harness judges success.

A handheld sat on LoRa with a usable access point at -47 dBm in earshot. The log said something that
should have been arithmetically impossible:

```
[NET] Heard AP 1 "Fuji" -47 dBm, 5 attached, 1 free, backbone no, grid time yes, 94 ms ago
[NET] Scan done: 10 records, 1 usable node(s)
[NET] Scan found 1 usable node(s); scanning again in 10 s     <- every ten seconds, for minutes
```

One usable access point, and the chooser picked nothing. `best_score` started at `-1000` as a "nothing
chosen yet" sentinel, and that had quietly become a **minimum quality** every avoided access point fell
through: the avoidance penalty is 2000, so Fuji scored `-47 + (1000 - 100 x 5 attached) - 2000 = -1547`,
below the sentinel, and no access point was ever returned. D84 promises the opposite in as many words —
*"an avoided access point that is the only one left is still chosen"* — so the decision was right and the
code did not implement it. A handheld refused once would not retry **the only access point on the grid**
for up to two minutes.

**Why no chaos run caught it.** Nothing logged an error; every part did as it was told. And the handheld
had a working second radio, so its messages kept arriving — the run measured delivery, delivery was fine,
and the fault was invisible. The backup did not merely fail to reveal the bug: it **concealed** it. Worse,
the veto only bites when the avoided access point is the last one standing, so a healthy three-access-point
bench hides it too.

That is a gap in how a run judges success, not only a gap in the code, and it matters because **not every
handheld will carry a second radio**. So the harness now asks a different question. Every stretch a
handheld spends with no Wi-Fi session is sampled and judged: was its radio switched off on purpose, is it
under an injected fault, is an access point in trouble, is any access point up, is every one full? If none
of those explains it, the fallback is **unjustified** and becomes a finding after 45 seconds — whether or
not a single message was lost. Every message now records the path it took (`wifi` or `backup`) and whether
Wi-Fi was available at the time, so a run can be analysed afterwards instead of re-derived. And a new
`no_net` round **switches the second radio off on every handheld that has one**, which makes them behave
like the ones that will ship without it: a Wi-Fi fault then has nothing to hide behind.

The lesson generalises past this bug. **A fallback that works is indistinguishable from a system that does
not need one, unless the test asks.** Any redundancy worth having deserves a round that takes it away.

## What the numbers say

The runs are kept as evidence: [2026-09-18, ten hours on the bench](chaos/2026-09-18-night-run.md) and [2026-10-01, ten hours to flat batteries](chaos/2026-10-01-night-run.md) - the second one on battery, with the backup audit and the `no_net` and `alerts` rounds, and the night that found the outbox leak that refused an SOS.

From [the 10-hour run of 2026-09-18](chaos/2026-09-18-night-run.md) — three access points, three
handhelds, seed 1172352511:

| Measure | Result |
|---|---|
| Experiments | 55 injected, 55 recovered, 0 needing a human |
| Backbone whole again after losing an access point | median 3.7 s, p95 15.5 s |
| Handheld moved to another access point | median 7.3 s |
| Messages with both ends registered throughout | 782 sent, 738 delivered; latency median 0.2 s, p95 0.4 s |
| Restarts the framework did not cause | none — no panic, no watchdog, no brownout in ten hours |
| Findings | 46: the 44 refusals described above, and 2 harness findings |

The 44 failures are the honest headline. They were not message loss in transit; they were the
presence bug, and they are why that run was worth ten hours.

## What it still cannot find

Worth stating plainly, because the gaps are as informative as the findings:

- **Every fault is binary.** Reset, power out, radio off. No packet loss, no attenuation, no jitter,
  no corrupted frames. Real radio failure is gradual, and the framework cannot enter that regime.
- **No asymmetric or arbitrary partitions.** It cannot cut the link between two specific access
  points while leaving a third connected, and it cannot make a one-way link — the classic
  distributed-systems killer.
- **Coverage is bounded by what the firmware logs.** Message loss and message refusal were
  indistinguishable until the firmware was taught to log the refusal.
- **Only boards on USB can be victims**, so nothing is battery-powered and everything is within
  radio range on one desk. No geometry, no distance, no roaming.
- **Faults never overlap.** The steady-state gate deliberately excludes cascading failure, which is
  the mode that takes real systems down.
- **The access points' own faults do not reach the handhelds.** A chaos build attacks itself (D82) and
  everything it does is to an access point. Nothing it does makes a *handheld* use its module, because a
  handheld only reaches for LoRa when its own Wi-Fi session has gone. That gap is now covered by a round
  typed at the handheld instead (`lora_handheld`, below), but the asymmetry is worth keeping in mind: a
  self-attacking access point can never test the device in someone's hand.
- **Leaks are not detected.** Heap is reported as the lowest value seen, not as a trend.
- **A chaos run cannot yet prove that LoRa carried anything between access points.** The journal
  records which fault happened and the BLE beacon carries heap, restarts and brownouts — neither
  carries a LoRa counter. So after a backbone-off fault on a battery-powered access point the run can
  say "the grid did not fall over" and cannot say "LoRa carried it". That is the difference between
  resilience as a fact and resilience as a statement, and it is the next thing to close on the access
  point side: the counters already exist in the firmware (`frames out N in N`), they are simply not in
  anything a laptop can read without a cable.
- **No asserted thresholds.** Recovery times are read after the run rather than declared before it
  and failing the run, and results are not yet kept as a baseline between runs.

## What is next: chaos in the field

The current framework reaches every device through a USB cable, which means every device is tethered
to one laptop on one desk, all inside each other's radio range. That tests recovery logic but not the
network. Three things have to be solved, roughly in this order:

1. **A control channel with no wires.** An authenticated Bluetooth link to each access point already
   exists (D70), but it is deliberately read-only — a watcher "stays a monitor and nothing more".
   Adding fault injection to production firmware would break that, so the likely answer is a separate
   chaos firmware build behind a build flag, never flashed to a real device, so the attack surface
   physically does not exist in the shipped image.
2. **A schedule that survives losing contact.** In a field the laptop cannot be assumed to reach
   every node when a fault is due. So invert it: give every node the same seed and the same schedule,
   let each inject its own faults from its own clock, and collect results afterwards. The
   self-restoring radio hooks already work this way. It also removes the harness as a single point of
   failure, which is a fitting irony.
3. **Power faults without a reset line,** which means a switch or relay per access point — and a real
   battery disconnect would finally test something the bench never has, since holding reset always
   left the chip's always-on memory intact.

Then the test itself: devices spread across a field, a walk that takes handhelds out of range, and the
first real measurement of the ranges the site currently only estimates.

## What this says about testing

All five bugs share a shape. The symptom appeared far from the cause. The failure was silent. And
the guarantee that was written down was not the guarantee the code implemented.

None were findable by code review or unit tests, because none of them are wrong in isolation — they
are only wrong in the presence of failure at an awkward moment. Which is the whole argument for chaos
testing: a distributed system's recovery paths are code too, and code that only runs during failure
is code that has never been tested.

The layering matters as much as the chaos. Deterministic tests on the device proved the logic, the
boot self-test proved the deployed binary, and chaos proved the recovery. Skip any layer and you
cannot tell which kind of broken you are looking at.

And the cheapest lesson: the framework itself was quick to write, and only because the control and
observability surface already existed — built months earlier for convenience, with no idea what it
would eventually be used for.

---

**Running it yourself:** [`.claude/skills/chaos/SKILL.md`](../.claude/skills/chaos/SKILL.md) has the
commands, the safety rules, and how to read a report. `python tools/chaos.py --self-check` checks the
log parsers and the scheduling rules without opening a port, and `python tools/chaos.py --hours 10
--seed 7` prints a night's schedule without touching a board.
