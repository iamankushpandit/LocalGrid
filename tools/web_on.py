#!/usr/bin/env python3
"""Ask an access point, over BLE, to serve its admin page for a while (D91).

An AP does not run its web server all the time: resident, it cost about 14 KB of an AP's memory to
wait for a visitor who came a few times a week. It runs for a window when asked - `web on` at the
console, or this, for an AP with no cable. This is the only request on the BLE admin link that changes
anything, and all it changes is whether the page is being served; the page still needs the grid's
Wi-Fi passphrase to reach and the admin password to log in to.

    python tools/web_on.py                  # the strongest AP heard, for its default 15 minutes
    python tools/web_on.py 30 --ap Fuji     # one AP by name or index, for 30 minutes
    python tools/web_on.py --ap all         # every AP heard

It joins no Wi-Fi and carries no grid message. It needs the admin password, proved the way the link
already proves it: an HMAC over a random challenge keyed by the stored password hash, inside a channel
sealed with the link key.

This file was `chaos_ble.py`, which also armed chaos runs and asked for faults over BLE (D85, D90).
Those were taken out by D103; what is left is the part that was never temporary.
"""
import argparse
import asyncio
import getpass
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import grid_watch as gw  # noqa: E402


async def talk(device, key, auth, do, expect_ap=None):
    """One short BLE connection: connect, log in, check which AP answered, run `do`, disconnect."""
    from bleak import BleakClient

    async with BleakClient(device, timeout=15.0) as client:
        req = reply = None
        services = getattr(client, "services", None)
        if not services and hasattr(client, "get_services"):
            services = await client.get_services()
        for service in (services or []):
            if not str(service.uuid).replace("-", "").lower().startswith(gw.LINK_SERVICE_PREFIX):
                continue
            for ch in service.characteristics:
                u = str(ch.uuid).replace("-", "").lower()
                if u.startswith(gw.LINK_REQUEST_PREFIX):
                    req = ch
                elif u.startswith(gw.LINK_REPLY_PREFIX):
                    reply = ch
        if req is None or reply is None:
            raise gw.LinkError("this access point has no admin link service")

        async def send(payload):
            await client.write_gatt_char(req, payload, response=False)

        session = gw.LinkSession(key, send)
        mtu = getattr(client, "mtu_size", 0) or 0
        if mtu:
            session.chunk = max(16, min(gw.LINK_CHUNK_BODY, mtu - 3 - gw.LINK_HEADER - gw.LINK_TAG))
        await client.start_notify(reply, lambda _c, d: session.feed(d))
        try:
            await session.wait_open()
            await session.say_hello()
            await session.log_in(auth)
            got = (session.hello or {}).get("ap")
            if expect_ap is not None and got is not None and got != expect_ap:
                raise gw.LinkError(
                    f"this is AP {got}, not AP {expect_ap}: the advert and the sealed link "
                    "disagree about which access point this address is, so nothing was run.")
            return await do(session)
        finally:
            try:
                await client.stop_notify(reply)
            except Exception:
                pass


async def find_ap(name_hint, company_id, key):
    """Find one access point, by AP index or bench name, and say which one it is.

    Not by the admin link's service UUID: that is a GATT service, discoverable only after
    connecting, and it is not in the advertisement. Not by an advertised BLE name either - an AP
    advertises none, so a name hint matched nothing and the loudest AP was armed instead, which is
    the wrong way to arm one AP and watch it.

    The AP index is in the discovery payload (byte 7), and in the header of the sealed status frame
    when that is what arrives. Scanning uses a detection callback rather than discover(), because
    the status frame rides in the scan response and only the callback reliably hands both over.
    Addresses are used only to open the connection: never printed, never logged (D21).
    """
    from bleak import BleakScanner

    names = gw.bench_ap_names()
    seen = {}            # AP index (or address, when unidentified) -> [device, rssi]

    def on_detect(device, adv):
        payloads = gw.manufacturer_payloads(adv, company_id)
        if not payloads:
            return
        rssi = adv.rssi if adv.rssi is not None else -100
        ident = None
        for payload in payloads:
            disc = gw.parse_discovery(payload)
            if disc:
                ident = disc["ap"]
                break
            if payload and payload[0] == gw.ST_MAGIC:
                opened, _why = gw.open_frame(key, payload)
                if opened:
                    ident = opened[0]
                    break
        who = ident if ident is not None else device.address
        if who not in seen or rssi > seen[who][1]:
            seen[who] = [device, rssi]

    print("Scanning for an access point...", flush=True)
    scanner = BleakScanner(detection_callback=on_detect, scanning_mode="active")
    await scanner.start()
    try:
        await asyncio.sleep(8.0)
    finally:
        await scanner.stop()

    if not seen:
        raise gw.LinkError(
            "no LocalGrid access point answered the scan. Check this laptop's Bluetooth is on and "
            "that an AP is in range; the beacon goes out every 500 ms.")

    def label(who):
        if isinstance(who, int):
            return f"{names.get(who, 'AP ' + str(who))} (AP {who})"
        return "an access point that said no index"

    heard = ", ".join(f"{label(w)} at {r} dBm"
                      for w, (_d, r) in sorted(seen.items(), key=lambda kv: -kv[1][1]))
    if name_hint:
        hint = name_hint.strip()
        wanted = [w for w in seen if isinstance(w, int)
                  and (w == int(hint) if hint.isdigit() else hint.lower() in names.get(w, "").lower())]
        if not wanted:
            raise gw.LinkError(f"no access point matched {name_hint!r}. Heard: {heard}.\n"
                               "Give an AP index (0, 1, 2) or a name from tools/bench_devices.json.")
        if len(wanted) > 1:
            raise gw.LinkError(f"{name_hint!r} matched more than one access point: "
                               + ", ".join(label(w) for w in wanted))
        who = wanted[0]
    else:
        who = max(seen, key=lambda w: seen[w][1])
        if len(seen) > 1:
            print(f"Heard {heard}.")
            print("No --ap given, so taking the strongest. Name one with --ap to be sure which "
                  "access point you are arming.")
    device, rssi = seen[who]
    print(f"Using {label(who)} at {rssi} dBm (heard {len(seen)} access point(s))")
    return device, (who if isinstance(who, int) else None)


async def find_all_aps(company_id, key):
    """Every access point this laptop can hear, by AP index. For a whole-grid night.

    Arming one access point tests one access point. A night that is meant to say something about the
    *grid* has to have every one of them attacking itself, and doing that an AP at a time meant one
    terminal and one renewal loop each - three chances to forget one, and three windows closing at
    different times. The schedules stay independent whatever this does: D82 seeds each one from its own
    AP index, so they never take themselves out at the same moment.
    """
    from bleak import BleakScanner

    names = gw.bench_ap_names()
    seen = {}

    def on_detect(device, adv):
        payloads = gw.manufacturer_payloads(adv, company_id)
        rssi = adv.rssi if adv.rssi is not None else -100
        for payload in payloads:
            disc = gw.parse_discovery(payload)
            ap = disc["ap"] if disc else None
            if ap is None and payload and payload[0] == gw.ST_MAGIC:
                opened, _why = gw.open_frame(key, payload)
                ap = opened[0] if opened else None
            if ap is None:
                continue
            if ap not in seen or rssi > seen[ap][1]:
                seen[ap] = (device, rssi)
            return

    print("Scanning for every access point...", flush=True)
    scanner = BleakScanner(detection_callback=on_detect, scanning_mode="active")
    await scanner.start()
    try:
        await asyncio.sleep(10.0)        # longer than a single-AP scan: every one has to be heard
    finally:
        await scanner.stop()
    if not seen:
        raise gw.LinkError("no LocalGrid access point answered the scan.")
    for ap in sorted(seen):
        print(f"  {names.get(ap, 'AP ' + str(ap))} (AP {ap}) at {seen[ap][1]} dBm")
    return [(ap, seen[ap][0]) for ap in sorted(seen)]


async def main_async(args):
    auth = gw.AdminAuth()
    auth.set_password(args.password or os.environ.get("LG_ADMIN_PASSWORD") or getpass.getpass("Admin password: "))
    backbone_key, _discriminator = gw.parse_secrets(args.secrets)
    key = gw.link_key(gw.derive_key(backbone_key))
    names = gw.bench_ap_names()

    async def do_web(s):
        body = await s.ask(gw.OP_WEB, gw.OP_WEB_OK, int(args.minutes).to_bytes(2, "little"))
        return int.from_bytes(body[:2], "little")

    if (args.ap or "").strip().lower() == "all":
        aps = await find_all_aps(gw.parse_company_id(), key)
        failed = 0
        for ap, device in aps:   # one at a time: a BLE radio does one link at once
            who = f"{names.get(ap, 'AP ' + str(ap))} (AP {ap})"
            try:
                left = await talk(device, key, auth, do_web, ap)
                print(f"  {who}: admin page on for {left} s")
            except Exception as e:
                failed += 1
                print(f"  {who}: FAILED ({type(e).__name__}: {e})")
        return 1 if failed or not aps else 0
    device, which_ap = await find_ap(args.ap, gw.parse_company_id(), key)
    left = await talk(device, key, auth, do_web, which_ap)
    print(f"AP {which_ap}: admin page on at http://192.168.4.1/ for {left} s, then it stops itself")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("minutes", nargs="?", type=int, default=0,
                    help="how long to serve the page, in minutes (default: the AP's own, 15; at most 120)")
    ap.add_argument("--ap", metavar="WHICH",
                    help="which access point: an AP index (0, 1, 2), a name from tools/bench_devices.json, "
                         "or \"all\" for every one heard; default the strongest heard")
    ap.add_argument("--secrets", default=gw.DEFAULT_SECRETS,
                    help="the grid's lg_secrets.h (default firmware/common/lg_secrets.h)")
    ap.add_argument("--password", help="admin password (or LG_ADMIN_PASSWORD; prompted for when neither is given)")
    args = ap.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
