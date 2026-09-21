---
name: serial-log
description: Keep every connected board's console in a log file and answer questions from the files instead of opening ports. Use for any read-only question about a board's identity or state; opening a port resets the board, reading a log does not.
---

# Serial logs: read the files, not the boards

Opening a serial port resets most bench boards. D77 says to ask the hardware rather than work from
memory, and this is how asking stays cheap: a daemon pays the reset once when it attaches, then
every later question is answered from a file.

**Rule of thumb: only flashing opens a port.** Identity, status, history and "what happened at
11:40" all come from the logs. When a port must be opened, the reset is expected and fine.

## Start the daemon

```bash
python tools/serial_log.py --daemon
```

Run it in the background and leave it. It attaches to every board with a port in
`tools/bench_devices.json`, writes `logs/serial/<name>.log` with an ISO timestamp on every line,
asks each board for its ID every five minutes so the log always says whose it is, rotates at 8 MB,
and purges rotated files older than 30 days once an hour.

Pass board names to log only some of them:

```bash
python tools/serial_log.py --daemon fnk0104b e28b hosyond
```

**Attaching resets the board**, so never start the daemon on an AP in the middle of a measurement.
Start it before a test run, not during one.

## Read without touching anything

```bash
python tools/serial_log.py --status          # one line per board: last activity, ID, latest state line
python tools/serial_log.py --who             # identities, checked against the device map
python tools/serial_log.py --find "\[LORA\]" node-main --tail 40
python tools/serial_log.py --purge           # drop rotated logs over 30 days old
```

`--find` takes a regex and board names, and shows a match count plus the last few, which is what
keeps a question cheap: search for the tag you need rather than reading a log into context. The
tags worth searching are the ones the firmware uses: `[NET]`, `[BB]`, `[GRID]`, `[TIME]`, `[BLE]`,
`[ROAM]`, `[MSG]`, `[LORA]`, `[IDENTITY]`.

## Reading a log honestly

- `--who` and `--status` print **how long ago** the board last spoke. Say that age when reporting.
  An hour-old answer means the board stopped printing, not that it is fine.
- A board whose port will not open records `#### unreachable <time> port <COMx>: <reason>`, and
  `--status` shows it. An empty log means no daemon logged that board, not that the board is quiet.
- `#### attach` and `#### detach` lines mark where the logger came and went. A gap between them is
  time nobody was listening.
- A log says what a board **printed**. For what the grid currently believes, use
  `python tools/grid_watch.py`, which reads over BLE and touches nothing.

## Flashing while the daemon runs

Nothing special: `tools/flash.py` and `tools/console.py` take a hold on the port, the daemon lets
go, and it reattaches when they finish. With no daemon running the hold does nothing. If a daemon
is killed mid-hold, stale markers live in `logs/serial/.holds/` and the next daemon start clears
them.

**The reattach costs a reset too.** For flashing that is free, since the board was being reset
anyway. But it makes `tools/console.py` more expensive than it used to be: one reset to ask the
question and a second when the daemon comes back. That is the argument for `--find` over
`console.py` for anything the board already prints. Reach for `console.py` when you need to *change*
something on a board, not to look at it.
