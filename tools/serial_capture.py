#!/usr/bin/env python3
"""Capture serial output from one or more boards at once, optionally typing console commands.

Examples:
  python tools/serial_capture.py --ports COM16 --seconds 60 --until LG_TESTS_RESULT
  python tools/serial_capture.py --ports COM16 COM17 COM18 --seconds 25 --reset \
      --send "COM16@8:time set 1790000000" --send "COM16@12:ping hello" --send "COM17@16:nodes"

Output lines are prefixed with the port name. Requires pyserial (present in the ESP-IDF Python env).
"""
import argparse
import re
import sys
import threading
import time

import serial

# Decision D21: hardware addresses never appear in tool output, even if a library logs one.
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")


def parse_send(spec):
    port, rest = spec.split("@", 1)
    delay, command = rest.split(":", 1)
    return port, float(delay), command


def release(ser):
    """Drop both control lines, then close. A plain close holds CP210x and CH340 boards in reset."""
    try:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.1)
    except (OSError, ValueError):
        pass
    ser.close()


def pulse_reset(ser):
    """Pulse EN through RTS with IO0 left high (DTR low).

    Windows' usbser.sys driver, used by the ESP32-S3 USB-Serial/JTAG port, sends a line-state
    change only when DTR is written as well, so DTR is rewritten after each RTS change, as esptool
    does. On CP210x and CH340 boards the extra writes change nothing.
    """
    ser.dtr = False
    ser.rts = True
    ser.dtr = False
    time.sleep(0.2)
    ser.rts = False
    ser.dtr = False


def reader(port, ser, stop, out_lock, until, hit):
    buf = b""
    while not stop.is_set():
        chunk = ser.read(1024)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = MAC_RE.sub("xx:xx:xx:xx:xx:xx", line.decode("utf-8", "replace").rstrip("\r"))
            with out_lock:
                print(f"[{port}] {text}", flush=True)
            if until and until in text:
                hit.set()


def main():
    # Board output includes UTF-8 (emoji in messages, box drawing). Redirected to a file on Windows,
    # stdout is cp1252 and one such line killed that port's reader thread mid-capture.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ports", nargs="+", required=True)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true", help="pulse EN via RTS on open (CP210x/CH340/USB-JTAG)")
    ap.add_argument("--until", help="stop early once this text appears on any port")
    ap.add_argument("--send", action="append", default=[], help="PORT@SECONDS:command, typed after SECONDS")
    ap.add_argument("--reset-at", action="append", default=[],
                    help="PORT@SECONDS: pulse EN via RTS after SECONDS (simulates a power cycle)")
    args = ap.parse_args()

    stop, hit, out_lock = threading.Event(), threading.Event(), threading.Lock()
    sers, threads = {}, []
    for port in args.ports:
        try:
            ser = serial.Serial(port, args.baud, timeout=0.1, write_timeout=0.5)
        except serial.SerialException as e:
            # Release every port already opened: a plain close holds auto-reset boards in reset,
            # so one bad port name must not leave nodes dead.
            for opened in sers.values():
                release(opened)
            sys.exit(f"{port}: {e}")
        if args.reset:
            pulse_reset(ser)
        sers[port] = ser
        t = threading.Thread(target=reader, args=(port, ser, stop, out_lock, args.until, hit), daemon=True)
        t.start()
        threads.append(t)

    events = [(delay, "send", port, command) for port, delay, command in (parse_send(s) for s in args.send)]
    for spec in args.reset_at:
        port, delay = spec.split("@", 1)
        events.append((float(delay), "reset", port, ""))
    events.sort(key=lambda e: e[0])

    start = time.time()
    while time.time() - start < args.seconds and not hit.is_set():
        now = time.time() - start
        while events and events[0][0] <= now:
            _, kind, port, command = events.pop(0)
            with out_lock:
                print(f"[{port}] >>> {'RESET' if kind == 'reset' else command}  (t={now:.1f}s)", flush=True)
            if kind == "reset":
                pulse_reset(sers[port])
            else:
                try:
                    sers[port].write((command + "\r\n").encode())
                except serial.SerialTimeoutException:
                    with out_lock:
                        print(f"[{port}] (write timed out: board is not reading its console)", flush=True)
        time.sleep(0.05)
    if hit.is_set():
        time.sleep(0.3)

    stop.set()
    for t in threads:
        t.join(timeout=1)
    for ser in sers.values():
        release(ser)
    return 0


if __name__ == "__main__":
    sys.exit(main())
