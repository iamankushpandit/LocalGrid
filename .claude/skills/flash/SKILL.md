---
name: flash
description: Flash LocalGrid firmware to one bench board, a group, or all boards with each board's assigned firmware, and manage device IDs. Use when flashing, reflashing, factory-resetting, provisioning a new board, or identifying which board is on a port.
---

# Flash bench boards

`tools/bench_devices.json` is the single source of truth: board types, firmware types, and each board's name, port hint, role, board type, assigned firmware, and device ID. It holds no MAC addresses. `tools/flash.py` reads and updates it.

## Device IDs

Format `LG-<role>-<board>-<10 base32 characters>`, for example `LG-N-ELG-7K3QX9PA2M`.

- **Role**: `N` infrastructure node, `H` handheld. **Board**: a code from the `boards` table (for example `ELG`, `HY3`, `F4B`). `python tools/flash.py --decode <ID>` prints the device type.
- The tail hashes the board's MAC, the provisioning time, a text label, and random bytes. The MAC is read in memory only and cannot be recovered from the ID.
- The ID lives in the `lgid` partition, which normal flashing never touches. Every firmware answers the serial command `id` with a line starting `LGID:`.
- The tool recognizes boards by asking for this ID before it writes anything.

## Steps

1. **Load ESP-IDF** in PowerShell: `. C:\esp\v6.1\esp-idf\export.ps1`.
2. **Check the map.** `python tools/flash.py --list`. Add a new board as a device entry with `name`, `port`, `role`, `board`, `firmware` (nodes also `node_index` and `node_name`; handhelds also `device_index`, their address in the grid roster) and an empty `id`. Done when every board you will flash is listed with the firmware it should run.
3. **Flash**:

   | Goal | Command |
   |---|---|
   | One board, keep its settings | `python tools/flash.py node-north` (name, port, or ID) |
   | Several boards | `python tools/flash.py node-main hosyond` |
   | All nodes, one at a time | `python tools/flash.py --role N` |
   | Every board | `python tools/flash.py --all` |
   | Factory reset: erase everything, keep the ID | `python tools/flash.py hosyond --erase` |
   | Replace a board's ID | `python tools/flash.py hosyond --erase --new-id` |
   | Different firmware this run | `python tools/flash.py hosyond --firmware tests` |
   | Apply a changed `node_index`, `node_name`, or `device_index` without erasing | `python tools/flash.py hosyond --update-identity` |

   The script checks the board answers the expected ID, and that the chip matches the board type. A board without an ID is provisioned with a new one. The script builds each firmware once per target and refuses builds with warnings. Handhelds flash first, then nodes one at a time with 10 s between them so the grid keeps its time.
4. **Read the RESULT table.** Done when every selected board shows `OK` and `id ok`.
5. **Commit the map.** New or changed IDs are written into `tools/bench_devices.json`; keep it in the same commit as the flash that created them, with a `CHANGELOG.md` entry.

## What each mode clears

| Mode | Keeps | Clears |
|---|---|---|
| default | admin setup, boot counter, device ID | nothing except the app |
| `--update-identity` | admin setup, boot counter, touch calibration, keys, device ID | nothing; the identity partition is rewritten from the map |
| `--erase` | device ID (rewritten from the map) | admin setup, boot counter, all saved settings |
| `--erase --new-id` | nothing | everything, and the old ID is retired |

## When a board fails

| Failure | Meaning and fix |
|---|---|
| `Another flash.py run (process N) is using the boards` | A run is still going, possibly started by an earlier session in the background. Let it finish, or stop that process if its session is gone. A lock left by a dead run clears itself. |
| `port COMx has LG-..., expected LG-...` | A different board is on that port. Run `python tools/flash.py --identify` and fix the `port` fields. |
| `board did not answer 'id'` | It runs firmware older than device IDs, or the port is wrong. If the port is right, add `--trust-port` once. |
| `chip ... needs ...` | The board type in the map does not match the hardware on that port. |
| `build has N warning(s)` | Fix the warnings; builds must be warning-free. |
| `flash failed` | Another program holds the port, or the board needs BOOT held during connect. |
| `no '...' within N s`, `crashed`, or `answered id ...` | Firmware did not start cleanly or identity is wrong. Capture with `python tools/serial_capture.py --ports COMx --seconds 30`. |

`--identify` opens each port, which resets most boards; identifying every node at once loses grid time until the admin sets it again.
