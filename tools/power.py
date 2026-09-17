#!/usr/bin/env python3
"""Ask bench boards for their supply voltage, all at the same time, and print one table.

Every LocalGrid firmware answers `power` on its serial console (lg_power.h):
    power                    one reading
    power -m <s> [-i <ms>]   monitor for s seconds, then min, average, and max
Boards that cannot measure (the Elegoo APs have no sense pin) say so and report restart
causes instead, which is where a sagging supply shows on them.

Examples:
  python tools/power.py all                    one reading from every board
  python tools/power.py all -m 60              every board monitored over the same minute
  python tools/power.py H -m 30 -i 250         handhelds, four readings a second
  python tools/power.py hosyond -m 10 --list   also print each reading

Boards are queried in parallel on purpose: a hub that sags when the radios transmit sags for
every board at once, and only simultaneous monitoring shows that.

Opening a port resets most boards (an AP loses grid time until a neighbour announces it), and
the reading is then taken a few seconds into boot. Use --settle to wait before measuring.
Decision D21: hardware addresses never appear in the output.
"""
import argparse
import pathlib
import re
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from console import MAC_RE, PROMPT, choose, write_line  # noqa: E402
from flash import LGID, acquire_lock, load_map, release_and_close  # noqa: E402

BOOT_WAIT_S = 12.0
RESULT_RE = re.compile(r"POWER_RESULT samples=(\d+) min=(\d+) avg=(\d+) max=(\d+) mV")
NOW_RE = re.compile(r"POWER: supply (\d+) mV")
SAMPLE_RE = re.compile(r"POWER_SAMPLE (\d+) ms (\d+) mV")


def query(device, command, seconds, settle, out):
    import serial

    row = {"board": device["name"], "port": device["port"], "ok": False, "lines": [], "note": ""}
    out.append(row)
    try:
        ser = serial.Serial(device["port"], 115200, timeout=0.2, write_timeout=0.5)
    except serial.SerialException as e:
        row["note"] = f"serial error: {e}"
        return
    try:
        text = ""
        deadline = time.time() + BOOT_WAIT_S
        while time.time() < deadline and PROMPT not in text:
            text += ser.read(512).decode("utf-8", "replace")
        m = LGID.search(text)
        if m and device.get("id") and m.group(1) != device["id"]:
            row["note"] = f"port answered {m.group(1)}, expected {device['id']}"
            return
        if settle:
            time.sleep(settle)
        ser.reset_input_buffer()
        if not write_line(ser, command):
            row["note"] = "could not type the command"
            return
        reply = ""
        # A monitored reading replies after `seconds`; allow for boot noise and the footer.
        deadline = time.time() + seconds + 10.0
        done_at = None
        while time.time() < deadline:
            chunk = ser.read(512).decode("utf-8", "replace")
            reply += chunk
            finished = ("POWER_RESULT" in reply) if seconds else ("POWER: this boot started after" in reply)
            if finished and done_at is None:
                done_at = time.time()
            if done_at is not None and time.time() - done_at > 0.8:   # let the footer lines arrive
                break
        for line in reply.splitlines():
            line = MAC_RE.sub("xx:xx:xx:xx:xx:xx", line.replace(PROMPT, "").strip())
            if line.startswith(("POWER", "Restarts since")):
                row["lines"].append(line)
        row["ok"] = any(l.startswith("POWER") for l in row["lines"])
        if not row["ok"]:
            row["note"] = "no reply to power (firmware without the command?)"
    except serial.SerialException as e:
        row["note"] = f"serial error: {e}"
    finally:
        release_and_close(ser)


def summarize(row, seconds):
    text = "\n".join(row["lines"])
    if not row["ok"]:
        return "FAILED", row["note"]
    if "not measurable" in text:
        return "no sense pin", ""
    if seconds:
        m = RESULT_RE.search(text)
        if m:
            n, lo, avg, hi = (int(x) for x in m.groups())
            return f"avg {avg / 1000:.2f} V", f"min {lo / 1000:.2f}  max {hi / 1000:.2f}  swing {(hi - lo)} mV  ({n} readings)"
        return "no result", ""
    m = NOW_RE.search(text)
    return (f"{int(m.group(1)) / 1000:.2f} V", "") if m else ("no result", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help='board name, port, or ID, or "all", "N", "H"')
    ap.add_argument("-m", "--monitor", type=int, default=0, metavar="SECONDS", help="monitor for this long")
    ap.add_argument("-i", "--interval", type=int, default=1000, metavar="MS", help="between readings while monitoring")
    ap.add_argument("--list", action="store_true", help="print every reading, not just the summary")
    ap.add_argument("--settle", type=float, default=0.0, metavar="SECONDS",
                    help="wait this long after the board boots before measuring")
    args = ap.parse_args()

    devices = choose(load_map(), args.board)
    command = "power" if not args.monitor else f"power -m {args.monitor} -i {args.interval}{'' if args.list else ' -q'}"
    acquire_lock()   # one tool owns the ports at a time

    rows = []
    threads = [threading.Thread(target=query, args=(d, command, args.monitor, args.settle, rows)) for d in devices]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    order = {d["name"]: i for i, d in enumerate(devices)}
    rows.sort(key=lambda r: order.get(r["board"], 99))
    print(f"Supply {'over ' + str(args.monitor) + ' s' if args.monitor else 'now'} ({command})")
    failures = 0
    for row in rows:
        head, detail = summarize(row, args.monitor)
        failures += head == "FAILED"
        print(f"  {row['board']:<12} {row['port']:<6} {head:<14} {detail}")
        text = "\n".join(row["lines"])
        restart = re.search(r"POWER: this boot started after: (.*)", text)
        counts = re.search(r"Restarts since counting began: (.*)", text)
        if restart:
            print(f"  {'':<19} this boot after: {restart.group(1)}")
        if counts:
            print(f"  {'':<19} restarts: {counts.group(1)}")
        if args.list:
            for line in row["lines"]:
                s = SAMPLE_RE.search(line)
                if s:
                    print(f"  {'':<19} {int(s.group(1)) / 1000:7.1f} s  {int(s.group(2)) / 1000:.3f} V")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
