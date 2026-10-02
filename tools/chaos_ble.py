#!/usr/bin/env python3
"""Arm, renew, disarm and read a chaos run over BLE, from this laptop (D85).

TEMPORARY. This drives the four opcodes registered in docs/TEMPORARY.md as T5, which are the only
opcodes on the D70 admin link that change anything. They exist so a chaos run can be started once
the access points are on battery and have no serial cable, and they come out before a release --
`tools/check_release.py --release` refuses both a push and a release while they are in the tree.

It joins no Wi-Fi and carries no grid message. It needs the admin password, which is proved the way
the admin link already proves it: an HMAC over a random 32-byte challenge keyed by the stored
password hash, inside a channel sealed with K_link.

    python tools/chaos_ble.py status
    python tools/chaos_ble.py arm 20            # a 20-minute window; renew before it closes
    python tools/chaos_ble.py arm 20 --renew-for 60   # keep renewing for an hour, then stop
    python tools/chaos_ble.py disarm
    python tools/chaos_ble.py journal           # read what the AP did to itself

The window is a dead-man switch: the access point stops by itself when nobody renews it, so losing
this laptop ends a run rather than sustaining one. Arming is refused when the grid has no time,
because the deadline could not then be enforced.
"""
import argparse
import asyncio
import getpass
import os
import pathlib
import sys
import time

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


def keep_awake(on=True):
    """Stop Windows sleeping while a window is being renewed.

    The dead-man window is the safety property of a wireless chaos run: nobody renews it, the run stops
    (D83). That makes a sleeping laptop indistinguishable from a laptop that was taken away, which is
    correct - and it also means an overnight run would end quietly at whenever the machine dozed rather
    than at dawn, with no indication in the morning beyond a journal that stops early. `tools/chaos.py`
    has held this off since its first night run; the BLE path needed the same and did not have it.

    Released on the way out, so a finished run leaves the machine's power settings as it found them.
    """
    if os.name != "nt":
        return
    try:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED if on else ES_CONTINUOUS)
    except Exception as e:
        print(f"Could not ask Windows to stay awake ({e}); a long run may stop early if it sleeps.")


def show(st):
    if not st["available"]:
        print("This access point is running an ordinary build: it has no chaos code to arm.")
        print("Upload a chaos build first (idf.py -DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.chaos\").")
    else:
        state = "ARMED" if st["armed"] else "not armed"
        print(f"Chaos: {state}, window {st['window_s']} s left, "
              f"{st['faults']} of {st['max_faults']} faults used")
    print(f"Journal: {st['journal']} record(s){' (FULL)' if st['journal_full'] else ''}")


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


def show_lora_windows(rows):
    """What the radio carried across each fault, from the snapshots written either side of it.

    The raw records above are the evidence; this is the sentence they add up to. A chaos night on
    battery can always show that the grid came back, and the thing it could not show until the journal
    carried these counters is whether **LoRa** was what carried it. "First" is the firmware's own count
    of frames that arrived over LoRa before Wi-Fi had delivered them, so a rising first across an
    ESP-NOW outage is the second radio doing the grid's work, and a flat one is the grid having quietly
    stopped instead of healing.
    """
    snaps, faults = [], []
    pending = {}
    for i, r in enumerate(rows):
        if r["kind"] in ("LoRa first", "LoRa out", "LoRa in"):
            pending[r["kind"]] = r["detail"]
            if len(pending) == 3:
                snaps.append({"at": i, "grid_time": r["grid_time"], "uptime_s": r["uptime_s"],
                              "first": pending["LoRa first"], "out": pending["LoRa out"],
                              "in": pending["LoRa in"]})
                pending = {}
        else:
            pending = {}
            if r["kind"] in ("backbone off", "LoRa off", "restart"):
                faults.append({"at": i, "kind": r["kind"], "detail": r["detail"],
                               "uptime_s": r["uptime_s"]})
    if not snaps:
        print("\nNo LoRa counters in this journal: either the module is not fitted on this access "
              "point, or the image predates them.")
        return
    if not faults:
        print(f"\n{len(snaps)} LoRa snapshot(s) and no fault yet, so nothing to difference across.")
        return

    print(f"\n{'FAULT':<13} {'FOR':>5}  {'LORA FIRST':>11} {'OUT':>7} {'IN':>7}   WHAT IT MEANS")
    for f in faults:
        before = max((s for s in snaps if s["at"] < f["at"]), key=lambda s: s["at"], default=None)
        after = min((s for s in snaps if s["at"] > f["at"]), key=lambda s: s["at"], default=None)
        if before is None or after is None:
            print(f"{f['kind']:<13} {f['detail']:>5}  {'':>11} {'':>7} {'':>7}   "
                  "no snapshot on both sides (a restart loses the one after it)")
            continue
        d_first = after["first"] - before["first"]
        d_out = after["out"] - before["out"]
        d_in = after["in"] - before["in"]
        if f["kind"] == "backbone off":
            meaning = ("LoRa carried the grid while ESP-NOW was down" if d_first > 0
                       else "nothing crossed by LoRa: the grid stopped rather than healed")
        elif f["kind"] == "LoRa off":
            meaning = ("the radio still moved frames with LoRa off, which should not happen"
                       if d_out > 0 else "quiet, as it should be with the radio off")
        else:
            meaning = "a restart: the counters were zeroed, so these are since it came back"
        print(f"{f['kind']:<13} {f['detail']:>5}  {d_first:>11} {d_out:>7} {d_in:>7}   {meaning}")


async def run_all(args, key, auth):
    """`--ap all`: the same command against every access point, and one renewal loop for all of them."""
    aps = await find_all_aps(gw.parse_company_id(), key)
    minutes = args.minutes

    async def do_status(s):
        return gw.decode_chaos_status(await s.ask(gw.OP_CHAOS_GET, gw.OP_CHAOS, b""))

    async def do_arm(s):
        return gw.decode_chaos_status(
            await s.ask(gw.OP_CHAOS_ARM, gw.OP_CHAOS, int(minutes).to_bytes(2, "little")))

    async def do_disarm(s):
        return gw.decode_chaos_status(await s.ask(gw.OP_CHAOS_DISARM, gw.OP_CHAOS, b""))

    names = gw.bench_ap_names()

    async def each(what, fn):
        """Run one command against every access point, one at a time: a BLE radio does one link at once.

        A failure on one is reported and the rest still happen. Arming two of three is a worse night
        than arming three, and far better than arming none because the first was asleep.
        """
        out = {}
        for ap, device in aps:
            who = f"{names.get(ap, 'AP ' + str(ap))} (AP {ap})"
            try:
                out[ap] = await talk(device, key, auth, fn, ap)
                print(f"  {who}: {what} ok")
            except Exception as e:
                print(f"  {who}: {what} FAILED ({type(e).__name__}: {e})")
        return out

    if args.command in ("status", "disarm", "journal"):
        if args.command == "journal":
            print("Reading a journal from every access point is one command per access point; run\n"
                  "`journal --ap <name>` for each, so each journal is printed under its own heading.")
            return 1
        states = await each(args.command, do_status if args.command == "status" else do_disarm)
        for ap in sorted(states):
            print(f"\n== {names.get(ap, 'AP ' + str(ap))} (AP {ap})")
            show(states[ap])
        return 0

    states = await each("arm", do_arm)
    armed = [ap for ap, st in states.items() if st["armed"]]
    for ap in sorted(states):
        print(f"\n== {names.get(ap, 'AP ' + str(ap))} (AP {ap})")
        show(states[ap])
    if not armed:
        print("\nNothing armed. The usual reason is that the grid has no time set.")
        return 1
    print(f"\n{len(armed)} of {len(aps)} access point(s) armed. Their schedules are independent: D82 "
          "seeds each from its own AP index, so they never take themselves out at the same moment.")
    if not args.renew_for:
        print(f"Each window closes in about {minutes} minute(s) unless it is renewed.")
        return 0

    deadline = time.time() + args.renew_for * 60
    every = max(30, (minutes * 60) // 2)
    keep_awake(True)
    print(f"Renewing all of them every {every} s for {args.renew_for} minute(s). Stop this and every "
          f"access point disarms itself within {minutes} minute(s).")
    try:
        while time.time() < deadline:
            await asyncio.sleep(every)
            print(f"-- renewing at {time.strftime('%H:%M:%S')}")
            states = await each("renew", do_arm)
            spent = [ap for ap, st in states.items() if st["faults"] >= st["max_faults"]]
            for ap in sorted(states):
                st = states[ap]
                print(f"     {names.get(ap, 'AP ' + str(ap))}: window {st['window_s']} s, faults "
                      f"{st['faults']}/{st['max_faults']}, journal {st['journal']}"
                      + (" (FULL)" if st["journal_full"] else ""))
            if states and len(spent) == len(states):
                print("  every access point has spent its fault budget; the run is over.")
                break
    except KeyboardInterrupt:
        print("\nStopped renewing. Every access point disarms itself when its window closes.")
    finally:
        keep_awake(False)
    return 0


async def main_async(args):
    auth = gw.AdminAuth()
    auth.set_password(args.password or getpass.getpass("Admin password: "))

    backbone_key, _discriminator = gw.parse_secrets(args.secrets)
    key = gw.link_key(gw.derive_key(backbone_key))
    if (args.ap or "").strip().lower() == "all":
        return await run_all(args, key, auth)
    device, which_ap = await find_ap(args.ap, gw.parse_company_id(), key)

    async def do_status(s):
        return gw.decode_chaos_status(await s.ask(gw.OP_CHAOS_GET, gw.OP_CHAOS, b""))

    if args.command == "status":
        show(await talk(device, key, auth, do_status, which_ap))
        return 0

    if args.command == "disarm":
        async def do(s):
            return gw.decode_chaos_status(await s.ask(gw.OP_CHAOS_DISARM, gw.OP_CHAOS, b""))
        show(await talk(device, key, auth, do, which_ap))
        return 0

    if args.command == "journal":
        async def do(s):
            rows, start = [], 0
            while True:
                body = await s.ask(gw.OP_CHAOS_LOG, gw.OP_CHAOS_LOG_DATA,
                                   start.to_bytes(4, "little"))
                batch = gw.decode_chaos_log(body)
                if not batch:
                    return rows
                rows += batch
                start += len(batch)
        rows = await talk(device, key, auth, do, which_ap)
        if not rows:
            print("The journal is empty: no chaos run has written to this access point.")
            return 0
        print(f"{'GRID TIME':>10} {'UP_S':>7} {'BOOT':>5} {'AP':>3}  {'EVENT':<13} {'DETAIL':>7} {'HEAP_KB':>7}")
        for r in rows:
            print(f"{r['grid_time']:>10} {r['uptime_s']:>7} {r['boot']:>5} {r['ap']:>3}  "
                  f"{r['kind']:<13} {r['detail']:>7} {r['heap_kb']:>7}")
        show_lora_windows(rows)
        return 0

    # arm, and optionally keep renewing so the window never closes under a long run
    minutes = args.minutes

    async def do_arm(s):
        return gw.decode_chaos_status(
            await s.ask(gw.OP_CHAOS_ARM, gw.OP_CHAOS, int(minutes).to_bytes(2, "little")))

    st = await talk(device, key, auth, do_arm, which_ap)
    show(st)
    if not st["armed"]:
        print("Not armed. The usual reason is that the grid has no time set, so the access point\n"
              "could not promise to stop; set the time and try again.")
        return 1
    if not args.renew_for:
        print(f"\nThe window closes in about {minutes} minute(s) unless it is renewed.\n"
              f"Renew with: python {pathlib.Path(__file__).name} arm {minutes}")
        return 0

    deadline = time.time() + args.renew_for * 60
    every = max(30, (minutes * 60) // 2)
    keep_awake(True)
    print(f"\nRenewing every {every} s for {args.renew_for} minute(s); stop this and the run stops "
          f"by itself within {minutes} minute(s). Windows is asked to stay awake until then, because a "
          f"laptop that sleeps looks exactly like a laptop that was taken away.")
    failures = 0
    try:
        while time.time() < deadline:
            await asyncio.sleep(every)
            try:
                st = await talk(device, key, auth, do_arm, which_ap)
            except Exception as e:
                # One missed renewal is survivable: the window is minutes wide. Say so, keep trying, and
                # make it loud if they keep failing, because that is a run about to end without a reason.
                failures += 1
                left = minutes - (failures * every) // 60
                print(f"  renewal failed ({type(e).__name__}: {e}); {failures} in a row, the window "
                      f"closes in roughly {max(0, left)} min if this continues")
                continue
            failures = 0
            print(f"  renewed: window {st['window_s']} s, faults {st['faults']}/{st['max_faults']}, "
                  f"journal {st['journal']}")
            if st["faults"] >= st["max_faults"]:
                print("  the fault budget is spent; the run is over.")
                break
    except KeyboardInterrupt:
        print("\nStopped renewing. The access point disarms itself when the window closes.")
    finally:
        keep_awake(False)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "arm", "disarm", "journal"])
    ap.add_argument("minutes", nargs="?", type=int, default=15,
                    help="window length for 'arm', in minutes (default 15)")
    ap.add_argument("--ap", metavar="WHICH",
                    help="which access point: an AP index (0, 1, 2), a name from "
                         "tools/bench_devices.json, or \"all\" for every one heard (a whole-grid "
                         "night); default the strongest heard")
    ap.add_argument("--secrets", default=gw.DEFAULT_SECRETS,
                    help="the grid's lg_secrets.h (default firmware/common/lg_secrets.h)")
    ap.add_argument("--password", help="admin password (prompted for when not given)")
    ap.add_argument("--renew-for", type=int, metavar="MINUTES",
                    help="keep renewing the window for this long, then let it close")
    args = ap.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
