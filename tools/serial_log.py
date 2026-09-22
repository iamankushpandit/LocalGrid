#!/usr/bin/env python3
"""Keep every connected board's console in a log file, so reading its state costs no reset.

Opening a serial port resets most boards (see release() below), so asking a board a question is
never free: it restarts, rejoins the grid, and whatever was being measured is gone. D77 says to
ask the hardware rather than work from memory; this tool makes asking cheap by paying the reset
once, when the logger attaches, and then never again. Reads come from the files.

  python tools/serial_log.py --daemon        attach to every mapped port and keep logging
  python tools/serial_log.py --status        one line per board, from the logs, no port opened
  python tools/serial_log.py --who           device identities, from the logs, no port opened
  python tools/serial_log.py --find "\\[BB\\]" --name node-main --tail 40
  python tools/serial_log.py --purge         drop log files older than KEEP_DAYS

A port can only be held by one program at a time, so the daemon yields it on request: anything
that needs the port (tools/flash.py, tools/console.py) takes a hold with hold_port() and the
daemon closes that port until the hold is dropped. With no daemon running a hold does nothing,
so callers need no special case.
"""
import argparse
import contextlib
import datetime as dt
import json
import os
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEVICES = ROOT / "tools" / "bench_devices.json"
LOGS = ROOT / "logs" / "serial"
HOLDS = LOGS / ".holds"

KEEP_DAYS = 30            # owner, 2026-09-21: purge logs a month old
ROTATE_BYTES = 8 << 20    # 8 MB per board, then .1, .2 ...
ROTATE_KEEP = 4

# D21: hardware addresses never appear in tool output, even if a library logs one.
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
LGID_RE = re.compile(r"LGID: (LG-[A-Z]-[A-Z0-9]{3}-[0-9A-HJKMNP-TV-Z]{10}|NONE)\s")
ATTACH_RE = re.compile(r"^#### attach (\S+) port (\S+)(?: id (\S+))?")

# Lines worth surfacing in --status without reading a whole log. Kept small on purpose: the point
# of this tool is to answer a question with a few lines, not to move a log file into a context
# window. Add a tag here only when it says something about a board's state.
STATE_TAGS = ("[NET]", "[BB]", "[GRID]", "[TIME]", "[ROAM]", "[IDENTITY]", "[LORA]", "[UI]")
BOOT_RE = re.compile(r"(rst:0x|Build:|\[UI\] Launcher ready|\[NET\] Listening)")


def devices():
    return json.loads(DEVICES.read_text(encoding="utf-8"))


def log_path(name):
    return LOGS / f"{name}.log"


def scrub(text):
    return MAC_RE.sub("<addr>", text)


# ---- holds: let another tool take a port the daemon is logging ----

def hold_path(port):
    return HOLDS / f"{port}.hold"


@contextlib.contextmanager
def hold_port(port, wait_s=12.0):
    """Ask the daemon to let go of `port` for the duration of the block.

    Does nothing useful when no daemon is running, which is the point: callers do not have to know
    whether one is. Waits for the daemon to confirm the port is closed before yielding.
    """
    HOLDS.mkdir(parents=True, exist_ok=True)
    p = hold_path(port)
    p.write_text(f"{os.getpid()} {dt.datetime.now().isoformat(timespec='seconds')}\n", encoding="utf-8")
    open_marker = HOLDS / f"{port}.open"
    deadline = time.time() + wait_s
    while open_marker.exists() and time.time() < deadline:
        time.sleep(0.25)
    try:
        yield
    finally:
        with contextlib.suppress(OSError):
            p.unlink()


def held(port):
    return hold_path(port).exists()


# ---- the daemon ----

def release(ser):
    """Drop RTS before DTR, then close: a plain close holds CP210x and CH340 boards in reset."""
    try:
        ser.rts = False
        ser.dtr = False
        time.sleep(0.1)
    except (OSError, ValueError):
        pass
    with contextlib.suppress(Exception):
        ser.close()


def rotate(path):
    if not path.exists() or path.stat().st_size < ROTATE_BYTES:
        return
    for i in range(ROTATE_KEEP - 1, 0, -1):
        older, newer = path.with_suffix(f".log.{i + 1}"), path.with_suffix(f".log.{i}")
        if newer.exists():
            with contextlib.suppress(OSError):
                newer.replace(older)
    with contextlib.suppress(OSError):
        path.replace(path.with_suffix(".log.1"))


class Board:
    """One mapped board: its port, its log file, and whatever connection it currently has."""

    def __init__(self, dev):
        self.name = dev["name"]
        self.port = dev["port"]
        self.expected = dev.get("id") or ""
        self.ser = None
        self.buf = b""
        self.asked_id = 0.0
        self.next_try = 0.0
        self.fh = None
        self.trouble = ""     # the last reason this port would not open, so the log can say so

    def write(self, text):
        if self.fh is None:
            LOGS.mkdir(parents=True, exist_ok=True)
            rotate(log_path(self.name))
            self.fh = log_path(self.name).open("a", encoding="utf-8", errors="replace")
        self.fh.write(text)
        self.fh.flush()

    def stamp(self, line):
        return f"{dt.datetime.now().isoformat(timespec='seconds')} {scrub(line)}\n"

    def open(self):
        import serial
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=0.2, write_timeout=0.5)
        except Exception as e:
            # A port that will not open is the most likely thing a reader asks about later, and
            # silence reads as "nothing happened" rather than "could not reach the board". Record
            # each distinct reason once, so --status can explain an empty log instead of guessing.
            reason = f"{type(e).__name__}: {e}"
            if reason != self.trouble:
                self.trouble = reason
                self.write(f"#### unreachable {dt.datetime.now().isoformat(timespec='seconds')} "
                           f"port {self.port}: {scrub(reason)}\n")
            self.next_try = time.time() + 10
            return False
        self.trouble = ""
        (HOLDS / f"{self.port}.open").write_text("", encoding="utf-8")
        self.write(f"#### attach {dt.datetime.now().isoformat(timespec='seconds')} port {self.port}\n")
        self.asked_id = 0.0
        self.buf = b""
        return True

    def close(self, why=""):
        if self.ser is not None:
            release(self.ser)
            self.ser = None
            self.write(f"#### detach {dt.datetime.now().isoformat(timespec='seconds')}{' ' + why if why else ''}\n")
        with contextlib.suppress(OSError):
            (HOLDS / f"{self.port}.open").unlink()

    def pump(self):
        """Read whatever arrived, log it, and ask now and then for an ID so a log can say whose it is."""
        try:
            data = self.ser.read(4096)
        except Exception as e:
            self.close(f"read failed: {type(e).__name__}")
            self.next_try = time.time() + 5
            return
        if data:
            self.buf += data
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    self.write(self.stamp(text))
        # The board prints its ID at boot, but the logger may have attached long after one. Ask
        # once, a couple of seconds in, so every log carries the identity of the board that wrote
        # it: that is what makes --who free. Re-ask after a reboot, which is why this is by time.
        if time.time() - self.asked_id > 300:
            self.asked_id = time.time()
            with contextlib.suppress(Exception):
                self.ser.write(b"id\r\n")


def daemon(names, quiet):
    data = devices()
    chosen = [d for d in data["devices"] if d["port"] and (not names or d["name"] in names)]
    if not chosen:
        sys.exit("no mapped boards with ports; set ports in tools/bench_devices.json")
    LOGS.mkdir(parents=True, exist_ok=True)
    HOLDS.mkdir(parents=True, exist_ok=True)
    # A stale .open marker from a killed daemon would make hold_port() wait its whole timeout.
    # Only this daemon's own ports, so a second daemon watching other boards does not wipe the
    # markers of the first and quietly break its holds.
    for dev in chosen:
        with contextlib.suppress(OSError):
            (HOLDS / f"{dev['port']}.open").unlink()
    boards = [Board(d) for d in chosen]
    if not quiet:
        print(f"logging {len(boards)} board(s) into {LOGS}")
        for b in boards:
            print(f"  {b.name:<14} {b.port}")
        print("Ctrl-C to stop. Reads: python tools/serial_log.py --status")
    last_purge = 0.0
    try:
        while True:
            for b in boards:
                if held(b.port):
                    if b.ser is not None:
                        b.close("port held by another tool")
                    continue
                if b.ser is None:
                    if time.time() < b.next_try:
                        continue
                    b.open()
                    continue
                b.pump()
            if time.time() - last_purge > 3600:
                last_purge = time.time()
                purge(quiet=True)
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        for b in boards:
            b.close("daemon stopping")
            if b.fh:
                b.fh.close()


# ---- reads: everything below opens no serial port ----

def read_lines(name, limit=4000):
    p = log_path(name)
    if not p.exists():
        return []
    with p.open("r", encoding="utf-8", errors="replace") as f:
        return f.readlines()[-limit:]


def age(stamp):
    try:
        t = dt.datetime.fromisoformat(stamp)
    except ValueError:
        return None
    return (dt.datetime.now() - t).total_seconds()


def human(seconds):
    if seconds is None:
        return "?"
    if seconds < 90:
        return f"{int(seconds)}s"
    if seconds < 5400:
        return f"{int(seconds / 60)}m"
    return f"{seconds / 3600:.1f}h"


def last_id(lines):
    """The most recent identity this board reported, and how long ago."""
    for line in reversed(lines):
        m = LGID_RE.search(line + " ")
        if m:
            return m.group(1), age(line.split(" ", 1)[0])
    return None, None


def status(names):
    data = devices()
    chosen = [d for d in data["devices"] if not names or d["name"] in names]
    print(f"{'NAME':<14} {'PORT':<7} {'LAST':<6} {'ID (from log)':<24} LATEST STATE LINE")
    for d in chosen:
        lines = read_lines(d["name"], 1500)
        if not lines:
            why = "no log yet (daemon not running, or never asked to log this board)"
            print(f"{d['name']:<14} {d['port'] or '-':<7} {'-':<6} {'-':<24} {why}")
            continue
        if lines[-1].startswith("#### unreachable"):
            # "#### unreachable <iso> port COMx: <reason>" - keep the reason, drop the bookkeeping.
            reason = lines[-1].split(f"port {d['port']}:", 1)[-1].strip()
            when = human(age(lines[-1].split(" ")[2]))
            print(f"{d['name']:<14} {d['port'] or '-':<7} {when:<6} {'-':<24} will not open: {reason[:52]}")
            continue
        fresh = age(lines[-1].split(" ", 1)[0])
        ident, _ = last_id(lines)
        note = ""
        if ident and d.get("id") and ident != d["id"]:
            note = f"  MISMATCH: map expects {d['id']}"
        latest = ""
        for line in reversed(lines):
            if any(tag in line for tag in STATE_TAGS):
                latest = line.split(" ", 1)[-1].strip()[:70]
                break
        print(f"{d['name']:<14} {d['port'] or '-':<7} {human(fresh):<6} {ident or '-':<24} {latest}{note}")
    print("\nfrom the logs, no board was opened or reset; 'LAST' is how long ago it last printed")


def who(names):
    data = devices()
    chosen = [d for d in data["devices"] if not names or d["name"] in names]
    print(f"{'NAME':<14} {'PORT':<7} {'ID (from log)':<24} {'HEARD':<7} MAP SAYS")
    stale = False
    for d in chosen:
        ident, when = last_id(read_lines(d["name"], 3000))
        agreed = "matches" if ident and ident == d.get("id") else (d.get("id") or "-")
        if ident and d.get("id") and ident != d["id"]:
            agreed = f"MISMATCH: map expects {d['id']}"
        if when is not None and when > 3600:
            stale = True
        print(f"{d['name']:<14} {d['port'] or '-':<7} {ident or '-':<24} {human(when):<7} {agreed}")
    print("\nfrom the logs, no board was opened or reset")
    if stale:
        print("some answers are over an hour old; the daemon re-asks every 5 minutes, so a stale")
        print("row means that board stopped printing (unplugged, wedged, or no daemon running)")


def find(pattern, names, tail):
    rx = re.compile(pattern)
    data = devices()
    chosen = [d for d in data["devices"] if not names or d["name"] in names]
    total = 0
    for d in chosen:
        hits = [l.rstrip("\n") for l in read_lines(d["name"], 20000) if rx.search(l)]
        if not hits:
            continue
        print(f"== {d['name']} ({len(hits)} match{'es' if len(hits) != 1 else ''}, last {min(tail, len(hits))})")
        for line in hits[-tail:]:
            print(f"  {line}")
        total += len(hits)
    if not total:
        print(f"no match for {pattern!r} in the logs")


def purge(quiet=False):
    cutoff = time.time() - KEEP_DAYS * 86400
    gone = 0
    for p in LOGS.glob("*.log.*"):
        if p.stat().st_mtime < cutoff:
            with contextlib.suppress(OSError):
                p.unlink()
                gone += 1
    if not quiet:
        print(f"purged {gone} rotated log file(s) older than {KEEP_DAYS} days from {LOGS}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="board names from tools/bench_devices.json (default all)")
    ap.add_argument("--daemon", action="store_true", help="attach to the ports and keep logging")
    ap.add_argument("--status", action="store_true", help="one line per board, read from the logs")
    ap.add_argument("--who", action="store_true", help="device identities, read from the logs")
    ap.add_argument("--find", metavar="REGEX", help="search the logs")
    ap.add_argument("--tail", type=int, default=20, help="matches to show per board with --find")
    ap.add_argument("--purge", action="store_true", help=f"drop rotated logs older than {KEEP_DAYS} days")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.daemon:
        daemon(args.names, args.quiet)
    elif args.who:
        who(args.names)
    elif args.find:
        find(args.find, args.names, args.tail)
    elif args.purge:
        purge()
    else:
        status(args.names)


if __name__ == "__main__":
    main()
