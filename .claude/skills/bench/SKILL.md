---
name: bench
description: Build, flash, and verify LocalGrid firmware on the bench boards. Use when compiling firmware, flashing a board, running the on-board tests, or reading serial logs from nodes or handhelds.
---

# Bench: build, flash, verify

## Boards

Boards, ports, board types, assigned firmware, and device IDs are listed in `tools/bench_devices.json` (`python tools/flash.py --list`). A node's index and name come from its identity partition, written by the `flash` skill; `id` on any board's serial console prints its device ID.

## Environment

Run in PowerShell. ESP-IDF v6.1 lives in `C:\esp\v6.1\esp-idf`:

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
```

A new checkout first needs `python tools/gen_secrets.py`; every board in one grid must be built from the same `lg_secrets.h`.

## Steps

1. **Build** with the `build` skill (`python tools/build.py ...`). Done when its RESULT table shows `OK` for every firmware type and target the change touches.
2. **Flash** with the `flash` skill (`python tools/flash.py ...`). Done when its RESULT table shows `OK` for every board.
3. **Verify.**
   - After an `lg_core` or `lg_crypto` change, run `tests/target` on any board: `python tools/serial_capture.py --ports COM16 --seconds 60 --reset --until LG_TESTS_RESULT`. Done when it prints `LG_TESTS_RESULT: PASS` with zero `FAIL` lines.
   - For firmware behavior, capture every involved board at once and type console commands on a schedule, for example `--ports COM16 COM17 COM18 --seconds 30 --reset --send "COM16@10:ping hello"`. Done when the expected `[BB]`, `[NET]`, `[GRID]`, and `[TIME]` lines appear on every board involved.
4. **Record.** Put measured numbers (heap, timings, binary size) in the milestone report or `CHANGELOG.md`.

## Gotchas

- An unrelated build or flash run holding a COM port makes the next flash fail with a port-busy error; only one process may open a port.
- Flashing the node firmware over a board replaces whatever it ran before, including the test app.
- Every device has a serial console (decision D28). Ask one board or many with `python tools/console.py <board|all|N|H> <command>`; it checks the board's device ID first, so a renumbered port cannot mislead it.
  - Nodes: `id`, `status`, `nodes`, `devices`, `config`, `ping [text]`, `time`, `time set <unix>`.
  - Handhelds: `id`, `status`, `nodes`, `people`, `node <index>|auto`, `scan`, `reconnect`, `time`, `reboot`.
  - Grid settings change on the master's admin page; `config` on a node is read-only.
- The development PC reaches the Internet only over Wi-Fi. Joining an `LG-*` network from it cuts the agent's own connection, so anything that needs a Wi-Fi client (the admin web page, TCP sessions) is tested from the owner's phone or a handheld, with a written procedure.
- Opening a serial port resets the Elegoo boards through their auto-reset circuit, even with DTR and RTS preset low.
- `tools/serial_capture.py --reset-at PORT@SECONDS` (repeatable) reboots a board mid-capture, including the ESP32-S3's native USB port. Use it to reboot a handheld while nodes keep running.
- Closing a port the plain way holds auto-reset boards in reset, so a node looks dead or "loses links" after a tool exits. Set DTR and RTS to False, wait about 0.1 s, then close; `tools/flash.py` (`release_and_close()`) and `tools/serial_capture.py` already do this. A board stuck this way recovers when its port is opened and release-closed once. Grid time lives only in RAM, so a reset of every node at once loses it until the admin sets it again.
- To update nodes without losing grid time, flash them one at a time with about 10 s between boards: each rebooted node re-adopts time from its still-running neighbors on link-up. Read state from NORTH or SOUTH rather than opening the master's port.
