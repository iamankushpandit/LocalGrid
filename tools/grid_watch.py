#!/usr/bin/env python3
"""Watch a LocalGrid offline network over BLE, without joining its Wi-Fi (decision D68).

Every AP advertises its discovery payload over BLE and, since D68, puts a sealed status frame in
its scan response (docs/ble-status.md): AP health, which handhelds are online and where, the
newest urgent alert, handheld batteries, and chosen names. This tool listens for both, checks
each status frame against this grid's key, and serves a small dashboard on this laptop.

Decision D25 stands: the laptop is an observer, never a participant. It never joins the grid's
Wi-Fi, never carries a message, and never speaks the LocalGrid protocol. Being exact about the
radio: Windows scans actively so that scan responses arrive, which means the laptop sends BLE
scan requests. Those are generic Bluetooth, not grid traffic, and nothing the grid acts on.

Decision D21 stands too: BLE addresses are hardware addresses, so they are never printed,
logged, or served. APs are keyed by the AP index they announce.

The status is sealed with a key derived from LG_SECRET_BACKBONE_KEY, so the laptop needs this
grid's firmware/common/lg_secrets.h (or --secrets). Without it the APs are still listed from
their discovery adverts, but no status can be read and none can be forged.

  python tools/grid_watch.py                 listen and open the dashboard in the browser
  python tools/grid_watch.py --no-browser    listen; open http://127.0.0.1:8768/ yourself
  python tools/grid_watch.py --port 9000     serve on another local port
  python tools/grid_watch.py --log run.jsonl also append every event to a JSON-lines file
  python tools/grid_watch.py --seconds 20    listen for 20 s, then print which APs were heard
  python tools/grid_watch.py --self-check    offline: seal frames per the spec and decode them

The dashboard is bound to 127.0.0.1 only and needs no Internet: one page, no CDN.
Exit with Ctrl+C. Exit code 0 on a clean stop or a passing self-check, 1 on a failing
self-check, 2 when the secrets, the port, or the Bluetooth radio are unusable.
"""
import argparse
import asyncio
import collections
import hmac
import json
import os
import re
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SECRETS = os.path.join(ROOT, "firmware", "common", "lg_secrets.h")
PROTO_CONFIG = os.path.join(ROOT, "firmware", "common", "lg_proto_config.h")
BENCH_DEVICES = os.path.join(ROOT, "tools", "bench_devices.json")
DEFAULT_PORT = 8768

# Discovery payload (lg_proto_config.h): 'L' 'G', version, discriminator[4], AP index,
# reserved, free station slots, flags, attached handhelds. BLE carries the fixed part only;
# an optional Wi-Fi-style tail (u8 length + name) is read if an AP ever sends one.
DISC_MAGIC = b"LG"
DISC_LEN = 12
DISC_FLAG_BACKBONE = 0x01
DISC_FLAG_TIME = 0x02
MAX_APS = 8                      # LG_MAX_NODES

# Status frame (docs/ble-status.md).
ST_MAGIC = 0x53
ST_HEADER = 9
ST_TAG = 4
ST_MAX = 27
ST_MAX_CT = 14
KDF_SALT = b"LG-BLE-STATUS-1"
KDF_INFO = b"ble status"
T_HEALTH, T_HANDHELDS, T_ALERT, T_BATTERY, T_NAME = 1, 2, 3, 4, 5
TYPE_LEN = {T_HEALTH: (10, 10), T_HANDHELDS: (14, 14), T_ALERT: (10, 10),
            T_BATTERY: (0, 14), T_NAME: (1, 14)}

AP_SILENT_S = 10.0               # an AP not heard for this long is "not heard"
FRAME_FRESH_S = 15.0             # a handhelds or alert frame older than this no longer counts
HANDHELD_SLOTS = 20              # the handhelds frame maps devices 1..20 to an AP

TIME_QUALITY = {0: "unset", 1: "carried", 2: "authoritative", 3: "unknown"}
# esp_reset_reason_t, worded as components/lg_power does.
RESET_WORDS = {0: "unknown", 1: "power-on or reset", 2: "external reset", 3: "software restart",
               4: "crash (panic)", 5: "crash (interrupt watchdog)", 6: "crash (task watchdog)",
               7: "crash (other watchdog)", 8: "wake from deep sleep",
               9: "low supply voltage (brownout)", 10: "SDIO reset", 11: "USB reset",
               12: "JTAG reset", 13: "eFuse error", 14: "power glitch", 15: "CPU lock-up"}


# ---------------------------------------------------------------- configuration from the repo

def parse_secrets(path):
    """Returns (backbone_key 32 bytes, discriminator 4 bytes) from an lg_secrets.h."""
    with open(path, encoding="utf-8") as f:
        text = f.read()

    def array(name):
        m = re.search(r"#define\s+" + name + r"\s*\{([^}]*)\}", text)
        if not m:
            raise ValueError(f"{name} not found in {path}")
        items = [s.strip() for s in m.group(1).split(",") if s.strip()]
        return bytes(int(s, 0) for s in items)

    key = array("LG_SECRET_BACKBONE_KEY")
    disc = array("LG_SECRET_DISCRIMINATOR")
    if len(key) != 32 or not any(key):
        raise ValueError(f"LG_SECRET_BACKBONE_KEY in {path} is not a generated 32-byte key "
                         "(run python tools/gen_secrets.py)")
    if len(disc) != 4:
        raise ValueError(f"LG_SECRET_DISCRIMINATOR in {path} is not 4 bytes")
    return key, disc


def parse_company_id(path=PROTO_CONFIG):
    with open(path, encoding="utf-8") as f:
        m = re.search(r"#define\s+LG_BLE_COMPANY_ID\s+(0x[0-9A-Fa-f]+|\d+)", f.read())
    if not m:
        raise ValueError(f"LG_BLE_COMPANY_ID not found in {path}")
    return int(m.group(1), 0)


def bench_ap_names(path=BENCH_DEVICES):
    """AP index -> name from the bench registry (names only; the file holds no addresses)."""
    try:
        with open(path, encoding="utf-8") as f:
            devices = json.load(f).get("devices", [])
    except (OSError, ValueError):
        return {}
    return {d["node_index"]: d["node_name"] for d in devices
            if d.get("role") == "N" and isinstance(d.get("node_index"), int) and d.get("node_name")}


# ---------------------------------------------------------------- the sealed status frame

def derive_key(backbone_key):
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=KDF_SALT,
                info=KDF_INFO).derive(backbone_key)


def make_nonce(ap, boot, counter):
    return (bytes([ap, 0]) + boot.to_bytes(4, "little") + counter.to_bytes(3, "little")
            + b"\x00\x00\x00")


def open_frame(k, frame):
    """Checks and decrypts one status frame.

    Returns (ap, type, boot, counter, plaintext) or (None, reason). The AEAD's own decrypt
    cannot check a 4-byte tag, so: decrypt with raw ChaCha20 at block counter 1 (the AEAD's
    payload keystream, RFC 8439 2.8), seal the recovered plaintext again with the same nonce
    and AAD, and compare the first 4 bytes of that tag in constant time.
    """
    if len(frame) < ST_HEADER + ST_TAG or len(frame) > ST_MAX:
        return None, "length"
    if frame[0] != ST_MAGIC:
        return None, "magic"
    ap = frame[1] & 0x0F
    ftype = frame[1] >> 4
    boot = int.from_bytes(frame[2:6], "little")
    counter = int.from_bytes(frame[6:9], "little")
    ct = frame[ST_HEADER:-ST_TAG]
    tag = frame[-ST_TAG:]
    if len(ct) > ST_MAX_CT:
        return None, "length"
    nonce = make_nonce(ap, boot, counter)
    aad = frame[:ST_HEADER]
    dec = Cipher(algorithms.ChaCha20(k, (1).to_bytes(4, "little") + nonce), mode=None).decryptor()
    pt = dec.update(ct) + dec.finalize()
    sealed = ChaCha20Poly1305(k).encrypt(nonce, pt, aad)
    if not hmac.compare_digest(sealed[len(pt):len(pt) + ST_TAG], tag):
        return None, "tag"
    return (ap, ftype, boot, counter, pt), None


def seal_frame(k, ap, ftype, boot, counter, pt):
    """The sender's side, as the AP does it. Used by --self-check only."""
    header = (bytes([ST_MAGIC, (ftype << 4) | (ap & 0x0F)]) + boot.to_bytes(4, "little")
              + counter.to_bytes(3, "little"))
    sealed = ChaCha20Poly1305(k).encrypt(make_nonce(ap, boot, counter), pt, header)
    return header + sealed[:len(pt)] + sealed[len(pt):len(pt) + ST_TAG]


def decode_body(ftype, pt):
    """Plaintext -> dict, or None if the length is wrong for the type."""
    lo, hi = TYPE_LEN.get(ftype, (None, None))
    if lo is None or not lo <= len(pt) <= hi:
        return None
    if ftype == T_HEALTH:
        t, g = pt[4], pt[9]
        return {"uptime_min": int.from_bytes(pt[0:2], "little"), "links": pt[2],
                "handhelds_here": pt[3], "time_quality": t & 0x03, "time_gps": bool(t & 0x04),
                "stratum": t >> 3, "heap_kb": pt[5], "reset_reason": pt[6], "restarts": pt[7],
                "brownouts": pt[8], "gps_fitted": bool(g & 1), "gps_fix": bool(g & 2),
                "gps_sats": g >> 2}
    if ftype == T_HANDHELDS:
        online = int.from_bytes(pt[0:4], "little")
        where = {}
        for i in range(HANDHELD_SLOTS):
            nib = (pt[4 + i // 2] >> (4 * (i % 2))) & 0x0F
            where[i + 1] = None if nib == 0x0F else nib
        return {"online": {d for d in range(1, 33) if online & (1 << (d - 1))}, "where": where}
    if ftype == T_ALERT:
        return {"active": bool(pt[0] & 1), "all_clear": bool(pt[0] & 2), "author": pt[1],
                "age_s": int.from_bytes(pt[2:4], "little"), "reads": pt[4],
                "grid_time": int.from_bytes(pt[6:10], "little")}
    if ftype == T_BATTERY:
        if len(pt) % 2:
            return None
        return {"batteries": {pt[i]: pt[i + 1] for i in range(0, len(pt), 2) if pt[i]}}
    if ftype == T_NAME:
        return {"device": pt[0], "name": pt[1:].decode("utf-8", errors="replace")}
    return None


def parse_discovery(p):
    if len(p) < DISC_LEN or p[:2] != DISC_MAGIC:
        return None
    name = None
    if len(p) > DISC_LEN:
        n = p[DISC_LEN]
        if 0 < n <= len(p) - DISC_LEN - 1:
            name = p[DISC_LEN + 1:DISC_LEN + 1 + n].decode("utf-8", errors="replace")
    return {"version": p[2], "disc": bytes(p[3:7]), "ap": p[7], "free_slots": p[9],
            "flags": p[10], "attached": p[11], "name": name}


# ---------------------------------------------------------------- grid state

def minutes_text(m):
    if m >= 0xFFFF:
        return "45 d or more"
    d, h, mi = m // 1440, (m % 1440) // 60, m % 60
    return f"{d} d {h} h" if d else (f"{h} h {mi} min" if h else f"{mi} min")


def time_text(q, gps, stratum):
    s = TIME_QUALITY.get(q, "?")
    if q == 0:
        return "no grid time"
    if gps:
        s += ", from GPS"
    s += ", stratum unknown" if stratum >= 31 else f", stratum {stratum}"
    return s


class GridState:
    """Everything heard, keyed by AP index and device number. Never holds a BLE address."""

    def __init__(self, k, discriminator, ap_names=None, log_path=None, echo=True):
        self.k = k
        self.disc = discriminator
        self.names_ap = dict(ap_names or {})
        self.lock = threading.Lock()
        self.aps = {}
        self.hh_names = {}               # device -> (name, heard at)
        self.batteries = {}              # device -> (percent, AP, heard at)
        self.alert_seq = 0
        self.alert_key = None            # (author, carried at, all clear) of the newest seen
        self.grid_offset = None          # grid time minus this laptop's clock
        self.events = collections.deque(maxlen=200)
        self.stats = collections.Counter()
        self.log_path = log_path
        self.echo = echo
        self.started = time.time()

    # -- events

    def event(self, now, kind, text, **extra):
        e = {"t": now, "kind": kind, "text": text}
        e.update(extra)
        self.events.append(e)
        if self.echo:
            print(f"{time.strftime('%H:%M:%S', time.localtime(now))}  {text}", flush=True)
        if self.log_path:
            try:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(e, ensure_ascii=False) + "\n")
            except OSError:
                pass

    def ap_name(self, ap):
        a = self.aps.get(ap)
        if a and a.get("adv_name"):
            return a["adv_name"]
        return self.names_ap.get(ap, f"AP {ap}")

    def hh_name(self, dev):
        n = self.hh_names.get(dev)
        return n[0] if n else f"Handheld {dev}"

    def _ap(self, ap, now):
        a = self.aps.get(ap)
        if a is None:
            a = {"index": ap, "first": now, "last": 0.0, "rssi": None, "heard": False,
                 "disc": None, "adv_name": None, "status_last": None, "boot": None,
                 "counter": -1, "health": None, "handhelds": None, "hh_at": 0.0,
                 "alert": None, "alert_at": 0.0, "ok": 0, "bad_tag": 0, "warned": False, "ever": False}
            self.aps[ap] = a
        return a

    def _heard(self, a, rssi, now):
        a["last"] = now
        if rssi is not None:
            a["rssi"] = rssi
        if not a["heard"]:
            a["heard"] = True
            again = a["ever"]
            a["ever"] = True
            self.event(now, "ap_heard", f"{self.ap_name(a['index'])} (AP {a['index']}) "
                       + ("heard again" if again else "heard"), ap=a["index"])

    # -- input

    def on_advert(self, payloads, rssi, now=None):
        """One BLE event: the manufacturer payloads under the grid's company ID it carried."""
        now = time.time() if now is None else now
        with self.lock:
            discs = [d for d in (parse_discovery(p) for p in payloads) if d]
            ours = [d for d in discs if d["disc"] == self.disc]
            if discs and not ours:
                self.stats["other_grid"] += 1        # another LocalGrid network: not ours
                return
            for d in ours:
                if d["ap"] >= MAX_APS:
                    continue
                a = self._ap(d["ap"], now)
                if d["name"]:
                    a["adv_name"] = d["name"]
                a["disc"] = d
                self._heard(a, rssi, now)
            paired = bool(ours)
            for p in payloads:
                if p and p[0] == ST_MAGIC:
                    self._status(p, rssi, now, paired)

    def _status(self, frame, rssi, now, paired):
        got, why = open_frame(self.k, frame)
        if got is None:
            self.stats["bad_" + why] += 1
            # A frame paired with this grid's discovery advert that fails its check means the
            # key is wrong or the firmware differs; one without a pairing may be anyone's.
            if paired and why == "tag" and len(frame) > 1:
                ap = frame[1] & 0x0F
                a = self.aps.get(ap)
                if a is not None:
                    a["bad_tag"] += 1
                    if a["bad_tag"] >= 5 and a["ok"] == 0 and not a["warned"]:
                        a["warned"] = True
                        self.event(now, "bad_key", f"{self.ap_name(ap)} (AP {ap}) sends status "
                                   "this laptop cannot verify: its lg_secrets.h is not this "
                                   "grid's, or the AP runs a different frame format", ap=ap)
            return
        ap, ftype, boot, counter, pt = got
        if ap >= MAX_APS:
            self.stats["bad_ap"] += 1
            return
        a = self._ap(ap, now)
        # Replay rule: within one boot the counter must rise. A higher boot is a restart; a
        # lower one is an old recording. The same counter again is Windows re-delivering the
        # cached scan response with a fresh advert, so it is dropped without counting.
        if a["boot"] is not None:
            if boot < a["boot"] or (boot == a["boot"] and counter < a["counter"]):
                self.stats["replay"] += 1
                return
            if boot == a["boot"] and counter == a["counter"]:
                self.stats["duplicate"] += 1
                return
        body = decode_body(ftype, pt)
        if body is None:
            self.stats["bad_body"] += 1
            return
        if a["boot"] is not None and boot > a["boot"]:
            self.event(now, "ap_restart", f"{self.ap_name(ap)} (AP {ap}) restarted "
                       f"(boot {a['boot']} -> {boot})", ap=ap)
        a["boot"], a["counter"] = boot, counter
        a["ok"] += 1
        a["status_last"] = now
        self.stats["ok"] += 1
        self._heard(a, rssi, now)

        if ftype == T_HEALTH:
            old = a["health"]
            if old and (old["time_quality"], old["time_gps"]) != (body["time_quality"],
                                                                  body["time_gps"]):
                self.event(now, "time_source", f"{self.ap_name(ap)} (AP {ap}) time: "
                           f"{time_text(old['time_quality'], old['time_gps'], old['stratum'])}"
                           f" -> {time_text(body['time_quality'], body['time_gps'], body['stratum'])}",
                           ap=ap)
            if old and body["brownouts"] != old["brownouts"]:
                self.event(now, "brownout", f"{self.ap_name(ap)} (AP {ap}) brownouts now "
                           f"{body['brownouts']}", ap=ap)
            a["health"] = body
        elif ftype == T_HANDHELDS:
            a["handhelds"], a["hh_at"] = body, now
        elif ftype == T_ALERT:
            a["alert"], a["alert_at"] = body, now
            if body["grid_time"]:
                self.grid_offset = body["grid_time"] - now
            self._alert_changed(now)
        elif ftype == T_BATTERY:
            for dev, pct in body["batteries"].items():
                self.batteries[dev] = (pct, ap, now)
        elif ftype == T_NAME:
            if body["device"]:
                self.hh_names[body["device"]] = (body["name"], now)

    # -- merging

    def _newest_alert(self, now):
        """The newest urgent broadcast any AP heard recently reports, with its read count."""
        best = None
        for a in self.aps.values():
            al = a["alert"]
            if not al or not al["author"] or now - a["alert_at"] > FRAME_FRESH_S:
                continue
            carried = a["alert_at"] - al["age_s"]
            if best is None or carried > best["carried"] + 2:
                best = {"carried": carried, "ap": a["index"], **al}
        if best is None:
            return None
        reads = [a["alert"]["reads"] for a in self.aps.values()
                 if a["alert"] and a["alert"]["author"] == best["author"]
                 and now - a["alert_at"] <= FRAME_FRESH_S and a["alert"]["reads"] != 255
                 and abs(a["alert_at"] - a["alert"]["age_s"] - best["carried"]) <= 5]
        best["reads"] = max(reads) if reads else None
        return best

    def _alert_changed(self, now):
        al = self._newest_alert(now)
        if al is None:
            return
        key = (al["author"], al["all_clear"], al["active"])
        if self.alert_key is not None and key == self.alert_key[0] \
                and abs(al["carried"] - self.alert_key[1]) <= 5:
            return
        self.alert_key = (key, al["carried"])
        who = self.hh_name(al["author"])
        if al["all_clear"]:
            self.event(now, "all_clear", f"ALL CLEAR: {who} is safe", device=al["author"])
        elif al["active"]:
            self.alert_seq += 1
            self.event(now, "alert", f"SOS from {who} near {self._near(al, now)}, "
                       f"{al['age_s'] // 60} min ago", device=al["author"])

    def _near(self, al, now):
        loc = self._locations(now).get(al["author"])
        return self.ap_name(loc if loc is not None else al["ap"])

    def _locations(self, now):
        """device -> AP index, from the newest fresh handhelds frame that places it."""
        best = {}
        for a in self.aps.values():
            hh = a["handhelds"]
            if not hh or now - a["hh_at"] > FRAME_FRESH_S:
                continue
            for dev, where in hh["where"].items():
                if where is not None and dev in hh["online"]:
                    if dev not in best or a["hh_at"] > best[dev][1]:
                        best[dev] = (where, a["hh_at"])
        return {d: v[0] for d, v in best.items()}

    def tick(self, now=None):
        now = time.time() if now is None else now
        with self.lock:
            for a in self.aps.values():
                if a["heard"] and now - a["last"] > AP_SILENT_S:
                    a["heard"] = False
                    self.event(now, "ap_lost", f"{self.ap_name(a['index'])} (AP {a['index']}) "
                               f"not heard for {AP_SILENT_S:.0f} s", ap=a["index"])

    # -- output

    def snapshot(self, now=None):
        now = time.time() if now is None else now
        with self.lock:
            online = set()
            for a in self.aps.values():
                if a["handhelds"] and now - a["hh_at"] <= FRAME_FRESH_S:
                    online |= a["handhelds"]["online"]
            where = self._locations(now)
            devices = sorted(online | set(self.batteries) | set(self.hh_names) | set(where))
            handhelds = []
            for d in devices:
                bat = self.batteries.get(d)
                handhelds.append({
                    "device": d, "name": self.hh_name(d), "online": d in online,
                    "ap": where.get(d), "ap_name": self.ap_name(where[d]) if d in where else None,
                    "battery": None if not bat or bat[0] == 255 else min(bat[0], 100),
                    "battery_age_s": round(now - bat[2]) if bat else None})

            aps = []
            for i in sorted(self.aps):
                a = self.aps[i]
                h = a["health"]
                d = a["disc"]
                item = {"index": i, "name": self.ap_name(i), "heard": a["heard"],
                        "silent_s": round(now - a["last"]) if a["last"] else None,
                        "rssi": a["rssi"],
                        "status_age_s": round(now - a["status_last"]) if a["status_last"] else None,
                        "frames_ok": a["ok"], "bad_tag": a["bad_tag"], "boot": a["boot"],
                        "here": sorted(dev for dev, ap in where.items() if ap == i),
                        "disc": None if not d else {
                            "backbone": bool(d["flags"] & DISC_FLAG_BACKBONE),
                            "time": bool(d["flags"] & DISC_FLAG_TIME),
                            "attached": d["attached"], "free_slots": d["free_slots"]},
                        "health": None}
                if h:
                    item["health"] = {
                        "uptime": minutes_text(h["uptime_min"]),
                        "links": [{"index": n, "name": self.ap_name(n)}
                                  for n in range(MAX_APS) if h["links"] & (1 << n)],
                        "handhelds_here": h["handhelds_here"],
                        "time": time_text(h["time_quality"], h["time_gps"], h["stratum"]),
                        "time_quality": TIME_QUALITY.get(h["time_quality"], "?"),
                        "time_gps": h["time_gps"],
                        "heap_kb": h["heap_kb"], "heap_sat": h["heap_kb"] >= 255,
                        "reset": RESET_WORDS.get(h["reset_reason"], f"reason {h['reset_reason']}"),
                        "restarts": h["restarts"], "brownouts": h["brownouts"],
                        "gps": {"fitted": h["gps_fitted"], "fix": h["gps_fix"],
                                "sats": h["gps_sats"]}}
                item["here_names"] = [self.hh_name(dev) for dev in item["here"]]
                aps.append(item)

            al = self._newest_alert(now)
            alert = None
            if al:
                loc = where.get(al["author"])
                alert = {"id": self.alert_seq, "active": al["active"], "all_clear": al["all_clear"],
                         "device": al["author"], "name": self.hh_name(al["author"]),
                         "near": self.ap_name(loc if loc is not None else al["ap"]),
                         "age_s": max(0, round(now - al["carried"])), "reads": al["reads"]}
            grid_time = round(now + self.grid_offset) if self.grid_offset is not None else None
            return {"now": now, "grid_time": grid_time, "listening_s": round(now - self.started),
                    "aps": aps, "handhelds": handhelds, "alert": alert,
                    "events": list(self.events)[-60:][::-1], "stats": dict(self.stats)}


# ---------------------------------------------------------------- the dashboard

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>LocalGrid grid watch</title>
<style>
  /* Theme tokens from firmware/node/main/web/admin.html (D10). */
  :root {
    --bg: #0b1310; --surface: #111d18; --line: #1f3329; --text: #d7efe0; --muted: #86a596;
    --accent: #5fd38d; --accent-ink: #06120c; --warn: #f0b64a; --danger: #ef6b6b;
    --radius: 10px; --gap: 14px; --font: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    --mono: ui-monospace, "SFMono-Regular", Consolas, monospace;
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 18px 16px 40px; background: var(--bg); color: var(--text); font: 16px/1.5 var(--font); }
  main { max-width: 1100px; margin: 0 auto; display: grid; gap: var(--gap); }
  header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 6px 14px; }
  header h1 { font-size: 22px; margin: 0; letter-spacing: .02em; }
  header .spacer { flex: 1; }
  .sub, .hint { color: var(--muted); font-size: 14px; }
  .mono { font-family: var(--mono); }
  section { background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); padding: 16px; }
  h2 { font-size: 13px; text-transform: uppercase; letter-spacing: .12em; color: var(--muted); margin: 0 0 10px; }
  .banner { border-radius: var(--radius); padding: 14px 16px; font-weight: 600; font-size: 18px; border: 1px solid var(--line); }
  .banner.sos { background: var(--danger); color: #1a0505; border-color: var(--danger); }
  .banner.clear { background: var(--accent); color: var(--accent-ink); border-color: var(--accent); }
  .banner.none { background: var(--surface); color: var(--muted); font-weight: 400; font-size: 15px; }
  .cards { display: grid; gap: var(--gap); grid-template-columns: repeat(auto-fill, minmax(250px, 1fr)); }
  .card { background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); padding: 14px; }
  .card.lost { opacity: .6; border-style: dashed; }
  .card h3 { margin: 0; font-size: 18px; display: flex; align-items: baseline; gap: 8px; }
  .card h3 .idx { color: var(--muted); font-size: 13px; font-weight: 400; }
  .pill { display: inline-block; padding: 1px 8px; border-radius: 999px; font-size: 12px; font-weight: 600; }
  .pill.ok { background: var(--accent); color: var(--accent-ink); }
  .pill.bad { background: var(--danger); color: #1a0505; }
  .pill.warn { background: var(--warn); color: #1c1302; }
  dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 12px; margin: 10px 0 0; font-size: 14px; }
  dt { color: var(--muted); }
  dd { margin: 0; overflow-wrap: anywhere; }
  .gps { color: var(--accent); font-weight: 600; }
  .warntext { color: var(--warn); }
  .dangertext { color: var(--danger); }
  table { width: 100%; border-collapse: collapse; font-size: 15px; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); }
  th { color: var(--muted); font-weight: 500; font-size: 13px; }
  .bar { display: inline-block; width: 90px; height: 10px; border-radius: 5px; background: var(--bg);
         border: 1px solid var(--line); vertical-align: middle; margin-right: 8px; overflow: hidden; }
  .bar i { display: block; height: 100%; background: var(--accent); }
  .bar i.low { background: var(--danger); } .bar i.mid { background: var(--warn); }
  .log { font-family: var(--mono); font-size: 13px; max-height: 260px; overflow-y: auto; }
  .log div { padding: 2px 0; border-bottom: 1px solid var(--line); }
  .log .k-alert, .log .k-ap_lost, .log .k-bad_key { color: var(--danger); }
  .log .k-all_clear, .log .k-ap_heard { color: var(--accent); }
  .log .k-time_source, .log .k-ap_restart, .log .k-brownout { color: var(--warn); }
  label.toggle { font-size: 14px; color: var(--muted); display: inline-flex; align-items: center; gap: 6px; cursor: pointer; }
  .stale { color: var(--danger); }
  @media (max-width: 560px) { .hide-sm { display: none; } }
</style>
</head>
<body>
<main>
  <header>
    <h1>LocalGrid &mdash; grid watch (listening over BLE)</h1>
    <span class="spacer"></span>
    <span class="sub">grid time <span class="mono" id="gt">&ndash;</span></span>
    <label class="toggle"><input type="checkbox" id="beep"> beep on alert</label>
  </header>
  <div class="sub" id="sub">An offline network, watched from this laptop without joining its Wi-Fi.</div>
  <div class="banner none" id="banner">No alert heard.</div>
  <section>
    <h2>APs</h2>
    <div class="cards" id="aps"><div class="hint">Listening&hellip; no AP of this grid heard yet.</div></div>
  </section>
  <section>
    <h2>Handhelds</h2>
    <table><thead><tr><th>Name</th><th>Online</th><th>AP</th><th>Battery</th></tr></thead>
      <tbody id="hh"><tr><td colspan="4" class="hint">None heard yet.</td></tr></tbody></table>
  </section>
  <section>
    <h2>Events</h2>
    <div class="log" id="log"><div class="hint">Nothing yet.</div></div>
    <p class="hint" id="stats"></p>
  </section>
</main>
<script>
"use strict";
const $ = id => document.getElementById(id);
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
function ago(s) { if (s == null) return "never"; if (s < 60) return s + " s ago"; if (s < 3600) return Math.floor(s / 60) + " min ago"; return Math.floor(s / 3600) + " h " + Math.floor(s % 3600 / 60) + " min ago"; }
function row(dl, k, v, cls) { dl.append(el("dt", "", k)); const d = el("dd", cls || ""); if (v instanceof Node) d.append(v); else d.textContent = v; dl.append(d); }

let beepOn = false, audio = null, lastAlertId = null, failures = 0;
try { beepOn = localStorage.getItem("gw-beep") === "1"; } catch (e) {}
$("beep").checked = beepOn;
$("beep").addEventListener("change", e => {
  beepOn = e.target.checked;
  try { localStorage.setItem("gw-beep", beepOn ? "1" : "0"); } catch (err) {}
  if (beepOn) { ensureAudio(); beep(); }
});
function ensureAudio() { if (!audio) { try { audio = new (window.AudioContext || window.webkitAudioContext)(); } catch (e) {} } }
function beep() {
  ensureAudio(); if (!audio) return;
  const t = audio.currentTime;
  for (let i = 0; i < 3; i++) {
    const o = audio.createOscillator(), g = audio.createGain();
    o.frequency.value = 880; o.connect(g); g.connect(audio.destination);
    g.gain.setValueAtTime(0.0001, t + i * 0.35); g.gain.exponentialRampToValueAtTime(0.3, t + i * 0.35 + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, t + i * 0.35 + 0.25);
    o.start(t + i * 0.35); o.stop(t + i * 0.35 + 0.3);
  }
}

function renderBanner(a) {
  const b = $("banner");
  if (!a) { b.className = "banner none"; b.textContent = "No alert heard."; return; }
  const reads = a.reads == null ? "" : ", read by " + a.reads;
  const mins = Math.floor(a.age_s / 60);
  if (a.all_clear) { b.className = "banner clear"; b.textContent = "All clear: " + a.name + " is safe (" + ago(a.age_s) + ")"; }
  else if (a.active) { b.className = "banner sos"; b.textContent = "SOS from " + a.name + " near " + a.near + ", " + mins + " min ago" + reads; }
  else { b.className = "banner none"; b.textContent = "Last SOS from " + a.name + " near " + a.near + ", " + mins + " min ago (no longer active)" + reads; }
  if (a.active && !a.all_clear && lastAlertId !== null && a.id !== lastAlertId && beepOn) beep();
  if (a.active && !a.all_clear && lastAlertId === null && beepOn) beep();
  lastAlertId = a.id;
}

function renderAps(aps) {
  const box = $("aps"); box.replaceChildren();
  if (!aps.length) { box.append(el("div", "hint", "Listening… no AP of this grid heard yet.")); return; }
  for (const a of aps) {
    const c = el("div", "card" + (a.heard ? "" : " lost"));
    const h = el("h3"); h.append(el("span", "", a.name), el("span", "idx", "AP " + a.index));
    c.append(h);
    const st = el("div");
    if (a.heard) st.append(el("span", "pill ok", "heard"), document.createTextNode(a.rssi != null ? "  " + a.rssi + " dBm" : ""));
    else st.append(el("span", "pill bad", "not heard"), document.createTextNode("  for " + ago(a.silent_s).replace(" ago", "")));
    c.append(st);
    const dl = el("dl");
    const hh = a.health;
    if (hh) {
      row(dl, "Uptime", hh.uptime);
      row(dl, "Links", hh.links.length ? hh.links.map(l => l.name).join(", ") : "none", hh.links.length ? "" : "warntext");
      const here = a.here_names.length ? a.here_names.join(", ") : (hh.handhelds_here ? hh.handhelds_here + " registered" : "none");
      row(dl, "Handhelds", here);
      const t = el("span", hh.time_quality === "unset" ? "warntext" : "", hh.time);
      if (hh.time_gps) { const g = el("span", "gps", " ⌖ GPS"); t.append(g); }
      row(dl, "Time", t);
      const gp = hh.gps.fitted ? (hh.gps.fix ? "fix, " + hh.gps.sats + " satellites" : "no fix (" + hh.gps.sats + " satellites)") : "not fitted";
      row(dl, "GPS", gp, hh.gps.fix ? "gps" : "");
      row(dl, "Lowest heap", (hh.heap_sat ? "255+ " : hh.heap_kb + " ") + "KB", hh.heap_kb < 20 ? "dangertext" : (hh.heap_kb < 40 ? "warntext" : ""));
      row(dl, "Restarts", hh.restarts + ", brownouts " + hh.brownouts, hh.brownouts ? "warntext" : "");
      row(dl, "Last reset", hh.reset, /crash|brownout/.test(hh.reset) ? "dangertext" : "");
      row(dl, "Status", a.status_age_s == null ? "none" : ago(a.status_age_s), a.status_age_s > 10 ? "stale" : "");
    } else {
      const d = a.disc;
      if (d) {
        row(dl, "Backbone", d.backbone ? "linked" : "no link", d.backbone ? "" : "warntext");
        row(dl, "Grid time", d.time ? "held" : "not held");
        row(dl, "Handhelds", d.attached + " attached, " + d.free_slots + " slots free");
      }
      row(dl, "Status", a.bad_tag ? "cannot be verified (" + a.bad_tag + " frames): wrong lg_secrets.h?" : "none yet (discovery advert only)", a.bad_tag ? "dangertext" : "warntext");
    }
    c.append(dl);
    box.append(c);
  }
}

function renderHandhelds(list) {
  const tb = $("hh"); tb.replaceChildren();
  if (!list.length) { const tr = el("tr"); const td = el("td", "hint", "None heard yet."); td.colSpan = 4; tr.append(td); tb.append(tr); return; }
  for (const h of list) {
    const tr = el("tr");
    tr.append(el("td", "", h.name));
    tr.append(el("td", h.online ? "" : "hint", h.online ? "online" : "offline"));
    tr.append(el("td", "", h.ap_name || "–"));
    const td = el("td");
    if (h.battery == null) td.append(el("span", "hint", "–"));
    else {
      const bar = el("span", "bar"); const i = el("i", h.battery < 20 ? "low" : (h.battery < 40 ? "mid" : ""));
      i.style.width = h.battery + "%"; bar.append(i); td.append(bar, document.createTextNode(h.battery + "%"));
    }
    tr.append(td); tb.append(tr);
  }
}

function renderLog(events, stats) {
  const box = $("log"); box.replaceChildren();
  if (!events.length) box.append(el("div", "hint", "Nothing yet."));
  for (const e of events) {
    const t = new Date(e.t * 1000).toLocaleTimeString();
    box.append(el("div", "k-" + e.kind, t + "  " + e.text));
  }
  const s = stats || {};
  $("stats").textContent = "Status frames verified " + (s.ok || 0) + ", failed the check " + (s.bad_tag || 0) +
    ", replays dropped " + (s.replay || 0) + ", other grids ignored " + (s.other_grid || 0) + ".";
}

async function poll() {
  try {
    const r = await fetch("state.json", { cache: "no-store" });
    const s = await r.json();
    failures = 0;
    $("gt").textContent = s.grid_time ? new Date(s.grid_time * 1000).toLocaleString() : "not heard";
    const up = s.aps.filter(a => a.heard).length;
    $("sub").textContent = "An offline network, watched from this laptop without joining its Wi-Fi. " +
      up + " of " + s.aps.length + " AP(s) heard; listening for " + ago(s.listening_s).replace(" ago", "") + ".";
    renderBanner(s.alert); renderAps(s.aps); renderHandhelds(s.handhelds); renderLog(s.events, s.stats);
  } catch (e) {
    if (++failures >= 3) $("sub").textContent = "grid_watch.py is not answering; is it still running?";
  }
}
poll(); setInterval(poll, 1000);
</script>
</body>
</html>
"""


def make_server(state, port):
    page = PAGE.encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                body, ctype = page, "text/html; charset=utf-8"
            elif path == "/state.json":
                body = json.dumps(state.snapshot(), ensure_ascii=False).encode("utf-8")
                ctype = "application/json"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    return srv


# ---------------------------------------------------------------- BLE

def manufacturer_payloads(adv, company_id):
    """The payloads under the company ID in the advert AND the scan response.

    Windows reports them separately, but bleak merges both into one dict keyed by company ID,
    so the scan response hides the discovery advert. bleak's WinRT backend keeps both raw
    events in platform_data; read them from there, and fall back to the merged dict elsewhere.
    """
    out = []
    pd = getattr(adv, "platform_data", None)
    raw = pd[1] if isinstance(pd, tuple) and len(pd) > 1 else None
    if raw is not None and hasattr(raw, "adv") and hasattr(raw, "scan"):
        for part in (raw.adv, raw.scan):
            if part is None:
                continue
            try:
                for m in part.advertisement.manufacturer_data:
                    if m.company_id == company_id:
                        out.append(bytes(m.data))
            except Exception:
                pass
        if out:
            return out
    d = (adv.manufacturer_data or {}).get(company_id)
    if d:
        out.append(bytes(d))
    return out


async def listen(state, company_id, stop, seconds=None):
    from bleak import BleakScanner

    def on_detect(_device, adv):
        # _device carries the BLE address; it is deliberately never read (D21).
        payloads = manufacturer_payloads(adv, company_id)
        if payloads:
            state.on_advert(payloads, adv.rssi)

    scanner = BleakScanner(detection_callback=on_detect, scanning_mode="active")
    await scanner.start()
    deadline = time.monotonic() + seconds if seconds else None
    try:
        while not stop.is_set() and (deadline is None or time.monotonic() < deadline):
            state.tick()
            try:
                await asyncio.wait_for(stop.wait(), 1.0)
            except asyncio.TimeoutError:
                pass
    finally:
        await scanner.stop()


# ---------------------------------------------------------------- self-check

def self_check():
    """Build frames the way the spec says an AP does, and run them through the decoder."""
    results = []

    def check(name, cond):
        results.append((name, bool(cond)))
        print(f"  {'PASS' if cond else 'FAIL'}  {name}")

    backbone = bytes(range(32))
    disc = bytes([0x11, 0x22, 0x33, 0x44])
    k = derive_key(backbone)
    check("HKDF-SHA256 derives a 32-byte key different from the backbone key",
          len(k) == 32 and k != backbone and k == derive_key(backbone))
    check("the key depends on the salt and info in the spec",
          k != HKDF(hashes.SHA256(), 32, b"LG-BLE-STATUS-2", KDF_INFO).derive(backbone))

    st = GridState(k, disc, {0: "MAIN", 1: "NORTH", 2: "SOUTH"}, echo=False)
    t0 = 1_000_000.0

    def discovery(ap, d=disc, flags=DISC_FLAG_BACKBONE | DISC_FLAG_TIME, attached=2):
        return b"LG" + bytes([1]) + d + bytes([ap, 0, 15 - attached, flags, attached])

    # Discovery from this grid.
    st.on_advert([discovery(1)], -60, t0)
    snap = st.snapshot(t0)
    check("discovery advert with this grid's discriminator lists AP 1 as NORTH",
          len(snap["aps"]) == 1 and snap["aps"][0]["name"] == "NORTH" and snap["aps"][0]["heard"]
          and snap["aps"][0]["disc"]["backbone"] and snap["aps"][0]["disc"]["attached"] == 2)

    # Health frame.
    health = (bytes([0x2C, 0x01, 0b101, 3, (2 << 3) | 0x04 | 2, 87, 9, 12, 4, (7 << 2) | 3]))
    boot = 365
    f = seal_frame(k, 1, T_HEALTH, boot, 10, health)
    check("a health frame is at most 27 bytes", len(f) == 9 + 10 + 4 <= ST_MAX)
    got, why = open_frame(k, f)
    check("a good health frame opens with raw ChaCha20 at counter 1 and the 4-byte tag",
          got is not None and got[:4] == (1, T_HEALTH, boot, 10) and got[4] == health)
    st.on_advert([discovery(1), f], -58, t0 + 0.5)
    h = st.snapshot(t0 + 0.5)["aps"][0]["health"]
    check("health decodes: uptime, links, time, heap, reset reason, counts, GPS",
          h and h["uptime"] == "5 h 0 min" and [l["name"] for l in h["links"]] == ["MAIN", "SOUTH"]
          and h["handhelds_here"] == 3 and h["time_quality"] == "authoritative" and h["time_gps"]
          and "stratum 2" in h["time"] and h["heap_kb"] == 87
          and h["reset"] == "low supply voltage (brownout)" and h["restarts"] == 12
          and h["brownouts"] == 4 and h["gps"] == {"fitted": True, "fix": True, "sats": 7})

    # Tampering.
    bad = bytearray(f); bad[-1] ^= 1
    check("a flipped tag bit is rejected", open_frame(k, bytes(bad))[1] == "tag")
    bad = bytearray(f); bad[12] ^= 0x80
    check("a flipped ciphertext bit is rejected", open_frame(k, bytes(bad))[1] == "tag")
    bad = bytearray(f); bad[3] ^= 1
    check("a changed header (AAD, nonce) is rejected", open_frame(k, bytes(bad))[1] == "tag")
    check("a frame sealed under another grid's key is rejected",
          open_frame(derive_key(bytes(32 * [7])), f)[1] == "tag")
    check("a frame longer than 27 bytes is rejected",
          open_frame(k, f + b"\0" * 5)[1] == "length")
    ok_before = st.stats["ok"]
    bad = bytearray(f); bad[-2] ^= 4
    st.on_advert([discovery(1), bytes(bad)], -58, t0 + 0.6)
    check("the grid state ignores a frame with a bad tag and counts it",
          st.stats["ok"] == ok_before and st.stats["bad_tag"] == 1)

    # Replay.
    st.on_advert([discovery(1), f], -58, t0 + 0.7)
    check("the same frame again is dropped (duplicate)",
          st.stats["ok"] == ok_before and st.stats["duplicate"] == 1)
    old = seal_frame(k, 1, T_HEALTH, boot, 9, health)
    st.on_advert([old], -58, t0 + 0.8)
    check("a lower counter in the same boot is dropped as a replay",
          st.stats["ok"] == ok_before and st.stats["replay"] == 1)
    older_boot = seal_frame(k, 1, T_HEALTH, boot - 1, 500, health)
    st.on_advert([older_boot], -58, t0 + 0.9)
    check("a frame from an earlier boot is dropped as a replay", st.stats["replay"] == 2)

    # Handhelds, batteries, names.
    online = (1 << 0) | (1 << 2) | (1 << 3)          # devices 1, 3, 4
    where = bytearray([0xFF] * 10)
    where[0] = 0xF1                                   # device 1 at AP 1, device 2 unknown
    where[1] = 0x12                                   # device 3 at AP 2, device 4 at AP 1
    hh = online.to_bytes(4, "little") + bytes(where)
    st.on_advert([seal_frame(k, 1, T_HANDHELDS, boot, 11, hh)], -58, t0 + 1)
    st.on_advert([seal_frame(k, 1, T_BATTERY, boot, 12, bytes([1, 80, 3, 15, 4, 255]))], -58, t0 + 1.1)
    name = "Priya été".encode("utf-8")
    st.on_advert([seal_frame(k, 1, T_NAME, boot, 13, bytes([3]) + name)], -58, t0 + 1.2)
    hs = {x["device"]: x for x in st.snapshot(t0 + 1.3)["handhelds"]}
    check("handhelds decode: who is online and at which AP",
          set(hs) == {1, 3, 4} and hs[1]["ap_name"] == "NORTH" and hs[3]["ap_name"] == "SOUTH"
          and hs[4]["ap_name"] == "NORTH" and all(x["online"] for x in hs.values()))
    check("batteries decode, 255 shown as no battery sense",
          hs[1]["battery"] == 80 and hs[3]["battery"] == 15 and hs[4]["battery"] is None)
    check("a name frame decodes UTF-8 and replaces the fallback name",
          hs[3]["name"] == "Priya été" and hs[1]["name"] == "Handheld 1")

    # Alert: active SOS, then all clear.
    alert = bytes([1, 3]) + (120).to_bytes(2, "little") + bytes([2, 0]) + (1_789_700_000).to_bytes(4, "little")
    st.on_advert([seal_frame(k, 1, T_ALERT, boot, 14, alert)], -58, t0 + 2)
    s = st.snapshot(t0 + 2)
    a = s["alert"]
    check("an active alert decodes: who, where, how long ago, read reports, grid time",
          a and a["active"] and not a["all_clear"] and a["name"] == "Priya été"
          and a["near"] == "SOUTH" and a["age_s"] == 120 and a["reads"] == 2
          and s["grid_time"] == 1_789_700_000 and any(e["kind"] == "alert" for e in s["events"]))
    clear = bytes([2, 3]) + (5).to_bytes(2, "little") + bytes([255, 0]) + (1_789_700_060).to_bytes(4, "little")
    st.on_advert([seal_frame(k, 1, T_ALERT, boot, 15, clear)], -58, t0 + 62)
    a = st.snapshot(t0 + 62)["alert"]
    check("an all clear replaces the alert", a and a["all_clear"] and a["reads"] is None
          and any(e["kind"] == "all_clear" for e in st.events))

    # Restart: a new boot starts a new counter space.
    st.on_advert([seal_frame(k, 1, T_HEALTH, boot + 1, 0, health)], -58, t0 + 63)
    check("a higher boot counter is accepted as a restart, counter from 0",
          st.aps[1]["boot"] == boot + 1 and any(e["kind"] == "ap_restart" for e in st.events))

    # Another grid.
    n_aps = len(st.aps)
    foreign = seal_frame(derive_key(bytes(32 * [9])), 5, T_HEALTH, 1, 1, health)
    st.on_advert([discovery(5, d=b"\x99\x99\x99\x99"), foreign], -70, t0 + 64)
    check("an AP with another grid's discriminator is ignored, status and all",
          len(st.aps) == n_aps and 5 not in st.aps and st.stats["other_grid"] == 1)

    # Silence.
    st.tick(t0 + 63 + AP_SILENT_S + 1)
    s = st.snapshot(t0 + 63 + AP_SILENT_S + 1)
    check(f"an AP silent for more than {AP_SILENT_S:.0f} s is marked not heard",
          not s["aps"][0]["heard"] and any(e["kind"] == "ap_lost" for e in s["events"]))

    # The dashboard answers on loopback and serves JSON without addresses.
    srv = make_server(st, 0)
    port = srv.server_address[1]
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    try:
        import urllib.request
        page = urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=5).read().decode()
        data = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/state.json", timeout=5).read())
        text = json.dumps(data)
        check("the page and state.json are served on 127.0.0.1 only",
              srv.server_address[0] == "127.0.0.1" and "grid watch" in page and "aps" in data)
        check("the page needs no network (no external URLs)",
              not re.search(r"(src|href)=\"https?://", page))
        check("nothing served looks like a BLE address (D21)",
              not re.search(r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}", text + page))
    finally:
        srv.shutdown()
        srv.server_close()

    failed = [n for n, ok in results if not ok]
    print(f"\nSELF-CHECK: {'PASS' if not failed else 'FAIL'} "
          f"({len(results) - len(failed)} of {len(results)} checks)")
    return 0 if not failed else 1


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"local port for the dashboard (default {DEFAULT_PORT})")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser")
    ap.add_argument("--secrets", default=DEFAULT_SECRETS,
                    help="this grid's lg_secrets.h (default firmware/common/lg_secrets.h)")
    ap.add_argument("--seconds", type=float, metavar="N",
                    help="stop by itself after N seconds and print what was heard")
    ap.add_argument("--log", metavar="FILE", help="append every event to this JSON-lines file")
    ap.add_argument("--self-check", action="store_true",
                    help="offline: seal frames per docs/ble-status.md and check the decoder")
    args = ap.parse_args()

    if args.self_check:
        print("grid_watch self-check: frames built per docs/ble-status.md with a test key")
        return self_check()

    try:
        backbone, disc = parse_secrets(args.secrets)
        company_id = parse_company_id()
    except (OSError, ValueError) as e:
        print(f"Cannot read the grid's settings: {e}")
        return 2

    state = GridState(derive_key(backbone), disc, bench_ap_names(), log_path=args.log)
    try:
        srv = make_server(state, args.port)
    except OSError as e:
        print(f"Cannot serve on 127.0.0.1:{args.port}: {e}. Try --port.")
        return 2
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    print(f"LocalGrid grid watch: listening over BLE for this grid's APs (company ID "
          f"0x{company_id:04X}); dashboard at {url}")
    print("Only listening: this laptop never joins the grid (D25). Ctrl+C to stop.")
    if not args.no_browser:
        webbrowser.open(url)

    async def run():
        stop = asyncio.Event()
        try:
            await listen(state, company_id, stop, args.seconds)
        except asyncio.CancelledError:
            stop.set()
            raise

    rc = 0
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    except Exception as e:                      # the radio: off, missing, or refused
        print(f"BLE scanning failed: {type(e).__name__}: {e}. Is Bluetooth switched on?")
        rc = 2
    finally:
        srv.shutdown()
        srv.server_close()
    s = state.snapshot()
    heard = sorted(a["index"] for a in s["aps"])
    print(f"\nStopped. APs heard: {', '.join(f'{state.ap_name(i)} (AP {i})' for i in heard) or 'none'}; "
          f"status frames verified {s['stats'].get('ok', 0)}, failed the check "
          f"{s['stats'].get('bad_tag', 0)}, replays dropped {s['stats'].get('replay', 0)}.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
