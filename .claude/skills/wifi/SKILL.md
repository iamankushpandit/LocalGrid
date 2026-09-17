---
name: wifi
description: Check which LocalGrid APs are actually beaconing, using this laptop's Wi-Fi radio as an independent witness. Use when an AP is missing from a phone or laptop, after flashing APs, when a handheld reports "No LocalGrid AP in range", or to tell an AP that is dead from one that comes and goes.
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

Since D46 every AP broadcasts one SSID, **`LocalMesh Access Point`**, and every AP answers at `192.168.4.1`, like a home mesh router. Since D45 there is no master: any AP serves the admin page and holds the settings, so one AP missing reduces coverage but blocks nothing by itself.

A Wi-Fi scan therefore counts APs rather than naming them: each AP is one radio answering on that SSID. The AP's name travels in a vendor element that Windows does not show, so **which** AP is missing comes from an AP's `nodes` output or a handheld's Status screen. An `LG-MAIN`-style SSID in the list means that board still runs firmware from before D46.

| AP | Index | Phone DHCP block |
|---|---|---|
| MAIN | 0 | `192.168.4.2`–`.13` |
| NORTH | 1 | `192.168.4.14`–`.25` |
| SOUTH | 2 | `192.168.4.26`–`.37` |

## Steps

1. **Run it elevated.** `netsh` needs administrator rights to query WLAN, so an ordinary terminal fails. The message it prints — "Network shell commands need location permission" — is misleading: the real error underneath is `WlanQueryInterface returns error 5: The requested operation requires elevation`. Check location consent only if elevation does not fix it. Done when the command prints a network list instead of an error.

   | Goal | Command |
   |---|---|
   | Is the grid SSID up on all three APs? | `python tools/wifi_scan.py` |
   | Everything in range, not just LocalGrid's | `python tools/wifi_scan.py --all` |
   | Does an AP come and go? | `python tools/wifi_scan.py --watch 120` |
   | Expect a different number of APs | `python tools/wifi_scan.py --aps 2` |
   | Gate a flash on the APs coming back | `python tools/flash.py --role N; python tools/wifi_scan.py` |

2. **Read the verdict.** The grid SSID prints `UP` with its best signal, channel, authentication and how many AP radios answered, or `MISSING`. Exit code is 0 when at least `--aps` radios were heard, 1 when fewer, 2 when the scan could not run. Done when you know how many APs are on the air, and, from an AP's `nodes`, which.

3. **Separate a dead AP from a flapping one.** A single scan cannot tell "never there" from "there a minute ago", and those have different causes. `--watch` logs every change in the number of AP radios with a timestamp. Done when you know which of the two you have.

4. **Cross-check against a handheld.** `python tools/console.py hosyond status` reports `link`, which AP it registered with, and signal. Done when you have both views and know whether they agree.

## Reading the result

| Scan says | Handhelds say | Means |
|---|---|---|
| AP up | registered with it | Healthy. |
| AP up | `No LocalGrid AP in range` | The AP beacons but refuses or drops sessions. Look at the AP's serial log, not its radio. |
| AP missing | registered with it | The AP is up and Espressif-visible but not reachable by ordinary clients. Check for a hidden SSID or a non-802.11 PHY mode before believing it. |
| AP missing | registered with a *different* AP | That board is down or its AP failed to start. Compare its USB port: if the port is still there, it booted and `esp_wifi_start()` or the AP config is the suspect; if the port vanished too, it is power. |
| AP missing | also missing | The board is off, crashed, or brownouts. APs behind cascaded USB hubs have done this. |
| All missing, phone sees them | — | The scan is broken, not the grid. Check elevation first. |

## Invariants

- The laptop is a witness, never a participant (D25). If a task needs a client that sends, it runs on an ESP32.
- No hardware address reaches the output (D21), including in `--all` mode where dozens of neighbouring networks appear.
- A percentage is not a measurement: `netsh` reports signal as 0–100%, and the dBm figure shown is an approximation of Windows' own mapping. Use it to compare, not to quote.
- The parser reads English `netsh` labels. On a localised Windows it says so rather than reporting an empty grid, which would look like every AP being down.
