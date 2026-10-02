---
name: chaos
description: Run a chaos test on the bench grid, taking APs and handhelds out at random for hours and measuring how the network recovers. Use when asked for chaos engineering, an overnight or soak test, fault injection, or how the grid reacts to APs dying.
---

# Chaos run on the bench grid

`tools/chaos.py` takes APs and handhelds out at random, has the handhelds message each other the whole time, and writes a report of what recovered, how fast, and what did not.

## Before a live run

**Owner approval.** Run `--live` only when the owner asks for a run. The first live run, one hour, was asked for on 2026-09-16.

**D25.** D25 says the PC only builds, flashes, and reads serial logs. This script also resets boards and types console commands (`send`, `time set`, `nodes`, `mem`, `volume`), as `serial_capture.py --reset-at` and `--send` already do. Messages are still sent and received only by the handhelds. The owner asked for live runs but has not recorded a D25 answer. Ask, and record it in `docs/DECISIONS.md`.

**Relays.** Handheld relaying (D47) is not built, so today the victims are APs and handhelds. Add a relay fault type once D47 lands.

## Boards are discovered, not listed

Nothing about the boards is written into the script. At the start it opens every serial port that has a USB ID, resets it, and reads the board's own `LGID:` line: device ID, role, board code, and AP index and name or handheld device index. A board whose banner was missed is asked with `id`. Ports that give no LocalGrid ID are released and left alone. `tools/bench_devices.json` is used only to give a known ID its bench name. Its ports are ignored, because they shift when boards are re-plugged.

- **Slots.** APs fill slots `AP1`, `AP2`, ... in AP index order, and handhelds fill `HH1`, `HH2`, ... in device index order. The schedule names slots, so a seed replays the same faults on the same grid size.
- **Holds.** Whether a board can be held comes from its port's USB vendor ID, not its board code. A separate bridge chip (CP210x `10C4`, CH34x `1A86`, FTDI `0403`) stays on the bus while the chip is in reset. A port that is the chip's own USB (Espressif `303A`) would disappear, so a hold planned there becomes a pulse, and the experiment says so.
- **Group messages.** The group comes from each handheld's `groups` command: the first group every handheld belongs to. With none, group messages are left out.
- **Swaps.** A port that later answers with a different ID is recorded as a finding and gets no more faults.

## Faults

| Fault | How | Boards |
|---|---|---|
| pulse | EN low through RTS for 0.2 s: a crash and restart | All |
| hold | EN held low through RTS for 5 s to 10 min, then released: a power loss | Ports behind a separate USB bridge chip |

## LoRa rounds (`--lora`, D71)

With `--lora` a share of the experiments (`--lora-share`, default 0.35) are LoRa rounds; `--scenario lora` makes every experiment one. These take **no board out**: they type a board's own chaos hooks, each of which restores itself from that board's clock, so a dead harness cannot leave a radio off.

| Round | What it does | What it proves | Typed at |
|---|---|---|---|
| `lora_only` | `bb off` on two APs for 90-300 s | Anything that still crosses went by LoRa | an AP |
| `lora_down` | `lora off` on one AP while Wi-Fi stays up | Nothing is lost and nothing stalls without LoRa | an AP |
| `lora_wedge` | `lora reset` | The module comes back configured, its peers return, and how long that took | an AP |
| `lora_blind` | `bb off` on two APs **and** `lora off` on one of them | One AP is genuinely isolated and recovers when both are restored | an AP |
| `lora_handheld` | `wifi off` on one handheld that has a module, 90-300 s | Its own messages still cross **and are acknowledged**, instead of waiting in its outbox for Wi-Fi | a handheld |

**Why `lora_handheld` exists, and why it is the one to watch.** The four AP rounds take ESP-NOW away between APs, which exercises the APs' radios. None of them makes a *handheld* use its module: a handheld reaches for LoRa only when its own Wi-Fi session is gone (D74, D78). That gap hid a real fault for as long as it existed — on 2026-09-30 a LoRa-only handheld could receive a message and an SOS and could originate nothing but its announcement, and a chaos night would have reported a clean grid. The round sends **both ways** on purpose: receiving was never broken.

Three things follow from it being typed at a handheld:

- **It is the only LoRa round that works with the APs on battery.** AP hooks are AP console commands and need a cable; the handhelds still have theirs. A run that finds no AP on USB now says what cannot run and carries on, instead of refusing to start.
- **Which handhelds have a module is read off the boards** with `lora`, never assumed from a board code, and the round is skipped for a handheld without one.
- **Its findings are not excused by the victim "losing its AP".** It lost its AP because the round took it away. Carrying its traffic anyway is the claim under test, so a message it failed to deliver is a finding even though the ordinary rule would call the probe unclean.

- **Traffic during an AP round** is the AP console's own `ping <token>` diagnostic echo, flooded AP to AP, counted on every other AP's `[BB] Echo from node ...` line. This is also the run's only traffic when fewer than two handhelds are on USB, and the report says so rather than skipping the run.
- **Traffic during a handheld round** is ordinary 1:1 messages, aimed rather than drawn at random: to the handheld whose Wi-Fi is off and from it, one pair every 12 s, with 45 s at the end for the last of them to land. A frame is seconds of airtime at SF9, so a round that did not wait would report its own impatience as message loss.
- **Counters** come from `lora` typed on the port the run already holds open — never `tools/console.py`, which would reset the board and zero them. They are cumulative since that AP booted, so each round is a snapshot before and after, and a round in which an AP restarted is reported as unattributable instead of a negative.
- **Reported per round and in the summary**: frames that arrived over LoRa before Wi-Fi had delivered them (`N of them arrived here first`), how long a message took to cross, RSSI and SNR per peer, parts dropped, reassembly timeouts, queue high-water, retries, airtime, module restarts, and each AP's free and lowest heap.
- `--no-alerts` is honoured: an echo is a diagnostic flood, not an announcement, and no SOS or urgent broadcast is sent.

## Backup and alert rounds (`--extra-share`, default 0.22)

Two rounds that take **no board out** and break no radio link. They change what the grid is allowed to
lean on, and they exist because of one bug (see finding seven in `docs/chaos-engineering.md`): a handheld
could not hold a Wi-Fi session, its LoRa module carried every message, and the run reported success.

- **`no_net`** switches LoRa **off on every handheld that has a module**, for a window, so those handhelds
  behave like the ones that will ship without one. A Wi-Fi fault then has nothing to hide behind, and any
  loss during the window is reported as what a module-less handheld would have suffered. `lora off
  <seconds>` restores itself from the handheld's own clock, so a dead harness cannot leave a handheld
  without its second radio.
- **`alerts`** walks the emergency path in order: **announcement, SOS, all clear** (`send safe`, D66/D79),
  from one handheld, because the three are a sequence and D86's re-stamping applies across it. With grid
  time unset it sends only the urgent pair, since an announcement is refused by D6 and counting correct
  behaviour as a failure would manufacture findings. Skipped entirely under `--no-alerts`.

## Was the backup needed? (the audit that runs always)

Independent of any round, and the most important thing the harness now does. Every handheld with **no
Wi-Fi session** is sampled every 10 s and the fallback is judged, not just counted:

| Verdict | When |
|---|---|
| justified | its Wi-Fi is switched off by this run (D78), it is under an injected fault, an AP is in trouble on its own, no AP is up, or every AP it can hear is full |
| **unjustified** | none of the above: an AP was up with room and no fault was injected |

An unjustified fallback becomes a **finding after 45 s** — long enough that an honest rejoin or roam is
never blamed — **whether or not a single message was lost**, because a handheld without a second radio
would simply be offline for that long. Every episode is filed either way (`fallback_began`,
`fallback_ended` in `events.jsonl`) and tabulated under **"Did anything lean on the backup, and did it
need to?"**, so the justified ones can be told apart instead of burying the real ones.

Every message also records the path it took: `sender_path` and `receiver_path` (`wifi` or `backup`), plus
`wifi_possible` and `backup_reason` when an end was on the backup, written at send time so a run can be
analysed afterwards without re-deriving anything.

## Safety rules the script enforces

- At most all APs but one are out at once, so grid time (RAM-only, D6) survives. `--blackout` allows every AP out, then sets time again from the PC clock.
- Never every handheld at once.
- A new fault starts only once the grid is steady again (see below). An experiment that could not finish before the planned end is not started.
- On exit, Ctrl+C, a crash, or the console window closing, every held board is released and every port closed with DTR and RTS low. A plain close holds these boards in reset (see the serial-close memory).
- It takes `tools/.flash.lock`, so no `flash.py` run can start while the grid is under test, and it refuses to start if one is running.
- Nothing is injected until discovery is over, and two boards with the same name stop the run before any fault.

## Steady state

The grid counts as steady when all of these are true:

- Every AP that is up has a backbone link to every other AP that is up.
- Every AP has printed its 30 s `[GRID] links ...` line since it last started, and its grid time is not UNSET once time was set.
- Every handheld that is up is registered with an AP that is up.
- The APs agree on the settings version.

## Steps

1. Check the logic offline. Neither command opens a port.

   | Goal | Command |
   |---|---|
   | Parsers, bridge rules, schedule rules | `python tools/chaos.py --self-check` |
   | See a night's schedule | `python tools/chaos.py --hours 10 --seed 7` (assumes `--aps 3 --handhelds 2`) |

   Done when self-check reports 0 failures.

2. Run it live, after owner approval. Starting a run pulses every board once and sets grid time from the PC clock (`--no-set-time` to skip, which limits the handhelds to urgent broadcasts). `--quiet-handhelds` turns notification sound off for the night and puts the old volume back at the end. If the script is killed hard, the volume stays off: put it back with `python tools/console.py <board> volume <level>`.

   | Goal | Command |
   |---|---|
   | Smoke run, 12 minutes | `python tools/chaos.py --hours 0.2 --live` |
   | Overnight | `python tools/chaos.py --hours 10 --live --quiet-handhelds` |
   | Night run with LoRa rounds, APs only | `python tools/chaos.py --hours 4 --live --lora --no-alerts` |
| APs on battery: the handheld LoRa leg only | `python tools/chaos.py --hours 4 --live --lora --lora-share 1.0 --no-alerts` |
| Whole system, nobody home, run to flat batteries | `python tools/chaos.py --live --lora --until-aps-die --hours 24` (alerts, SOS and the all clear included, `no_net` rounds on) |
   | Replay a night's schedule | add `--seed <seed from the summary>` |
   | Some boards only | `--devices <bench names or device IDs>` |
   | More faults in a short run | `--min-gap 2 --max-gap 5` |

   Run it in a terminal that stays open. The script keeps Windows from sleeping while it runs.

3. Read `chaos-runs/<start>/summary.md` (gitignored). Every finding points at a time; find it in `raw-NN.log` for that hour, or in `events.jsonl`.

## Reading the report

| Line | Meaning |
|---|---|
| Backbone whole again | Release to every live AP linked to every other. P7 measured 1.3 to 3.4 s. |
| Steady state after release | Can trail relinking by up to 30 s, because it waits for each restarted AP's status line. |
| Handheld failover | From the AP fault to the handheld registered again on any AP. |
| Messages with both ends registered throughout | Must be 100%. A fault somewhere else in the grid does not excuse a loss. |
| Messages where an end lost its AP | Information only. D13 has no store-and-forward, so these may be lost. |
| Restarted without an injected fault | A crash, watchdog, or brownout. Brownouts have come from the USB hub before (P7), so check the reason. |
| Messages that crossed on the backup | How many had an end with no Wi-Fi session, and how many of those **should not have needed it**. The second number must be 0. |
| Unjustified fallback episodes | Must be 0. Each one is a handheld that could have been on Wi-Fi and was not, and an outage on any handheld without a module. |
| no_net rounds | Losses here are Wi-Fi resilience failures with nothing to hide behind. |

## Known gaps

- Heap is reported as the lowest value seen. The trend over the night has to be read from the hourly `nodes` and `mem` output in the raw logs.
- A port that comes back under a new COM name is searched for only among ports that did not exist when the run started.
- **With the APs on battery, a run proves LoRa carried the handheld leg and cannot yet prove it carried between APs.** The AP rounds need a cable; arming the APs' own chaos build over BLE (`tools/chaos_ble.py`, D85) injects a backbone-off fault but reports no LoRa counters, so the grid surviving is visible and "LoRa carried it" is not. Closing that needs the LoRa counters in the chaos journal or the BLE beacon.
