#!/usr/bin/env python3
"""Check which LocalGrid APs are actually on the air, using this laptop's Wi-Fi radio.

Every earlier answer about AP visibility came from one of two places: the handhelds' own
radios, which see Espressif beacons and attach happily, or somebody looking at a phone's
Wi-Fi list. Neither is an instrument. This is: an independent client-side view that says
which SSIDs are being beaconed, how strong they are, and on which channel.

Decision D25 stands. The laptop never joins the grid, never carries a message, and never
stands in for a handheld or an AP. It reads what Windows heard, the same way the PC reads a
serial log, and reports it.

Being precise about the radio, because "it only listens" would be too strong: Windows may
answer this query with an active scan, which sends probe requests from the laptop's own
radio. It never associates with an AP and never speaks the LocalGrid protocol, so nothing
here is a grid participant -- but it is not guaranteed to be passive either.

Decision D21 stands too: BSSIDs are hardware addresses, so they are never printed. Radios
are counted and their signals aggregated per SSID instead.

Since D46 every AP broadcasts the same SSID, "LocalMesh Access Point", like a home mesh
router. A Wi-Fi scan therefore cannot say which AP is which (their names travel in a vendor
element Windows does not show); it counts how many radios answer on that SSID. Which AP is
missing comes from the APs' own `nodes` output or a handheld's Status screen.

  python tools/wifi_scan.py                     is the grid SSID up, and on how many APs
  python tools/wifi_scan.py --aps 2             expect two APs instead of three
  python tools/wifi_scan.py --all               every network in range, LocalGrid or not
  python tools/wifi_scan.py --watch 120         keep looking for two minutes, log changes

Exit code is 0 when the expected number of APs was heard, 1 when fewer were, 2 when the scan
itself could not run. So it can gate a flash: `python tools/flash.py --role N && python
tools/wifi_scan.py`.

Windows only, because it shells out to netsh. The parser reads English netsh output; on a
localised Windows the field labels differ and it will say so rather than report nothing.
"""
import argparse
import re
import subprocess
import sys
import time

# One SSID on every AP (D46), every AP at 192.168.4.1, and no master (D45): any AP serves
# the admin page, so a missing AP reduces coverage but blocks nothing on its own.
GRID_SSID = "LocalMesh Access Point"
DEFAULT_APS = 3
ADMIN_URL = "http://192.168.4.1/"

# D21: a hardware address never reaches the output, even if netsh prints one.
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")

SSID_RE = re.compile(r"^\s*SSID\s+\d+\s*:\s*(.*)$")
AUTH_RE = re.compile(r"^\s*Authentication\s*:\s*(.*)$")
SIGNAL_RE = re.compile(r"^\s*Signal\s*:\s*(\d+)\s*%")
CHANNEL_RE = re.compile(r"^\s*Channel\s*:\s*(\d+)")


def netsh():
    """Ask Windows what it can hear. Returns the text, or None with a reason printed."""
    try:
        p = subprocess.run(["netsh", "wlan", "show", "networks", "mode=bssid"],
                           capture_output=True, text=True, timeout=30)
    except FileNotFoundError:
        print("netsh not found: this script needs Windows.")
        return None
    except subprocess.TimeoutExpired:
        print("netsh did not answer within 30 s.")
        return None
    if p.returncode != 0:
        print(f"netsh failed ({p.returncode}): {(p.stderr or p.stdout).strip()[:200]}")
        return None
    text = p.stdout
    if "There is no wireless interface" in text or "not running" in text:
        print("No usable wireless interface. Is Wi-Fi switched on?")
        return None
    return text


def parse(text):
    """SSID -> {signal: best %, channels: set, radios: count, auth: str}.

    Aggregated per SSID on purpose: a mesh AP can answer on more than one BSSID, and a BSSID
    is a hardware address we do not print (D21). The count says how many were heard.
    """
    nets = {}
    current = None
    for line in text.splitlines():
        m = SSID_RE.match(line)
        if m:
            name = m.group(1).strip()
            current = name if name else "(hidden)"
            nets.setdefault(current, {"signal": 0, "channels": set(), "radios": 0, "auth": "?"})
            continue
        if current is None:
            continue
        m = AUTH_RE.match(line)
        if m:
            nets[current]["auth"] = m.group(1).strip()
            continue
        m = SIGNAL_RE.match(line)
        if m:
            pct = int(m.group(1))
            nets[current]["radios"] += 1
            if pct > nets[current]["signal"]:
                nets[current]["signal"] = pct
            continue
        m = CHANNEL_RE.match(line)
        if m:
            nets[current]["channels"].add(int(m.group(1)))
    return nets


def approx_dbm(pct):
    """netsh reports a percentage. Windows maps -100..-50 dBm onto 0..100, so this inverts
    that. Approximate by construction: treat it as a strength indication, not a measurement."""
    return -100 + (pct / 2.0)


def describe(name, info):
    chans = ",".join(str(c) for c in sorted(info["channels"])) or "?"
    radios = f", {info['radios']} radios" if info["radios"] > 1 else ""
    return (f"{name:<16} {info['signal']:>3}% (about {approx_dbm(info['signal']):.0f} dBm)  "
            f"channel {chans}  {info['auth']}{radios}")


def report(nets, aps, show_all):
    info = nets.get(GRID_SSID)
    heard = info["radios"] if info else 0

    print("LocalGrid on the air")
    if info:
        print(f"  UP      {describe(GRID_SSID, info)}")
        print(f"          admin page on whichever AP you join: {ADMIN_URL}")
    else:
        print(f"  MISSING {GRID_SSID}")
    old = sorted(n for n in nets if n.startswith("LG-"))
    for name in old:
        print(f"  OLD     {describe(name, nets[name])}  <- firmware from before D46 (one SSID)")

    if show_all:
        print("\nEverything else in range")
        for name in sorted(n for n in nets if n != GRID_SSID and not n.startswith("LG-")):
            print(f"  {describe(name, nets[name])}")

    print(f"\n{heard} of {aps} expected AP radio(s) heard on \"{GRID_SSID}\", {len(nets)} network(s) in range.")
    if heard < aps:
        print("  Fewer APs than expected. Which one is missing: run `nodes` on an AP's console,")
        print("  or look at a handheld's Status screen, which names each AP.")
    return 0 if heard >= aps else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aps", type=int, default=DEFAULT_APS,
                    help=f"how many APs should answer on the grid SSID (default {DEFAULT_APS})")
    ap.add_argument("--all", action="store_true", help="also list networks that are not LocalGrid's")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="keep scanning for this long and report every change")
    ap.add_argument("--interval", type=float, default=10.0, help="seconds between scans while watching")
    args = ap.parse_args()

    text = netsh()
    if text is None:
        return 2
    if "SSID" not in text:
        print("netsh answered but listed no SSID field. On a localised Windows the labels")
        print("differ from the English ones this parser expects; run `netsh wlan show")
        print("networks mode=bssid` by hand to see the raw output.")
        return 2

    nets = parse(MAC_RE.sub("(address withheld)", text))
    rc = report(nets, args.aps, args.all)

    if args.watch:
        # Watching matters because the symptom being chased is an AP that comes and goes:
        # one scan cannot tell "never there" from "there a moment ago".
        print(f"\nWatching for {args.watch:.0f}s, a scan every {args.interval:.0f}s. Changes only.")
        deadline = time.time() + args.watch
        count = nets[GRID_SSID]["radios"] if GRID_SSID in nets else 0
        while time.time() < deadline:
            time.sleep(args.interval)
            text = netsh()
            if text is None:
                continue
            now = parse(MAC_RE.sub("(address withheld)", text))
            n = now[GRID_SSID]["radios"] if GRID_SSID in now else 0
            if n != count:
                print(f"  {time.strftime('%H:%M:%S')}  {count} -> {n} AP radio(s) on \"{GRID_SSID}\"")
                count = n
        print(f"  done; {count} AP radio(s) heard at the end")
        rc = 0 if count >= args.aps else 1

    return rc


if __name__ == "__main__":
    sys.exit(main())
