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

With `--lora` a share of the experiments (`--lora-share`, default 0.35) are LoRa rounds; `--scenario lora` makes every experiment one. These take **no board out**: they type the AP console's own chaos hooks, each of which restores itself from the AP's clock, so a dead harness cannot leave a radio off. They need no handheld.

| Round | What it does | What it proves |
|---|---|---|
| `lora_only` | `bb off` on two APs for 90-300 s | Anything that still crosses went by LoRa |
| `lora_down` | `lora off` on one AP while Wi-Fi stays up | Nothing is lost and nothing stalls without LoRa |
| `lora_wedge` | `lora reset` | The module comes back configured, its peers return, and how long that took |
| `lora_blind` | `bb off` on two APs **and** `lora off` on one of them | One AP is genuinely isolated and recovers when both are restored |

- **Traffic during a round** is the AP console's own `ping <token>` diagnostic echo, flooded AP to AP, counted on every other AP's `[BB] Echo from node ...` line. This is also the run's only traffic when fewer than two handhelds are on USB, and the report says so rather than skipping the run.
- **Counters** come from `lora` typed on the port the run already holds open — never `tools/console.py`, which would reset the board and zero them. They are cumulative since that AP booted, so each round is a snapshot before and after, and a round in which an AP restarted is reported as unattributable instead of a negative.
- **Reported per round and in the summary**: frames that arrived over LoRa before Wi-Fi had delivered them (`N of them arrived here first`), how long a message took to cross, RSSI and SNR per peer, parts dropped, reassembly timeouts, queue high-water, retries, airtime, module restarts, and each AP's free and lowest heap.
- `--no-alerts` is honoured: an echo is a diagnostic flood, not an announcement, and no SOS or urgent broadcast is sent.

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

## Known gaps

- Heap is reported as the lowest value seen. The trend over the night has to be read from the hourly `nodes` and `mem` output in the raw logs.
- A port that comes back under a new COM name is searched for only among ports that did not exist when the run started.
