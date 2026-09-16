---
name: wifi
description: Check which LocalGrid APs are actually beaconing, using this laptop's Wi-Fi radio as an independent witness. Use when an AP is missing from a phone or laptop, after flashing APs, when a handheld reports "No LocalGrid node in range", when grid time cannot be set because the master is unreachable, or to tell an AP that is dead from one that comes and goes.
---

# Check the APs are on the air

Three kinds of radio can answer "is that AP up?", and they disagree in useful ways:

| Witness | Sees | Blind to |
|---|---|---|
| A handheld (`status`, `nodes`) | Espressif beacons, and whether a session and registration succeed | Nothing about how an ordinary phone sees the grid |
| The owner's phone or laptop Wi-Fi list | What any 802.11 client sees | Whether the AP would accept a handheld |
| `tools/wifi_scan.py` | The same as a phone, but logged, timestamped, and repeatable | The same as a phone |

Before this existed, every answer came from the first two, and neither is an instrument: the handhelds attach happily to APs a phone cannot see, and a phone cannot be asked what it saw ninety seconds ago. Use the scan when those two disagree, or when you need a record.

**Decision D25 holds.** The laptop reads what Windows heard, the way the PC reads a serial log. It never joins the grid, never carries a message, and never stands in for a handheld or an AP. Being exact about the radio: Windows may answer the query with an *active* scan, sending probe requests from the laptop's own radio, so this is not guaranteed to be passive — but it never associates and never speaks the LocalGrid protocol, so it is an observer and not a participant. **Decision D21 holds too**: BSSIDs are hardware addresses, so they are never printed — signals are aggregated per SSID and any address in `netsh` output is substituted out.

## The grid's APs

Addressing is deterministic: AP *n* owns `192.168.(4 + n).0/24` and sits at `.1`.

| AP | Index | Address | Serves the admin page |
|---|---|---|---|
| `LG-MAIN` | 0 | `192.168.4.1` | **yes, and only this one** |
| `LG-NORTH` | 1 | `192.168.5.1` | no |
| `LG-SOUTH` | 2 | `192.168.6.1` | no |

`LG-MAIN` missing is worse than any other AP missing: the master alone serves the setup page, so **grid time cannot be set while MAIN is off the air**, and without grid time D6 restricts every handheld to urgent broadcasts. Anything that needs to send a 1:1 or group message is blocked until MAIN returns.

## Steps

1. **Run it elevated.** `netsh` needs administrator rights to query WLAN, so an ordinary terminal fails. The message it prints — "Network shell commands need location permission" — is misleading: the real error underneath is `WlanQueryInterface returns error 5: The requested operation requires elevation`. Check location consent only if elevation does not fix it. Done when the command prints a network list instead of an error.

   | Goal | Command |
   |---|---|
   | Are the three APs up? | `python tools/wifi_scan.py` |
   | Everything in range, not just LocalGrid's | `python tools/wifi_scan.py --all` |
   | Does an AP come and go? | `python tools/wifi_scan.py --watch 120` |
   | Insist on one AP only | `python tools/wifi_scan.py --expect LG-MAIN` |
   | Gate a flash on the APs coming back | `python tools/flash.py --role N; python tools/wifi_scan.py` |

2. **Read the verdict.** Each expected SSID prints `UP` with strength, channel and authentication, or `MISSING`. Exit code is 0 when all were heard, 1 when any was missing, 2 when the scan could not run. Done when you can name which APs are on the air.

3. **Separate a dead AP from a flapping one.** A single scan cannot tell "never there" from "there a minute ago", and those have different causes. `--watch` logs every `APPEARED` and `VANISHED` with a timestamp. Done when you know which of the two you have.

4. **Cross-check against a handheld.** `python tools/console.py hosyond status` reports `link`, which AP it registered with, and signal. Done when you have both views and know whether they agree.

## Reading the result

| Scan says | Handhelds say | Means |
|---|---|---|
| AP up | registered with it | Healthy. |
| AP up | `No LocalGrid node in range` | The AP beacons but refuses or drops sessions. Look at the AP's serial log, not its radio. |
| AP missing | registered with it | The AP is up and Espressif-visible but not reachable by ordinary clients. Check for a hidden SSID or a non-802.11 PHY mode before believing it. |
| AP missing | registered with a *different* AP | That board is down or its AP failed to start. Compare its USB port: if the port is still there, it booted and `esp_wifi_start()` or the AP config is the suspect; if the port vanished too, it is power. |
| AP missing | also missing | The board is off, crashed, or brownouts. APs behind cascaded USB hubs have done this. |
| All missing, phone sees them | — | The scan is broken, not the grid. Check elevation first. |

## Invariants

- The laptop is a witness, never a participant (D25). If a task needs a client that sends, it runs on an ESP32.
- No hardware address reaches the output (D21), including in `--all` mode where dozens of neighbouring networks appear.
- A percentage is not a measurement: `netsh` reports signal as 0–100%, and the dBm figure shown is an approximation of Windows' own mapping. Use it to compare, not to quote.
- The parser reads English `netsh` labels. On a localised Windows it says so rather than reporting an empty grid, which would look like every AP being down.
