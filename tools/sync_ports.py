#!/usr/bin/env python3
"""Re-sync the port hints in tools/bench_devices.json from what each board actually answers.

Ports on this bench shuffle whenever the hub is reseated or a native-USB board resets, and a stale
hint makes tools/flash.py refuse a board that is sitting right there. Boards are recognised by the
device ID they answer to `id`, never by the port, so the fix is to ask every port who it is and
write the answers back.

  python tools/sync_ports.py            show what each port answers and what would change
  python tools/sync_ports.py --write    write the new ports into the device map

Only the "port" field of an existing entry is ever changed. An ID that is not in the map is
reported and left alone: adding a board is tools/flash.py's job, with a device index and a name.
A board that answers nothing is reported too -- that is how a board running other firmware looks.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from flash import DEVICES, decode, load_map, query_id, save_map  # noqa: E402

try:
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing. Run inside the ESP-IDF environment.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true", help="write the new ports into the device map")
    args = ap.parse_args()

    data = load_map()
    by_id = {d["id"]: d for d in data["devices"] if d.get("id")}

    ports = sorted(p.device for p in list_ports.comports())
    if not ports:
        sys.exit("No serial ports at all. Is the hub connected?")

    print(f"{'PORT':<7} {'ANSWERED ID':<24} {'ENTRY':<14} WHAT HAPPENS")
    changes = []
    answered = set()
    for port in ports:
        got = query_id(port)
        if not got or got.startswith("ERROR"):
            why = "no answer: other firmware, or not a LocalGrid board" if not got else got
            print(f"{port:<7} {'-':<24} {'-':<14} {why}")
            continue
        answered.add(got)
        entry = by_id.get(got)
        if entry is None:
            kind = decode(data, got) or "an ID this device map does not recognise"
            print(f"{port:<7} {got:<24} {'-':<14} not in the map: {kind}")
            continue
        if entry.get("port") == port:
            print(f"{port:<7} {got:<24} {entry['name']:<14} already correct")
        else:
            was = entry.get("port") or "(none)"
            print(f"{port:<7} {got:<24} {entry['name']:<14} port {was} -> {port}")
            changes.append((entry, port))

    # An entry whose board did not answer anywhere has no port worth keeping: a stale hint is what
    # sends a flash run at whatever board happens to hold that number now.
    stale = [d for d in data["devices"]
             if d.get("port") and d.get("id") not in answered and d.get("port") not in ports]
    for d in stale:
        print(f"{'-':<7} {'-':<24} {d['name']:<14} not connected; clearing stale port {d['port']}")

    if not changes and not stale:
        print("\nEvery port hint already matches. Nothing to do.")
        return

    if not args.write:
        print(f"\n{len(changes)} port(s) to correct, {len(stale)} to clear. Re-run with --write to apply.")
        return

    for entry, port in changes:
        entry["port"] = port
    for d in stale:
        d["port"] = ""
    save_map(data)
    print(f"\nWrote {DEVICES}: {len(changes)} corrected, {len(stale)} cleared.")


if __name__ == "__main__":
    main()
