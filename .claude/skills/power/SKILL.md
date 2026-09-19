---
name: power
description: Measure the supply voltage on LocalGrid bench boards, now or monitored over time with min, average, and max, for one board or all at once. Use when boards brown out, restart for no reason, drop off USB, or before trusting a long test on a shared USB hub.
---

# Measure board power

Every firmware answers `power` on its serial console (`components/lg_power`, D28):

| Command | Reply |
|---|---|
| `power` | `POWER: supply <mV> mV (sense pin <mV> x <ratio>, GPIO<n>)` |
| `power -m <s> [-i <ms>]` | one `POWER_SAMPLE <ms> ms <mV> mV` per reading, then `POWER_RESULT samples= min= avg= max= mV over <s> s` |
| `power -m <s> -q` | the `POWER_RESULT` line only |

Every reply ends with `POWER: this boot started after: <cause>`; on APs it also prints the restart counts by cause.

## What each board can measure

| Board | Sense | Notes |
|---|---|---|
| Hosyond (HY3) | GPIO34, 2:1 | With no battery fitted this is the charger output, which follows USB. |
| FNK0104B (F4B) | GPIO9, 2:1 | Braino read 4.09–4.16 V on USB with no pack. |
| E32R28T-1 (E28) | GPIO34, 2:1 | Vendor manual's divider. Cannot tell a missing pack from a present one. |
| E32R40T (E40) | GPIO34, 2:1 | Inherited from the E32R28T-1, not measured: check against a meter before trusting it. |
| CYD (CYD) | none | Replies `not measurable`. GPIO34 on a CYD is the light sensor, not a battery. |
| Elegoo AP (ELG) | none | Replies `not measurable`; read the brownout count instead. Wire a divider to an ADC1 pin and set `CONFIG_LG_NODE_SUPPLY_SENSE_GPIO` to measure. |

Pins and ratios are the vendor's (from Braino's measured board profiles), not a meter's. Compare one reading against a multimeter before quoting tenths of a volt.

## Steps

1. **Pick the query.**

   | Goal | Command |
   |---|---|
   | Every board, one reading | `python tools/power.py all` |
   | Every board over the same minute | `python tools/power.py all -m 60` |
   | Handhelds, four readings a second, listed | `python tools/power.py H -m 30 -i 250 --list` |
   | One board after it has settled | `python tools/power.py hosyond --settle 20` |

   Boards are queried in parallel so a hub sag shows on all of them at once. Done when the table prints a row per board.

2. **Remember the reset.** Opening a port resets most boards, so a reading lands during boot unless `--settle` waits. An AP loses grid time until a neighbour announces it. Done when you know whether the reading is from boot or steady state.

3. **Read the table.**

   | Seen | Means |
   |---|---|
   | Average near 4.1 V, swing under ~100 mV | Healthy USB supply through the charger. |
   | Swing of several hundred mV, or minimum under ~3.9 V | Supply sags under radio load: the hub or cable is the suspect. |
   | AP `restarts:` shows `low supply voltage (brownout)` climbing | That AP's supply dips below the brownout threshold; move it to a powered hub or its own charger. |
   | `FAILED no reply to power` | Firmware from before the command; flash it. |

## Invariants

- Hardware addresses never appear in the output (D21).
- `power -m` runs on the console task and blocks only the console for its duration; the grid keeps running.
- Monitoring is capped at 3600 s and 50 ms between readings.
