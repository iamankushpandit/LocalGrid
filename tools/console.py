#!/usr/bin/env python3
"""Ask a bench board a console command over its serial port and print the reply.

Every LocalGrid device answers status and configuration commands (decision D28), the same
way it answers `id` while flashing. Boards come from tools/bench_devices.json, and the
board's device ID is checked before the command is sent, so a command never reaches the
wrong board after a replug renumbers the ports.

Examples:
  python tools/console.py node-main status
  python tools/console.py node-main config
  python tools/console.py hosyond nodes
  python tools/console.py fnk0104b node 1
  python tools/console.py all status
  python tools/console.py H people

Boards: a name, port, or ID from the device map, or "all", "N" (nodes), or "H" (handhelds).
Opening a port resets most boards: a node loses grid time, which lives only in RAM, and a
handheld reconnects. Use tools/serial_capture.py when you need to watch without commands.
"""
import argparse
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from flash import LGID, acquire_lock, load_map, release_and_close  # noqa: E402

# Decision D21: hardware addresses never appear in tool output, even if firmware logs one.
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
LOG_RE = re.compile(r"^[IWED] \(\d+\) ")   # ESP-IDF log lines: shown only with --raw
PROMPT = "grid>"
ID_WAIT_S = 9.0
PROMPT_WAIT_S = 12.0   # opening the port resets the board; its console starts after boot
QUIET_S = 1.0
REPLY_MAX_S = 15.0


def choose(data, key):
    devices = data["devices"]
    if key == "all":
        return list(devices)
    if key in ("N", "H"):
        return [d for d in devices if d["role"] == key]
    found = [d for d in devices if key.lower() in (d["name"].lower(), d["port"].lower(), (d.get("id") or "").lower())]
    if not found:
        sys.exit(f"Unknown board '{key}'. Run 'python tools/flash.py --list' for names, ports, and IDs.")
    return found


def send_and_read(ser, command, raw):
    """Types a command and collects the reply until the board goes quiet."""
    ser.reset_input_buffer()
    ser.write((command + "\r\n").encode())
    reply = ""
    last = time.time()
    stop = time.time() + REPLY_MAX_S
    while time.time() - last < QUIET_S and time.time() < stop:
        chunk = ser.read(512).decode("utf-8", "replace")
        if chunk:
            reply += chunk
            last = time.time()
    lines = []
    for line in reply.splitlines():
        line = MAC_RE.sub("xx:xx:xx:xx:xx:xx", line.rstrip())
        clean = line.replace(PROMPT, "").strip()
        if not clean or clean == command:
            continue
        if LOG_RE.match(clean) and not raw:
            continue
        lines.append(line.rstrip())
    return lines


def ask(device, command, trust_port, raw=False):
    import serial

    try:
        ser = serial.Serial(device["port"], 115200, timeout=0.2, write_timeout=0.5)
    except serial.SerialException as e:
        return False, f"serial error: {e}"
    try:
        # Identify first, exactly as flashing does.
        text = ""
        answered = None
        deadline = time.time() + ID_WAIT_S
        ser.write(b"\r\n")
        ser.write(b"id\r\n")
        while time.time() < deadline and answered is None:
            text += ser.read(512).decode("utf-8", "replace")
            m = LGID.search(text)
            answered = m.group(1) if m else None
        expected = device.get("id")
        if answered is None and not trust_port:
            return False, "board did not answer 'id'; check the port or pass --trust-port"
        if expected and answered and answered != expected:
            return False, f"port {device['port']} answered {answered}, expected {expected}"

        # Wait for the console prompt: keystrokes sent while the board is still booting are lost.
        prompt_deadline = time.time() + PROMPT_WAIT_S
        while PROMPT not in text and time.time() < prompt_deadline:
            text += ser.read(512).decode("utf-8", "replace")

        lines = send_and_read(ser, command, raw)
        if not lines:
            lines = send_and_read(ser, command, raw)   # one retry: the prompt may have just appeared
        return True, "\n".join(lines) if lines else "(no reply; try --raw to see the board's log)"
    except serial.SerialException as e:
        return False, f"serial error: {e}"
    finally:
        release_and_close(ser)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help='board name, port, or ID, or "all", "N", "H"')
    ap.add_argument("command", nargs="+", help="console command, for example: status")
    ap.add_argument("--trust-port", action="store_true", help="send even if the board does not answer 'id'")
    ap.add_argument("--raw", action="store_true", help="keep the board's log lines in the reply")
    args = ap.parse_args()

    data = load_map()
    chosen = choose(data, args.board)
    command = " ".join(args.command)
    acquire_lock()   # shares the flash lock: one tool owns the ports at a time

    failures = 0
    for device in chosen:
        print(f"== {device['name']} ({device['port']}) <- {command}", flush=True)
        ok, reply = ask(device, command, args.trust_port, args.raw)
        print(reply if ok else f"  FAILED: {reply}", flush=True)
        failures += 0 if ok else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
