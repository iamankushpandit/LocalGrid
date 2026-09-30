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
radio off for two minutes; any device can be asked for its supply voltage. All of that was built just
to make development bearable — but it meant a full control and observability surface already existed
across every node, with no new hardware and no changes to the firmware.

**This is not a backdoor, and the boundaries are deliberate:**

- The console exists **only on the physical USB cable**. It is not reachable over Wi-Fi, LoRa,
  Bluetooth, or the network, so using it means physically holding the board.
- It **cannot change anything grid-wide**. The network name, the admin password and permissions
  change only through the admin page, behind a salted PBKDF2 password hash with login lockout.
- A device that **cannot prove its identity refuses every command** except "who are you" (D75).
- The radio-off commands **restore themselves on a timer**, so a crashed test harness cannot leave
  the network degraded.

The cable gives one more thing: control of each board's reset line. A brief pulse is a crash and
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

## Three bugs it found

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

## What the numbers say

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
- **Leaks are not detected.** Heap is reported as the lowest value seen, not as a trend.
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

All three bugs share a shape. The symptom appeared far from the cause. The failure was silent. And
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
