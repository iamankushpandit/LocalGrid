#!/usr/bin/env python3
"""Watch a LocalGrid offline network over BLE, without joining its Wi-Fi (decision D68).

Every AP advertises its discovery payload over BLE and, since D68, puts a sealed status frame in
its scan response (docs/ble-status.md): AP health, which handhelds are online and where, the
newest urgent alert, handheld batteries, and chosen names. This tool listens for both, checks
each status frame against this grid's key, and serves a small dashboard on this laptop.

Since D70 it can also ask an AP for the bigger picture over a BLE connection (docs/ble-link.md):
the admin page's own status and history, the map of everyone's positions, and the traffic and
performance counters. That link is sealed with the same grid key, needs the admin password, and
is read-only: there is no opcode that writes, none that carries message text, and none that
carries audio. The watcher monitors and takes no part in the grid.

Decision D25 stands: the laptop is an observer, never a participant. It never joins the grid's
Wi-Fi, has no device ID, never carries a grid message, and never speaks the LocalGrid protocol.
Being exact about the radio: Windows scans actively so that scan responses arrive, which means the
laptop sends BLE scan requests, and the admin link means it writes to an AP's BLE characteristic.
Neither is grid traffic, and neither changes anything on the grid.

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
  python tools/grid_watch.py --pair          show the QR code that pairs the Android app (D69)

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

AP_SILENT_S = 45.0               # an AP not heard for this long is "not heard"
# This laptop's Bluetooth shares its radio (and usually its antenna) with Wi-Fi, so Windows stalls
# scanning for seconds at a time. Measured on the bench: adverts normally arrive every 0.25 s, but
# every AP goes quiet together for up to 15 s, which is the laptop, not the grid. A threshold of a
# few seconds therefore reports APs as lost when they are transmitting perfectly well. 45 s is long
# enough to ride out those stalls and still notice an AP that has really gone (it beacons twice a
# second). The quiet spell itself is still visible as "last heard N s ago".
FRAME_FRESH_S = 90.0             # a handhelds or alert frame older than this no longer counts
# Presence reaches this laptop only through an AP's rotating beacon: health every other frame, and
# handhelds / alert / batteries / names taking turns, so about one handhelds frame per AP every 4 s
# (docs/ble-status.md). The receiver's own quiet spells (see AP_SILENT_S) swallow several in a row,
# and a handheld vanishing from the list is a more alarming false signal than an AP flickering, so
# this window is generous. Nothing urgent waits on it: an alert raises its banner the moment its
# frame is decoded.
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


# ---------------------------------------------------------------- the admin link (D70)
#
# docs/ble-link.md: one GATT service on every AP. The watcher connects, says HELLO, logs in with
# the admin password (never sending it), and asks for the admin page's own /api/status and
# /api/history bytes, plus the traffic counters. Every message is sealed with ChaCha20-Poly1305
# under a key derived from the same K the pairing code carries. Read-only: no opcode writes.

LINK_KDF_SALT = b"LG-BLE-LINK-1"
LINK_KDF_INFO = b"admin link"
LINK_VERSION = 1
LINK_PROOF_CONTEXT = b"lg-ble-admin"
# The spec writes the UUIDs as "4c47-0001-…" and leaves the tail open, so the client matches on
# the first 8 hex digits and takes whatever tail the AP's firmware uses.
LINK_SERVICE_PREFIX = "4c470001"
LINK_REQUEST_PREFIX = "4c470002"
LINK_REPLY_PREFIX = "4c470003"

OP_HELLO, OP_LOGIN, OP_GET_STATUS, OP_GET_HISTORY, OP_GET_TRAFFIC = 0x01, 0x02, 0x03, 0x04, 0x05
OP_SESSION = 0x80                # the one unsealed message: the session both nonces are built from
OP_HELLO_OK, OP_LOGIN_OK, OP_STATUS, OP_HISTORY, OP_TRAFFIC = 0x81, 0x82, 0x83, 0x84, 0x85
OP_ERROR, OP_LOGIN_FAIL = 0xC0, 0xC2
OP_NAMES = {OP_SESSION: "SESSION", OP_HELLO: "HELLO", OP_LOGIN: "LOGIN", OP_GET_STATUS: "GET_STATUS",
            OP_GET_HISTORY: "GET_HISTORY", OP_GET_TRAFFIC: "GET_TRAFFIC",
            OP_HELLO_OK: "HELLO_OK", OP_LOGIN_OK: "LOGIN_OK", OP_STATUS: "STATUS",
            OP_HISTORY: "HISTORY", OP_TRAFFIC: "TRAFFIC", OP_ERROR: "ERROR",
            OP_LOGIN_FAIL: "LOGIN_FAIL"}

LINK_FLAG_MORE = 0x01            # a reply chunk that is not the last one
LINK_HEADER = 4                  # opcode u8, flags u8, length u16 LE
LINK_TAG = 16                    # the full Poly1305 tag: no advert budget here
LINK_CHUNK_BODY = 32             # body bytes per request chunk; requests are tiny anyway
HELLO_OK_LEN = 1 + 1 + 4 + 1 + 16 + 4 + 32
LINK_MAX_REPLY = 64 * 1024       # a reply longer than this is a fault, not a message

DIR_TO_AP, DIR_TO_CLIENT = 0, 1


class LinkError(Exception):
    """The link failed its own rules: a bad tag, a counter out of order, or a refused request."""


def link_key(k):
    """K_link = HKDF-SHA256(salt, ikm = K, info, 32), K being the status key the pairing code holds."""
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=LINK_KDF_SALT,
                info=LINK_KDF_INFO).derive(k)


def link_nonce(direction, session, counter):
    """dir (u8), session (u16 LE), 0x00, counter (u32 LE), four zero bytes."""
    return (bytes([direction & 1]) + (session & 0xFFFF).to_bytes(2, "little") + b"\x00"
            + (counter & 0xFFFFFFFF).to_bytes(4, "little") + b"\x00\x00\x00\x00")


def seal_link(key, direction, session, counter, opcode, flags, body):
    """One sealed message: the 4-byte header is the AAD and travels in the clear."""
    header = bytes([opcode, flags]) + len(body).to_bytes(2, "little")
    return header + ChaCha20Poly1305(key).encrypt(link_nonce(direction, session, counter),
                                                  bytes(body), header)


def open_link(key, direction, session, counter, data):
    """The other side of seal_link. Raises LinkError on anything that does not add up."""
    if len(data) < LINK_HEADER + LINK_TAG:
        raise LinkError("a link message shorter than a header and a tag")
    header, sealed = bytes(data[:LINK_HEADER]), bytes(data[LINK_HEADER:])
    opcode, flags = header[0], header[1]
    length = int.from_bytes(header[2:4], "little")
    if length != len(sealed) - LINK_TAG:
        raise LinkError("the length in the header does not match the message")
    try:
        body = ChaCha20Poly1305(key).decrypt(link_nonce(direction, session, counter), sealed, header)
    except Exception:
        raise LinkError("a link message failed its tag: wrong key, session, or counter")
    return opcode, flags, body


class ChunkJoiner:
    """Joins a reply's chunks. Each chunk is its own sealed message; bit 0 of flags means more."""

    def __init__(self):
        self.opcode = None
        self.parts = []
        self.size = 0

    def add(self, opcode, flags, body):
        """Returns (opcode, body) when the reply is complete, else None."""
        if self.opcode is not None and opcode != self.opcode:
            raise LinkError("a reply changed opcode halfway through its chunks")
        self.opcode = opcode
        self.parts.append(bytes(body))
        self.size += len(body)
        if self.size > LINK_MAX_REPLY:
            raise LinkError("a reply longer than this tool accepts")
        if flags & LINK_FLAG_MORE:
            return None
        out = (opcode, b"".join(self.parts))
        self.opcode, self.parts, self.size = None, [], 0
        return out


def split_chunks(body, chunk=LINK_CHUNK_BODY):
    """The sender's side of the chunking, used by the mock AP and --self-check."""
    if not body:
        return [(0, b"")]
    parts = [body[i:i + chunk] for i in range(0, len(body), chunk)]
    return [(LINK_FLAG_MORE if i < len(parts) - 1 else 0, p) for i, p in enumerate(parts)]


def login_hash(password, salt, iterations):
    """What the AP stores: PBKDF2-HMAC-SHA256(password, salt, iterations, 32)."""
    import hashlib
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes(salt), int(iterations), 32)


def login_proof(hash32, challenge):
    """HMAC-SHA256(key = the stored hash, message = challenge || "lg-ble-admin")."""
    import hashlib
    return hmac.new(bytes(hash32), bytes(challenge) + LINK_PROOF_CONTEXT, hashlib.sha256).digest()


def parse_hello_ok(body):
    """version u8, AP u8, boot u32, password set u8, salt 16, iterations u32, challenge 32.

    The challenge is the last field; the doc's list stops at the iterations, but logging in needs
    it (ble_link.c builds exactly these 59 bytes).
    """
    if len(body) < HELLO_OK_LEN:
        raise LinkError("HELLO_OK is too short")
    return {"version": body[0], "ap": body[1],
            "boot": int.from_bytes(body[2:6], "little"), "password_set": bool(body[6]),
            "salt": bytes(body[7:23]), "iterations": int.from_bytes(body[23:27], "little"),
            "challenge": bytes(body[27:59])}


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


# ---------------------------------------------------------------- the pulled replies, decoded
#
# STATUS is the admin page's own /api/status JSON. HISTORY and TRAFFIC are packed records (D49):
# the bytes are decoded here and the words are made in the browser, exactly as the admin page does.

HIST_HEADER = 16
HIST_AP = 76
HIST_AVAIL_MINUTES = 120
HIST_INCIDENT = 16
INCIDENT_KINDS = ["down", "restarted", "link", "unexplained"]


def decode_history(blob):
    """GET /api/history, layout 1 (h_history in firmware/node/main/web_admin.c)."""
    b = bytes(blob)
    if len(b) < HIST_HEADER or b[0] != 1:
        raise LinkError("history bytes are not layout 1")
    u32 = lambda o: int.from_bytes(b[o:o + 4], "little")
    u16 = lambda o: int.from_bytes(b[o:o + 2], "little")
    i16 = lambda o: int.from_bytes(b[o:o + 2], "little", signed=True)
    n_aps, n_inc = b[2], b[3]
    out = {"self": b[1],
           "time_set": {"unix": u32(4), "on": -1 if b[8] == 255 else b[8],
                        "from": -1 if b[9] == 255 else b[9], "gps": b[10] == 1},
           "newest_min": u32(12), "aps": [], "incidents": []}
    o = HIST_HEADER
    for _ in range(n_aps):
        if o + HIST_AP > len(b):
            break
        name = b[o + 28:o + 44].split(b"\0", 1)[0].decode("utf-8", errors="replace")
        avail = [(b[o + 44 + (m >> 2)] >> ((m % 4) * 2)) & 3 for m in range(HIST_AVAIL_MINUTES)]
        sync_age, prev = u32(o + 16), u32(o + 24)
        out["aps"].append({
            "ap": b[o], "self": bool(b[o + 1] & 1), "time_quality": b[o + 2], "stratum": b[o + 3],
            "sync_from": -1 if b[o + 4] == 255 else b[o + 4], "reset_reason": b[o + 5],
            "reset": RESET_WORDS.get(b[o + 5], f"reason {b[o + 5]}"),
            "sync_drift_ms": i16(o + 6), "uptime_s": u32(o + 8), "heard_s": u32(o + 12),
            "sync_age_s": -1 if sync_age == 0xFFFFFFFF else sync_age, "boot": u32(o + 20),
            "prev_run_s": -1 if prev == 0xFFFFFFFF else prev, "name": name, "avail": avail})
        o += HIST_AP
    for _ in range(n_inc):
        if o + HIST_INCIDENT > len(b):
            break
        kr, prev_min = b[o + 11], u16(o + 6)
        out["incidents"].append({
            "down_grid_time": u32(o), "duration_s": u16(o + 4),
            "prev_run_s": -1 if prev_min == 0xFFFF else prev_min * 60, "ap": b[o + 10],
            "kind": INCIDENT_KINDS[kr & 3], "reset": RESET_WORDS.get(kr >> 2, "unknown"),
            "seen_by": b[o + 12]})
        o += HIST_INCIDENT
    return out


# TRAFFIC (opcode 0x85): the packed record traffic.c builds on the AP (D70). Little-endian.
# Trailing bytes a newer AP adds are ignored; a record in an unknown layout is refused, not guessed.
#
#   header, 24 B:   u8 layout (1) | u8 this AP | u8 backbone links | u8 message classes |
#                   u8 rate buckets | u8 CPU busy percent (255 not measured) | u16 bucket ms |
#                   u32 uptime s | u32 grid time | u32 messages in | u32 messages out
#   class, 12 B each, in MSG_CLASSES order: u32 in | u32 out | u32 relayed
#   faults:         10 u32 counters, in FAULT_FIELDS order
#   handhelds, 28 B: u8 sessions open now | u8 registered now | u16 pad | u32 sessions opened |
#                   u32 registrations | u32 disconnects | u32 bytes in | u32 bytes out |
#                   u32 slowest send wait ms
#   performance, 40 B: u32 free heap | u32 lowest free heap | u32 largest free block |
#                   u16 core queue depth | u16 queue high-water | u32 core stack headroom B |
#                   u32 link task stack headroom B | u32 longest main-loop pass ms |
#                   u32 average pass ms | u32 ESP-NOW errors | u32 Wi-Fi side errors
#   bucket, 4 B each, oldest first: u16 messages in | u16 messages out
#   link, 28 B each: u8 AP at the far end | u8 up | i8 RSSI | u8 pad | u32 frames sent |
#                   u32 bytes sent | u32 frames received | u32 bytes received | u32 send failures |
#                   u32 since a frame arrived, ms
TRAFFIC_LAYOUT = 1
TRAFFIC_HEADER = 24
TRAFFIC_CLASS = 12
TRAFFIC_SESS = 28
TRAFFIC_PERF = 40
TRAFFIC_LINK = 28
TRAFFIC_BUCKET = 4
# lg_traffic_class_t (components/lg_core/include/lg_node.h), in its own order.
MSG_CLASSES = ["direct", "group", "broadcast", "voice", "ack", "presence", "announce", "position",
               "time", "other"]
FAULT_FIELDS = ["duplicate", "table_full", "unknown_recipient", "decrypt_failed", "ttl_expired",
                "queue_full", "send_timeout", "voice_dropped", "malformed", "rejected"]


def decode_traffic(blob):
    """TRAFFIC bytes -> dict. Raises LinkError if the layout is not one this tool knows."""
    b = bytes(blob)
    if len(b) < TRAFFIC_HEADER or b[0] != TRAFFIC_LAYOUT:
        raise LinkError("traffic bytes are not layout 1")
    u16 = lambda o: int.from_bytes(b[o:o + 2], "little")
    u32 = lambda o: int.from_bytes(b[o:o + 4], "little")
    n_links, n_classes, n_buckets, cpu = b[2], b[3], b[4], b[5]
    if n_classes != len(MSG_CLASSES):
        raise LinkError(f"this AP counts {n_classes} message classes, this tool knows "
                        f"{len(MSG_CLASSES)}")
    out = {"ap": b[1], "cpu_busy": None if cpu == 255 else cpu, "bucket_s": u16(6) / 1000.0,
           "uptime_s": u32(8), "grid_time": u32(12), "in_total": u32(16), "out_total": u32(20),
           "messages": {}, "faults": {}, "links": [], "buckets": []}
    o = TRAFFIC_HEADER
    need = (n_classes * TRAFFIC_CLASS + len(FAULT_FIELDS) * 4 + TRAFFIC_SESS + TRAFFIC_PERF
            + n_buckets * TRAFFIC_BUCKET)
    if o + need > len(b):
        raise LinkError("traffic bytes stop before the fixed sections end")
    for cls in MSG_CLASSES:
        out["messages"][cls] = {"in": u32(o), "out": u32(o + 4), "relayed": u32(o + 8)}
        o += TRAFFIC_CLASS
    for name in FAULT_FIELDS:
        out["faults"][name] = u32(o)
        o += 4
    out["handhelds"] = {"sessions": b[o], "registered": b[o + 1], "opened": u32(o + 4),
                        "registrations": u32(o + 8), "disconnects": u32(o + 12),
                        "bytes_in": u32(o + 16), "bytes_out": u32(o + 20),
                        "slowest_send_ms": u32(o + 24)}
    o += TRAFFIC_SESS
    out["perf"] = {"heap_free": u32(o), "heap_min": u32(o + 4), "heap_largest": u32(o + 8),
                   "queue_depth": u16(o + 12), "queue_high": u16(o + 14),
                   "stack_core": u32(o + 16), "stack_link": u32(o + 20),
                   "loop_max_ms": u32(o + 24), "loop_avg_us": u32(o + 28),
                   "espnow_errors": u32(o + 32), "wifi_errors": u32(o + 36),
                   "cpu_busy": None if cpu == 255 else cpu}
    o += TRAFFIC_PERF
    for _ in range(n_buckets):
        out["buckets"].append({"in": u16(o), "out": u16(o + 2),
                               "messages": u16(o) + u16(o + 2)})
        o += TRAFFIC_BUCKET
    for _ in range(n_links):
        if o + TRAFFIC_LINK > len(b):
            break
        heard = u32(o + 24)
        out["links"].append({"ap": b[o], "up": bool(b[o + 1]),
                             "rssi": int.from_bytes(b[o + 2:o + 3], "little", signed=True),
                             "sent": u32(o + 4), "bytes_out": u32(o + 8),
                             "received": u32(o + 12), "bytes_in": u32(o + 16),
                             "failures": u32(o + 20),
                             "heard_s": None if heard == 0xFFFFFFFF else round(heard / 1000.0)})
        o += TRAFFIC_LINK
    # The rates the page shows, worked out here so every viewer sees the same arithmetic.
    bs = out["bucket_s"] or 30.0
    buckets = out["buckets"]

    def rate(seconds):
        take = buckets[-max(1, round(seconds / bs)):]
        if not take:
            return None
        return round(60.0 * sum(x["messages"] for x in take) / (len(take) * bs), 1)

    out["rate_1m"] = rate(60) if buckets else None
    out["rate_5m"] = rate(300) if buckets else None
    out["total_messages"] = out["in_total"] + out["out_total"]
    return out


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
        # AP index -> the bleak device to connect to for the admin link (D70). A BLE address is a
        # hardware address: it is kept here to open a connection and is never printed, logged or
        # served (D21).
        self._radio = {}
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

    def best_ap(self):
        """The AP to ask for the bigger data: heard just now, strongest signal. Never a name."""
        now = time.time()
        best = None
        with self.lock:
            for i, a in self.aps.items():
                if not a["heard"] or now - a["last"] > AP_SILENT_S or i not in self._radio:
                    continue
                score = a["rssi"] if a["rssi"] is not None else -120
                if best is None or score > best[0]:
                    best = (score, i, self._radio[i])
        return None if best is None else (best[1], best[2])

    def on_advert(self, payloads, rssi, now=None, radio=None):
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
                if radio is not None:
                    self._radio[d["ap"]] = radio
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


# ---------------------------------------------------------------- talking to an AP (D70)

LINK_TIMEOUT_S = 12.0            # one request and its reply
LINK_PERIOD_S = 60.0             # a pull cycle when nobody is watching the traffic tab
LINK_TRAFFIC_PERIOD_S = 30.0     # while the Traffic tab is open: one short connection each time


class LinkSession:
    """The sealed conversation with one AP, independent of how the bytes travel.

    A real connection hands it a bleak client; --self-check hands it a mock AP in this process.
    Counters rise by one per message in each direction and never repeat; the session is the number
    the AP picks, and HELLO/HELLO_OK run at session 0 because the client cannot know it yet.
    """

    def __init__(self, key, send):
        self.key = key
        self._send = send
        self.session = None
        self.tx_counter = 0
        self.rx_counter = 0
        self.joiner = ChunkJoiner()
        self.queue = asyncio.Queue()
        self.opened = asyncio.Event()
        self.chunk = LINK_CHUNK_BODY
        self.logged_in = False
        self.hello = None

    def feed(self, data):
        """One notification from the AP. Anything wrong ends the conversation (docs/ble-link.md)."""
        data = bytes(data)
        if len(data) == LINK_HEADER + 2 and data[0] == OP_SESSION:
            # The only message that is not sealed: it carries the session number both directions'
            # nonces are built from. A nonce is public by design (ble_link.c).
            self.session = int.from_bytes(data[LINK_HEADER:], "little")
            self.opened.set()
            return
        if self.session is None:
            self.queue.put_nowait(LinkError("the AP sent a sealed message before its session"))
            return
        try:
            opcode, flags, body = open_link(self.key, DIR_TO_CLIENT, self.session,
                                            self.rx_counter, data)
            self.rx_counter += 1
            done = self.joiner.add(opcode, flags, body)
        except LinkError as e:
            self.queue.put_nowait(e)
            return
        if done is not None:
            self.queue.put_nowait(done)

    async def wait_open(self, timeout=LINK_TIMEOUT_S):
        """The AP sends its session as soon as the watcher subscribes to the reply."""
        await asyncio.wait_for(self.opened.wait(), timeout)

    async def send(self, opcode, body=b""):
        for flags, part in split_chunks(bytes(body), self.chunk):
            await self._send(seal_link(self.key, DIR_TO_AP, self.session, self.tx_counter,
                                       opcode, flags, part))
            self.tx_counter += 1

    async def reply(self, timeout=LINK_TIMEOUT_S):
        got = await asyncio.wait_for(self.queue.get(), timeout)
        if isinstance(got, Exception):
            raise got
        opcode, body = got
        if opcode == OP_ERROR:
            code = body[0] if body else 0
            raise LinkError(f"the AP refused: {body[1:].decode('utf-8', 'replace') or code}")
        return opcode, body

    async def ask(self, opcode, want, body=b""):
        await self.send(opcode, body)
        got, reply = await self.reply()
        if got != want:
            raise LinkError(f"asked for {OP_NAMES.get(want, want)} and got "
                            f"{OP_NAMES.get(got, hex(got))}")
        return reply

    async def say_hello(self):
        await self.wait_open()
        body = await self.ask(OP_HELLO, OP_HELLO_OK, bytes([LINK_VERSION]))
        info = parse_hello_ok(body)
        if info["version"] != LINK_VERSION:
            raise LinkError(f"this AP speaks admin link version {info['version']}, "
                            f"this tool speaks {LINK_VERSION}")
        self.hello = info
        return info

    async def log_in(self, auth):
        """Sends the HMAC proof, never the password. Returns (ok, message)."""
        info = self.hello
        if not info["password_set"]:
            return False, "This AP has no admin password yet: set it up on its admin page first."
        proof = auth.proof(info["salt"], info["iterations"], info["challenge"])
        if proof is None:
            return False, "Log in to see positions, groups, availability and traffic."
        await self.send(OP_LOGIN, proof)
        opcode, body = await self.reply()
        if opcode == OP_LOGIN_OK:
            self.logged_in = True
            return True, "Logged in."
        if opcode == OP_LOGIN_FAIL:
            wait = int.from_bytes(body[:4], "little") if len(body) >= 4 else 0
            return False, ("Wrong admin password." if not wait else
                           f"Too many failed attempts. Try again in {wait} seconds.")
        raise LinkError(f"unexpected reply to LOGIN: {OP_NAMES.get(opcode, hex(opcode))}")


class AdminAuth:
    """The admin password while the tool runs: in memory only, never written down or printed.

    The page posts it to 127.0.0.1; from it this makes the PBKDF2 hash the AP stores and, per
    connection, the HMAC proof. The password itself never leaves this process.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self._password = None
        self._hash = None
        self._hash_for = None
        self.state = "none"          # none | trying | ok | failed
        self.message = "Log in with the admin password to see positions and the other tabs."

    def set_password(self, password):
        with self.lock:
            self._password = password or None
            self._hash = self._hash_for = None
            self.state = "trying" if self._password else "none"
            self.message = ("Checking the password with an AP…" if self._password else
                            "Log in with the admin password to see positions and the other tabs.")

    def forget(self):
        with self.lock:
            self._password = self._hash = self._hash_for = None
            self.state = "none"
            self.message = "Logged out. The password was never written to disk."

    def have_password(self):
        with self.lock:
            return self._password is not None

    def proof(self, salt, iterations, challenge):
        with self.lock:
            if self._password is None:
                return None
            if self._hash is None or self._hash_for != (bytes(salt), int(iterations)):
                self._hash = login_hash(self._password, salt, iterations)
                self._hash_for = (bytes(salt), int(iterations))
        return login_proof(self._hash, challenge)

    def result(self, ok, message):
        with self.lock:
            if ok:
                self.state, self.message = "ok", message
            else:
                self.state = "failed"
                self.message = message
                if "Wrong admin password" in message:
                    self._password = self._hash = self._hash_for = None

    def snapshot(self):
        with self.lock:
            return {"state": self.state, "message": self.message,
                    "logged_in": self.state == "ok"}


class LinkData:
    """What the last pull brought back: never a BLE address, only the AP's name (D21)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sections = {}           # name -> {"data": …, "at": epoch, "ap": name}
        self.traffic = {}            # AP index -> {"data": …, "at": epoch, "ap": name}
        self.state = "idle"          # idle | connecting | pulling | done | error
        self.message = "Not connected to an AP yet."
        self.last_try = None
        self.last_ok = None
        self.want_traffic = False
        self.request = threading.Event()

    def note(self, state, message):
        with self.lock:
            self.state, self.message, self.last_try = state, message, time.time()

    def store(self, name, data, ap_name, now=None):
        with self.lock:
            self.sections[name] = {"data": data, "at": now or time.time(), "ap": ap_name}
            self.last_ok = time.time()

    def store_traffic(self, ap_index, decoded, ap_name, now=None):
        """Kept per AP, so the Traffic tab can total across the APs it has reached."""
        with self.lock:
            self.traffic[ap_index] = {"data": decoded, "at": now or time.time(), "ap": ap_name}
            self.last_ok = time.time()

    def drop(self, name):
        with self.lock:
            self.sections.pop(name, None)
            if name == "traffic":
                self.traffic.clear()

    def snapshot(self, now, logged_in):
        with self.lock:
            out = {"state": self.state, "message": self.message,
                   "last_try_age_s": None if not self.last_try else round(now - self.last_try),
                   "sections": {}}
            if not logged_in:
                return out            # positions and the rest stay hidden until the login works
            for name, sec in self.sections.items():
                out["sections"][name] = {"at_age_s": round(now - sec["at"]), "ap": sec["ap"],
                                         "data": sec["data"]}
            if self.traffic:
                items = [{"ap": t["ap"], "at_age_s": round(now - t["at"]), "data": t["data"]}
                         for _, t in sorted(self.traffic.items())]
                newest = min(self.traffic.values(), key=lambda t: now - t["at"])
                out["sections"]["traffic"] = {"at_age_s": round(now - newest["at"]),
                                              "ap": newest["ap"], "data": items}
            return out


async def pull_once(session, auth, data, ap_name, want_traffic):
    """HELLO, LOGIN, GET_STATUS, GET_HISTORY and, when the Traffic tab is open, GET_TRAFFIC."""
    info = await session.say_hello()
    ok, message = await session.log_in(auth)
    auth.result(ok, message)
    if not ok:
        data.note("error", message)
        return False
    body = await session.ask(OP_GET_STATUS, OP_STATUS)
    data.store("status", json.loads(body.decode("utf-8")), ap_name)
    body = await session.ask(OP_GET_HISTORY, OP_HISTORY)
    data.store("history", decode_history(body), ap_name)
    if want_traffic:
        try:
            body = await session.ask(OP_GET_TRAFFIC, OP_TRAFFIC)
            decoded = decode_traffic(body)
            data.store_traffic(decoded.get("ap", info["ap"]), decoded, ap_name)
        except LinkError as e:
            data.drop("traffic")
            data.note("done", f"{ap_name} answered status and history; no traffic counters ({e}).")
            return True
    data.note("done", f"Pulled from {ap_name}.")
    return True


async def pull_from_device(device, key, auth, data, ap_name, want_traffic):
    """One short BLE connection: connect, talk, disconnect. The AP takes one client at a time."""
    from bleak import BleakClient

    async with BleakClient(device, timeout=15.0) as client:
        req = reply = None
        services = getattr(client, "services", None)
        if not services and hasattr(client, "get_services"):
            services = await client.get_services()       # bleak before 0.21
        for service in (services or []):
            if not str(service.uuid).replace("-", "").lower().startswith(LINK_SERVICE_PREFIX):
                continue
            for ch in service.characteristics:
                u = str(ch.uuid).replace("-", "").lower()
                if u.startswith(LINK_REQUEST_PREFIX):
                    req = ch
                elif u.startswith(LINK_REPLY_PREFIX):
                    reply = ch
        if req is None or reply is None:
            raise LinkError(f"{ap_name} has no admin link service (firmware older than D70)")

        async def send(payload):
            await client.write_gatt_char(req, payload, response=False)

        session = LinkSession(key, send)
        # The AP needs room for a header, a tag and a body in one ATT packet (it refuses an MTU
        # under 64). Requests are tiny, but keep the chunk inside whatever was negotiated.
        mtu = getattr(client, "mtu_size", 0) or 0
        if mtu:
            session.chunk = max(16, min(LINK_CHUNK_BODY, mtu - 3 - LINK_HEADER - LINK_TAG))
        await client.start_notify(reply, lambda _c, d: session.feed(d))
        try:
            return await pull_once(session, auth, data, ap_name, want_traffic)
        finally:
            try:
                await client.stop_notify(reply)
            except Exception:
                pass


async def puller(state, key, auth, data, stop):
    """Pulls the bigger data now and then, and whenever the page presses Refresh.

    Gentle on the AP: one connection a minute at rest, one every 30 s while the Traffic tab is
    open, and never two at once. The beacon keeps the live view going in between.
    """
    while not stop.is_set():
        period = LINK_TRAFFIC_PERIOD_S if data.want_traffic else LINK_PERIOD_S
        waited = 0.0
        while not stop.is_set() and waited < period and not data.request.is_set():
            await asyncio.sleep(0.25)
            waited += 0.25
        if stop.is_set():
            break
        data.request.clear()
        if not auth.have_password():
            data.note("idle", "Log in to see positions, groups, availability and traffic.")
            continue
        pick = state.best_ap()
        if pick is None:
            data.note("error", "No AP of this grid is in range to ask.")
            continue
        ap_index, device = pick
        name = state.ap_name(ap_index)
        data.note("connecting", f"Asking {name}…")
        try:
            await pull_from_device(device, key, auth, data, name, data.want_traffic)
        except asyncio.TimeoutError:
            data.note("error", f"{name} did not answer in time; it may be busy with another watcher.")
        except LinkError as e:
            data.note("error", f"{name}: {e}")
        except Exception as e:                      # a dropped connection, a busy AP, a stack error
            data.note("error", f"{name}: {type(e).__name__}: {e}")


# ---------------------------------------------------------------- the dashboard

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<meta name="referrer" content="no-referrer">
<title>LocalGrid grid watch</title>
<style>
  /* Theme tokens from firmware/node/main/web/admin.html (D10): same page, same colours. */
  :root {
    --bg: #0b1310; --surface: #111d18; --line: #1f3329; --text: #d7efe0; --muted: #86a596;
    --accent: #5fd38d; --accent-ink: #06120c; --warn: #f0b64a; --danger: #ef6b6b;
    --radius: 10px; --gap: 14px; --font: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    --mono: ui-monospace, "SFMono-Regular", Consolas, monospace;
    --up: var(--accent); --partial: var(--warn); --down: var(--danger); --unknown: var(--line);
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 18px 16px 40px; background: var(--bg); color: var(--text); font: 16px/1.5 var(--font); }
  main { max-width: 1100px; margin: 0 auto; display: grid; gap: var(--gap); }
  header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 6px 14px; }
  header h1 { font-size: 22px; margin: 0; letter-spacing: .02em; }
  header .spacer { flex: 1; }
  .sub, .hint, .dim { color: var(--muted); font-size: 14px; }
  .mono { font-family: var(--mono); }
  .tabs { display: flex; gap: 4px; overflow-x: auto; border-bottom: 1px solid var(--line); scrollbar-width: none; }
  .tabs button { flex: 0 0 auto; background: none; border: none; border-bottom: 2px solid transparent; border-radius: 0;
                 color: var(--muted); font: inherit; font-size: 15px; padding: 8px 12px; margin-bottom: -1px; cursor: pointer; }
  .tabs button:hover { color: var(--text); }
  .tabs button[aria-selected="true"] { color: var(--accent); border-bottom-color: var(--accent); font-weight: 600; }
  section { background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); padding: 16px; }
  h2 { font-size: 13px; text-transform: uppercase; letter-spacing: .12em; color: var(--muted); margin: 0 0 10px; }
  h3.sub-h { font-size: 15px; margin: 16px 0 6px; color: var(--text); }
  .src { color: var(--muted); font-size: 13px; margin: -4px 0 10px; }
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
  .pill.flat { background: none; border: 1px solid currentColor; color: var(--muted); font-weight: 500; }
  dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 12px; margin: 10px 0 0; font-size: 14px; }
  dt { color: var(--muted); }
  dd { margin: 0; overflow-wrap: anywhere; }
  .gps { color: var(--accent); font-weight: 600; }
  .warntext { color: var(--warn); }
  .dangertext { color: var(--danger); }
  .oktext { color: var(--accent); }
  .table { overflow-x: auto; }
  table { width: 100%; border-collapse: collapse; font-size: 15px; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); }
  th { color: var(--muted); font-weight: 500; font-size: 13px; }
  td { font-variant-numeric: tabular-nums; }
  tr:last-child td { border-bottom: 0; }
  .num { text-align: right; font-family: var(--mono); font-size: 14px; }
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
  button.act { padding: 7px 13px; border-radius: 8px; border: 1px solid var(--accent); background: var(--accent);
               color: var(--accent-ink); font: 600 14px var(--font); cursor: pointer; }
  button.act.ghost { background: transparent; color: var(--accent); }
  button.act:disabled { opacity: .5; cursor: default; }
  input.pw { width: 100%; max-width: 340px; padding: 10px 12px; border-radius: 8px; border: 1px solid var(--line);
             background: var(--bg); color: var(--text); font: inherit; }
  .row { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin-top: 10px; }
  .msg { margin-top: 10px; font-size: 14px; min-height: 1.2em; }
  .msg.err { color: var(--danger); } .msg.ok { color: var(--accent); }
  .loc-map { position: relative; height: 320px; margin-top: 10px; overflow: hidden; border: 1px solid var(--line);
             border-radius: var(--radius); background: var(--bg); }
  .loc-tiles { position: absolute; inset: 0; }
  .loc-tiles img { position: absolute; width: 256px; height: 256px; }
  .loc-pins { position: absolute; inset: 0; }
  .loc-pin { position: absolute; width: 16px; height: 16px; margin: -8px 0 0 -8px; border-radius: 50%;
             background: var(--danger); border: 3px solid #fff; box-shadow: 0 0 0 2px var(--danger); }
  .loc-pin.hh { width: 12px; height: 12px; margin: -6px 0 0 -6px; border-width: 2px; background: var(--accent);
                box-shadow: 0 0 0 2px var(--accent-ink); }
  .loc-label { position: absolute; transform: translate(-50%, 10px); padding: 0 5px; border-radius: 4px; white-space: nowrap;
               font-size: 12px; font-weight: 600; color: var(--text); background: var(--surface); border: 1px solid var(--line); }
  .loc-credit { position: absolute; right: 0; bottom: 0; padding: 1px 6px; font-size: 0.7rem; color: #333;
                background: rgba(255, 255, 255, 0.8); }
  .loc-credit a { color: #333; }
  .plan { width: 100%; height: 320px; display: block; }
  .plan text { fill: var(--text); font: 12px var(--font); }
  .plan .lab { font-weight: 600; }
  .plan .ring, .plan .axis { stroke: var(--line); fill: none; }
  .plan .scale { stroke: var(--muted); }
  .plan .dim { fill: var(--muted); font-size: 11px; }
  .copybox { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin-top: 8px; }
  .copybox code { flex: 1; min-width: 240px; background: var(--bg); border: 1px solid var(--line); border-radius: 8px;
                  padding: 8px 10px; font: 13px var(--mono); overflow-wrap: anywhere; user-select: all; }
  .avail { display: grid; grid-template-columns: auto 1fr auto; gap: 6px 10px; align-items: center; }
  .av0 { fill: var(--unknown); } .av1 { fill: var(--up); } .av2 { fill: var(--partial); } .av3 { fill: var(--down); }
  .avail .sum { font: 13px var(--mono); color: var(--muted); text-align: right; white-space: nowrap; }
  .avail .axis { display: flex; justify-content: space-between; font-size: 12px; color: var(--muted); }
  .avail svg { width: 100%; height: 18px; display: block; }
  .legend { display: flex; flex-wrap: wrap; gap: 12px; font-size: 13px; color: var(--muted); margin-top: 8px; }
  .legend i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 5px; vertical-align: -1px; }
  .spark { width: 100%; height: 34px; display: block; margin: 4px 0 2px; }
  .spark path { fill: none; stroke: var(--accent); stroke-width: 1.5; }
  .spark rect.drop { fill: var(--danger); }
  .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; }
  .stat .k { color: var(--muted); font-size: 13px; } .stat .v { font: 600 18px var(--mono); }
  .notes { margin: 0; padding-left: 18px; font-size: 14px; }
  .notes li { margin: 2px 0; }
  [hidden] { display: none !important; }
  @media (max-width: 560px) { .hide-sm { display: none; } }
</style>
</head>
<body>
<main>
  <header>
    <h1 id="title">LocalGrid &mdash; grid watch</h1>
    <span class="spacer"></span>
    <span class="sub">grid time <span class="mono" id="gt">&ndash;</span></span>
    <label class="toggle"><input type="checkbox" id="beep"> beep on alert</label>
    <button class="act ghost" id="refresh">Refresh</button>
    <button class="act ghost" id="logout" hidden>Log out</button>
  </header>
  <div class="sub" id="sub">An offline network, watched from this laptop without joining its Wi-Fi.</div>

  <nav class="tabs" id="tabs" role="tablist">
    <button role="tab" data-tab="overview">Overview</button>
    <button role="tab" data-tab="map">Map</button>
    <button role="tab" data-tab="network">Network</button>
    <button role="tab" data-tab="handhelds">Handhelds</button>
    <button role="tab" data-tab="traffic">Traffic</button>
  </nav>

  <!-- The admin password: typed here, held in memory by grid_watch.py on this laptop only. -->
  <section id="login" hidden>
    <h2>Admin password</h2>
    <p class="hint" id="login-why" style="margin-top:0">Log in to see positions, groups, availability, what happened to the APs, and traffic. The beacon view above needs no password.</p>
    <form id="login-form">
      <input class="pw" id="login-password" type="password" autocomplete="current-password" placeholder="Admin password">
      <div class="row"><button class="act" type="submit">Log in</button></div>
      <div class="msg" id="login-msg"></div>
    </form>
    <p class="hint">The password is sent to this tool over 127.0.0.1, kept in memory while it runs, and never written to disk or to the log. Only a proof of it crosses the air; the AP never receives the password itself.</p>
  </section>

  <div class="banner none" id="banner" data-tab="overview">No alert heard.</div>

  <section data-tab="overview" id="traffic-brief" data-empty="1" hidden>
    <h2>Traffic now</h2>
    <div class="src" id="traffic-brief-src"></div>
    <div class="stats" id="traffic-brief-stats"></div>
    <ul class="notes" id="traffic-brief-notes"></ul>
  </section>

  <section data-tab="overview">
    <h2>APs</h2>
    <div class="cards" id="aps"><div class="hint">Listening&hellip; no AP of this grid heard yet.</div></div>
  </section>
  <section data-tab="overview">
    <h2>Handhelds</h2>
    <table><thead><tr><th>Name</th><th>Online</th><th>AP</th><th>Battery</th></tr></thead>
      <tbody id="hh"><tr><td colspan="4" class="hint">None heard yet.</td></tr></tbody></table>
  </section>
  <section data-tab="overview">
    <h2>Events</h2>
    <div class="log" id="log"><div class="hint">Nothing yet.</div></div>
    <p class="hint" id="stats"></p>
  </section>

  <!-- Map -->
  <section data-tab="map" id="map-card">
    <h2>Map</h2>
    <div class="src" id="map-src"></div>
    <p class="hint" id="map-empty">No positions yet. MAIN appears here once its GPS has a fix, and a handheld once its own GPS does.</p>
    <div id="map-body" hidden>
      <p class="hint" id="loc-ref" style="margin-top:0"></p>
      <div class="loc-map" id="loc-map">
        <div class="loc-tiles" id="loc-tiles"></div>
        <div class="loc-pins" id="loc-pins"></div>
        <div class="loc-credit">&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener noreferrer">OpenStreetMap</a> contributors</div>
      </div>
      <svg class="plan" id="loc-plan" viewBox="0 0 600 320" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Plan of where everyone is" hidden></svg>
      <p class="hint" id="map-mode"></p>
      <div class="copybox"><code id="loc-link"></code><button class="act ghost" id="loc-copy">Copy link</button></div>
      <div class="msg" id="loc-msg"></div>
      <div class="table">
        <table>
          <thead><tr><th>Name</th><th>Coordinates</th><th>From MAIN</th><th>Fix</th></tr></thead>
          <tbody id="loc-body"></tbody>
        </table>
      </div>
      <p class="hint">Positions stay in memory on the APs and handhelds, never in flash, and are shown only here, behind the login. This laptop never sends a coordinate anywhere: map tiles are the only thing fetched, and a tile is asked for by its grid square.</p>
    </div>
  </section>

  <!-- Network -->
  <section data-tab="network" id="nodes-card">
    <h2>APs</h2>
    <div class="src" id="nodes-src"></div>
    <div class="table"><table>
      <thead><tr><th>AP</th><th>Status</th><th>Up for</th><th>Grid time</th><th>Last time sync</th><th>Last restart</th><th class="hide-sm">Signal</th></tr></thead>
      <tbody id="nodes-body"></tbody>
    </table></div>
    <p class="hint" id="nodes-empty">Log in and this fills in from the AP's own record of the grid.</p>
  </section>
  <section data-tab="network" id="avail-card" data-empty="1" hidden>
    <h2>Availability, last 2 hours</h2>
    <div class="avail" id="avail"></div>
    <div class="legend">
      <span><i style="background:var(--up)"></i>reachable</span>
      <span><i style="background:var(--partial)"></i>part of the minute</span>
      <span><i style="background:var(--down)"></i>unreachable</span>
      <span><i style="background:var(--unknown)"></i>no record (before this AP started)</span>
    </div>
    <p class="hint" id="avail-hint"></p>
  </section>
  <section data-tab="network" id="incidents-card" data-empty="1" hidden>
    <h2>What happened to the APs</h2>
    <p class="hint" id="incidents-empty">No AP outage on record. The log survives restarts and is shared between APs.</p>
    <div class="table" id="incidents-table" hidden><table>
      <thead><tr><th>When</th><th>AP</th><th>Down for</th><th>What happened</th><th class="hide-sm">Seen by</th></tr></thead>
      <tbody id="incidents-body"></tbody>
    </table></div>
  </section>

  <!-- Handhelds -->
  <section data-tab="handhelds" id="groups-card">
    <h2>Groups</h2>
    <div class="src" id="groups-src"></div>
    <p class="hint" style="margin:0 0 10px">Groups are shared by every AP and handheld. This tool only watches: groups are made and changed on a handheld or on the AP's admin page.</p>
    <p class="hint" id="groups-empty">Log in to see the groups.</p>
    <div class="table" id="groups-table" hidden><table>
      <thead><tr><th>Name</th><th>Members</th></tr></thead>
      <tbody id="groups-body"></tbody>
    </table></div>
  </section>
  <section data-tab="handhelds" id="announce-card" data-empty="1" hidden>
    <h2>Announcements</h2>
    <p class="hint" style="margin:0 0 10px">An announcement goes to every handheld and takes over its screen, so only these people may send one. Urgent messages are never limited: anyone can always raise an emergency.</p>
    <p id="announce-who"></p>
  </section>
  <section data-tab="handhelds" id="devices-card">
    <h2>Handhelds</h2>
    <div class="src" id="devices-src"></div>
    <p class="hint" id="devices-empty">Log in to see every handheld the grid has seen, including the ones out of this laptop's range.</p>
    <div class="table" id="devices-table" hidden><table>
      <thead><tr><th>Name</th><th>Status</th><th>Connected to</th><th>Battery</th></tr></thead>
      <tbody id="devices-body"></tbody>
    </table></div>
  </section>

  <!-- Traffic -->
  <section data-tab="traffic" id="traffic-card">
    <h2>Traffic and performance</h2>
    <div class="src" id="traffic-src"></div>
    <p class="hint" id="traffic-empty">Log in, then open this tab: the counters are asked for while it is open, once every 30 seconds, so the APs are left alone the rest of the time.</p>
    <ul class="notes" id="traffic-notes"></ul>
    <div id="traffic-body"></div>
  </section>
</main>
<script>
"use strict";
const $ = id => document.getElementById(id);
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
function svgEl(tag, attrs) { const e = document.createElementNS("http://www.w3.org/2000/svg", tag); for (const k in (attrs || {})) e.setAttribute(k, attrs[k]); return e; }
function ago(s) { if (s == null) return "never"; if (s < 60) return s + " s ago"; if (s < 3600) return Math.floor(s / 60) + " min ago"; return Math.floor(s / 3600) + " h " + Math.floor(s % 3600 / 60) + " min ago"; }
function row(dl, k, v, cls) { dl.append(el("dt", "", k)); const d = el("dd", cls || ""); if (v instanceof Node) d.append(v); else d.textContent = v; dl.append(d); }
function fmtDur(s) { if (s == null || s < 0) return "--"; const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); if (d) return d + " d " + h + " h"; if (h) return h + " h " + m + " min"; if (m) return m + " min"; return Math.round(s) + " s"; }
function fmtKB(b) { return b == null ? "--" : b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.round(b / 1024) + " KB"; }
function fmtBytes(b) { return b == null ? "--" : b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : b >= 1024 ? Math.round(b / 1024) + " KB" : b + " B"; }
function cell(tr, text, cls) { tr.append(el("td", cls || "", text)); return tr; }
const TABS = ["overview", "map", "network", "handhelds", "traffic"];
const TIME_QUALITY_WORDS = ["not set", "carried", "set here"];

/* ---- which tab is open ---- */
let tab = TABS.includes(location.hash.slice(1)) ? location.hash.slice(1) : "overview";
function applyTabs(loggedIn) {
  for (const b of $("tabs").querySelectorAll("button")) b.setAttribute("aria-selected", String(b.dataset.tab === tab));
  /* Only the cards, never the tab buttons themselves (they carry data-tab too). */
  for (const sec of document.querySelectorAll("main > [data-tab]")) sec.hidden = sec.dataset.tab !== tab || sec.dataset.empty === "1";
  $("login").hidden = loggedIn;
  $("logout").hidden = !loggedIn;
}
$("tabs").addEventListener("click", ev => {
  const b = ev.target.closest("button[data-tab]");
  if (!b) return;
  tab = b.dataset.tab;
  try { history.replaceState(null, "", "#" + tab); } catch (e) {}
  applyTabs(lastLoggedIn);
  if (tab === "traffic") ask(true);
});

/* ---- the beacon layer: alerts, AP health, who is online, batteries (D68) ---- */
let beepOn = false, audio = null, lastAlertId = null, failures = 0, lastLoggedIn = false;
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

function batteryCell(pct) {
  const td = el("td");
  if (pct == null) { td.append(el("span", "hint", "–")); return td; }
  const bar = el("span", "bar"); const i = el("i", pct < 20 ? "low" : (pct < 40 ? "mid" : ""));
  i.style.width = pct + "%"; bar.append(i); td.append(bar, document.createTextNode(pct + "%"));
  return td;
}

function renderHandhelds(list) {
  const tb = $("hh"); tb.replaceChildren();
  if (!list.length) { const tr = el("tr"); const td = el("td", "hint", "None heard yet."); td.colSpan = 4; tr.append(td); tb.append(tr); return; }
  for (const h of list) {
    const tr = el("tr");
    tr.append(el("td", "", h.name));
    tr.append(el("td", h.online ? "" : "hint", h.online ? "online" : "offline"));
    tr.append(el("td", "", h.ap_name || "–"));
    tr.append(batteryCell(h.battery));
    tb.append(tr);
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

/* ---- the admin link: logging in, refreshing, and where each section came from (D70) ---- */
async function post(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" },
                                body: JSON.stringify(body || {}), cache: "no-store" });
  return r.json().catch(() => ({}));
}
function ask(traffic) { post("/refresh", { traffic: !!traffic || tab === "traffic" }).catch(() => {}); }
$("refresh").addEventListener("click", () => { ask(tab === "traffic"); $("refresh").disabled = true; setTimeout(() => { $("refresh").disabled = false; }, 2000); });
$("logout").addEventListener("click", async () => { await post("/logout"); poll(); });
$("login-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const box = $("login-password"), pw = box.value;
  box.value = "";                                 /* the password does not linger in the page */
  if (!pw) return;
  $("login-msg").className = "msg ok";
  $("login-msg").textContent = "Checking with an AP…";
  await post("/login", { password: pw });
  poll();
});
function source(id, sec) {
  const e = $(id);
  if (!sec) { e.textContent = ""; return; }
  e.textContent = "From " + sec.ap + ", fetched " + ago(sec.at_age_s) + ".";
}

/* ---- Map (D65, D70): tiles when this laptop has Internet, a drawn plan when it has not ---- */
function worldPx(lat, lon, z) {
  const n = 256 * 2 ** z, r = lat * Math.PI / 180;
  return { x: (lon + 180) / 360 * n, y: (1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * n };
}
function distBearing(a, b) {
  const R = 6371000, rad = Math.PI / 180;
  const f1 = a.lat * rad, f2 = b.lat * rad, df = (b.lat - a.lat) * rad, dl = (b.lon - a.lon) * rad;
  const h = Math.sin(df / 2) ** 2 + Math.cos(f1) * Math.cos(f2) * Math.sin(dl / 2) ** 2;
  const d = 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
  const brg = Math.atan2(Math.sin(dl) * Math.cos(f2), Math.cos(f1) * Math.sin(f2) - Math.sin(f1) * Math.cos(f2) * Math.cos(dl));
  return { d, brg: (brg / rad + 360) % 360 };
}
const COMPASS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
const fmtDist = m => m < 1000 ? Math.round(m) + " m" : (m / 1000).toFixed(m < 10000 ? 2 : 1) + " km";
const fmtAge = sec => sec < 0 ? "--" : sec < 60 ? "just now" : sec < 3600 ? Math.floor(sec / 60) + " min ago"
  : sec < 86400 ? Math.floor(sec / 3600) + " h ago" : Math.floor(sec / 86400) + " d ago";
function dms(v, pos, neg) {
  const a = Math.abs(v), d = Math.floor(a), mf = (a - d) * 60, m = Math.floor(mf), sec = ((mf - m) * 60).toFixed(1);
  return d + "° " + m + "' " + sec + '" ' + (v >= 0 ? pos : neg);
}

let mapKey = "", tilesWork = null;
/* Tiles come through this tool (/tile/z/x/y.png), which asks OpenStreetMap with a proper
   User-Agent and fetches only the squares this map shows. No coordinate is ever in a URL. */
function drawTiles(pins) {
  const box = $("loc-tiles"), layer = $("loc-pins");
  const w = box.clientWidth || 600, h = box.clientHeight || 320, margin = 46;
  let z = 17, lo, hi;
  for (; z >= 10; z--) {
    const pts = pins.map(p => worldPx(p.lat, p.lon, z));
    lo = { x: Math.min(...pts.map(p => p.x)), y: Math.min(...pts.map(p => p.y)) };
    hi = { x: Math.max(...pts.map(p => p.x)), y: Math.max(...pts.map(p => p.y)) };
    if (hi.x - lo.x <= w - 2 * margin && hi.y - lo.y <= h - 2 * margin) break;
  }
  if (z < 10) z = 10;
  const cx = (lo.x + hi.x) / 2, cy = (lo.y + hi.y) / 2, n = 2 ** z;
  const left = cx - w / 2, top = cy - h / 2;
  box.replaceChildren();
  let wanted = 0, loaded = 0, failed = 0;
  const settle = () => {
    if (loaded + failed < wanted) return;
    tilesWork = loaded > 0;
    showMapMode(pins);
  };
  for (let ty = Math.floor(top / 256); ty <= Math.floor((top + h) / 256); ty++) {
    if (ty < 0 || ty >= n) continue;
    for (let tx = Math.floor(left / 256); tx <= Math.floor((left + w) / 256); tx++) {
      const img = new Image();
      img.alt = "";
      img.style.left = Math.round(tx * 256 - left) + "px";
      img.style.top = Math.round(ty * 256 - top) + "px";
      img.onload = () => { loaded++; settle(); };
      img.onerror = () => { failed++; settle(); };
      img.src = "/tile/" + z + "/" + (((tx % n) + n) % n) + "/" + ty + ".png";
      wanted++;
      box.appendChild(img);
    }
  }
  if (!wanted) { tilesWork = false; showMapMode(pins); }
  layer.replaceChildren();
  for (const p of [...pins].sort((a, b) => a.ap - b.ap)) {
    const q = worldPx(p.lat, p.lon, z), x = Math.round(q.x - left) + "px", y = Math.round(q.y - top) + "px";
    const dot = el("div", p.ap ? "loc-pin" : "loc-pin hh");
    dot.style.left = x; dot.style.top = y; dot.title = p.name;
    const label = el("div", "loc-label", p.name);
    label.style.left = x; label.style.top = y;
    layer.append(dot, label);
  }
}

/* No Internet, or the tiles did not come: a plan drawn here, with distances and bearings. */
function drawPlan(pins, main) {
  const svg = $("loc-plan");
  svg.replaceChildren();
  const W = 600, H = 320, cx = W / 2, cy = H / 2;
  const ref = main || pins[0];
  const pts = pins.map(p => {
    const db = distBearing(ref, p);
    const a = db.brg * Math.PI / 180;
    return { p, dx: db.d * Math.sin(a), dy: -db.d * Math.cos(a), d: db.d, brg: db.brg };
  });
  const span = Math.max(20, ...pts.map(q => Math.hypot(q.dx, q.dy)));
  const scale = (Math.min(W, H) / 2 - 50) / span;                  /* pixels per metre */
  for (const r of [0.25, 0.5, 1]) {
    svg.append(svgEl("circle", { cx, cy, r: span * scale * r, class: "ring" }));
    svg.append(Object.assign(svgEl("text", { x: cx + 3, y: cy - span * scale * r + 12, class: "dim" }),
      { textContent: fmtDist(span * r) }));
  }
  svg.append(svgEl("line", { x1: cx, y1: 18, x2: cx, y2: H - 18, class: "axis" }));
  svg.append(svgEl("line", { x1: 18, y1: cy, x2: W - 18, y2: cy, class: "axis" }));
  svg.append(Object.assign(svgEl("text", { x: cx + 5, y: 16, class: "dim" }), { textContent: "N" }));
  for (const q of pts.sort((a, b) => a.p.ap - b.p.ap)) {
    const x = cx + q.dx * scale, y = cy + q.dy * scale;
    svg.append(svgEl("circle", { cx: x, cy: y, r: q.p.ap ? 7 : 5,
      fill: q.p.ap ? "var(--danger)" : "var(--accent)", stroke: "var(--bg)", "stroke-width": 2 }));
    const t = svgEl("text", { x: x + 10, y: y + 4, class: "lab" });
    t.textContent = q.p.name + (q.d < 5 ? "" : " · " + fmtDist(q.d) + " " + COMPASS[Math.round(q.brg / 45) % 8]);
    svg.append(t);
  }
  const t = svgEl("text", { x: 14, y: H - 10, class: "dim" });
  t.textContent = "Drawn on this laptop: everyone's place relative to " + ref.name + ", north up.";
  svg.append(t);
}

function showMapMode(pins) {
  const online = tilesWork === true;
  $("loc-map").hidden = !online;
  $("loc-plan").hidden = online;
  $("map-mode").textContent = online
    ? "Map tiles from OpenStreetMap, fetched by this tool for the squares shown. The grid itself has no Internet and never sees them."
    : "No map tiles: this laptop has no Internet, or the tile server did not answer. The plan below is drawn here from the coordinates, with distances and bearings; coordinates and the link work either way.";
}

function renderMap(status, sec) {
  source("map-src", sec);
  if (!status) {
    $("map-body").hidden = true;
    $("map-empty").hidden = false;
    $("map-empty").textContent = lastLoggedIn
      ? "Waiting for the first answer from an AP…"
      : "Log in to see positions. Without the admin password this tool shows the beacon only, which never carries a coordinate.";
    return;
  }
  const g = status.gps || {};
  const pins = (status.positions || []).map(p => ({
    ap: !!p.ap, id: p.subject, lat: p.lat_u / 1e6, lon: p.lon_u / 1e6, fix_time: p.fix_time, sats: p.sats,
    name: p.name || (p.ap ? apLabel(p.subject) : ("Handheld " + p.subject)),
  }));
  if (g.pos) {
    const own = pins.find(p => p.ap && p.id === status.node);
    const live = { ap: true, id: status.node, lat: g.lat_u / 1e6, lon: g.lon_u / 1e6, sats: g.sats,
                   name: status.node_name, fix_time: g.fix ? 0 : (own ? own.fix_time : -1), live: g.fix };
    if (own) Object.assign(own, live); else pins.push(live);
  }
  if (!pins.length) {
    $("map-body").hidden = true; $("map-empty").hidden = false;
    $("map-empty").textContent = "No positions yet. MAIN appears here once its GPS has a fix, and a handheld once its own GPS does.";
    return;
  }
  $("map-body").hidden = false; $("map-empty").hidden = true;
  const now = status.grid_time || Math.floor(Date.now() / 1000);
  const main = pins.find(p => p.ap && p.id === 0);
  if (main) {
    const la = main.lat.toFixed(6), lo = main.lon.toFixed(6);
    $("loc-ref").textContent = main.name + ": " + la + ", " + lo + " · " + dms(main.lat, "N", "S") + ", " + dms(main.lon, "E", "W") +
      " · " + (main.live ? "GPS fix, " + main.sats + " satellites" : "fix " + fmtAge(Math.round(now - main.fix_time)) + ", " + main.sats + " satellites");
    /* Text, not a link: nothing on this page navigates anywhere with a coordinate in it. */
    $("loc-link").textContent = "https://www.openstreetmap.org/?mlat=" + la + "&mlon=" + lo + "#map=17/" + la + "/" + lo;
  } else {
    $("loc-ref").textContent = "Where MAIN is is not known: it has no GPS fix, or this AP has not heard it yet.";
    $("loc-link").textContent = "";
  }
  const key = tab + ";" + ($("loc-tiles").clientWidth) + ";" + pins.map(p => (p.ap ? "a" : "h") + p.id + "@" + p.lat.toFixed(4) + "," + p.lon.toFixed(4) + ":" + p.name).sort().join(";");
  if (key !== mapKey) {
    mapKey = key;
    drawPlan(pins, main);
    if (tilesEnabled && tilesOnline !== false) drawTiles(pins); else { tilesWork = false; showMapMode(pins); }
  }
  const body = $("loc-body"); body.replaceChildren();
  for (const p of [...pins].sort((a, b) => (b.ap - a.ap) || (a.id - b.id))) {
    const tr = el("tr");
    const nm = el("td", "", p.name);
    if (p.ap) nm.append(el("span", "dim", " AP"));
    tr.append(nm);
    cell(tr, p.lat.toFixed(5) + ", " + p.lon.toFixed(5), "mono");
    let from = "--";
    if (main && p === main) from = "MAIN";
    else if (main) { const db = distBearing(main, p); from = db.d < 5 ? "here" : fmtDist(db.d) + " " + COMPASS[Math.round(db.brg / 45) % 8]; }
    cell(tr, from);
    cell(tr, p.live ? "live" : fmtAge(p.fix_time > 0 ? Math.round(now - p.fix_time) : -1));
    body.append(tr);
  }
}

$("loc-copy").addEventListener("click", async () => {
  const text = $("loc-link").textContent;
  if (!text) return;
  try { await navigator.clipboard.writeText(text); } catch (e) {
    const t = document.createElement("textarea");
    t.value = text; document.body.append(t); t.select();
    try { document.execCommand("copy"); } catch (e2) {}
    t.remove();
  }
  $("loc-msg").className = "msg ok";
  $("loc-msg").textContent = "Link copied. Open it on a device with Internet later.";
  setTimeout(() => { $("loc-msg").textContent = ""; }, 2500);
});

/* ---- Network: the APs' own record, from HISTORY ---- */
function apLabel(ap) { return lastHistory ? apName(lastHistory, ap) : "AP " + ap; }
function apName(hist, ap) {
  const a = (hist.aps || []).find(x => x.ap === ap);
  return a && a.name ? a.name : "AP " + ap;
}

function renderNetwork(status, hist, sec) {
  source("nodes-src", sec);
  const body = $("nodes-body"); body.replaceChildren();
  if (!hist) {
    $("nodes-empty").hidden = false;
    $("nodes-empty").textContent = lastLoggedIn ? "Waiting for the first answer from an AP…"
      : "Log in to see every AP the grid knows, its availability, and what happened to it.";
    $("avail-card").dataset.empty = $("incidents-card").dataset.empty = "1";
    applyTabs(lastLoggedIn);
    return;
  }
  $("nodes-empty").hidden = true;
  $("avail-card").dataset.empty = $("incidents-card").dataset.empty = "0";
  const links = {};
  for (const l of ((status && status.links) || [])) links[l.node] = l;
  for (const a of hist.aps) {
    const tr = el("tr");
    cell(tr, a.name || apName(hist, a.ap));
    const st = el("td"), l = links[a.ap];
    let signal = el("span", "dim", "this AP");
    if (a.self) st.append(el("span", "pill ok", "YOU ARE HERE"));
    else if (l && l.up) {
      st.append(el("span", "pill ok", "ONLINE"));
      signal = el("span", l.rssi >= -60 ? "oktext" : l.rssi >= -75 ? "warntext" : "dangertext", l.rssi + " dBm");
    } else {
      st.append(el("span", "pill bad", "UNREACHABLE"));
      signal = el("span", "dim", "last heard " + fmtDur(a.heard_s) + " ago");
    }
    tr.append(st);
    const reachable = a.self || (l && l.up);
    cell(tr, reachable ? fmtDur(a.uptime_s) : "--");
    cell(tr, TIME_QUALITY_WORDS[a.time_quality] || "unknown");
    cell(tr, a.sync_age_s < 0 ? "never"
      : a.sync_from === a.ap ? "set here " + fmtDur(a.sync_age_s) + " ago"
      : "from " + apName(hist, a.sync_from) + ", " + fmtDur(a.sync_age_s) + " ago" +
        (a.sync_drift_ms ? " (" + (a.sync_drift_ms > 0 ? "+" : "") + a.sync_drift_ms + " ms)" : ""));
    cell(tr, a.reset + (a.prev_run_s >= 0 ? ", after " + fmtDur(a.prev_run_s) : ""), "dim");
    const sg = el("td", "hide-sm"); sg.append(signal); tr.append(sg);
    body.append(tr);
  }

  const av = $("avail"); av.replaceChildren();
  for (const a of hist.aps) {
    av.append(el("div", "", a.name || apName(hist, a.ap)));
    const svg = svgEl("svg", { viewBox: "0 0 120 10", preserveAspectRatio: "none", role: "img",
                               "aria-label": (a.name || "AP") + " availability" });
    (a.avail || []).forEach((c, i) => {
      const r = svgEl("rect", { x: i, y: 0, width: 0.92, height: 10, class: "av" + (c & 3) });
      const t = svgEl("title"); t.textContent = (120 - i) + " min ago";
      r.append(t); svg.append(r);
    });
    av.append(svg);
    const rec = (a.avail || []).filter(c => c !== 0).length;
    const upMin = (a.avail || []).reduce((t, c) => t + (c === 1 ? 1 : c === 2 ? 0.5 : 0), 0);
    av.append(el("div", "sum", rec ? Math.round(100 * upMin / rec) + "% of " + rec + " min" : "no record"));
  }
  if (hist.aps.length) {
    av.append(el("div"));
    const axis = el("div", "axis");
    axis.append(el("span", "", "2 h ago"), el("span", "", "1 h"), el("span", "", "now"));
    av.append(axis, el("div"));
  }
  const from = (hist.aps.find(a => a.self) || {}).name || "the AP asked";
  $("avail-hint").textContent = "As seen from " + from + ": whether each AP was reachable over the AP-to-AP link, minute by minute. " +
    "The history survives restarts and unplugging; minutes " + from + " was down are filled in from the other APs' records.";

  const inc = (hist.incidents || []).slice().sort((a, b) => (b.down_grid_time || 0) - (a.down_grid_time || 0));
  const tb = $("incidents-body"); tb.replaceChildren();
  for (const i of inc) {
    const tr = el("tr");
    cell(tr, i.down_grid_time ? new Date(i.down_grid_time * 1000).toLocaleString() : "grid time was not set");
    cell(tr, apName(hist, i.ap));
    cell(tr, fmtDur(i.duration_s));
    const what = i.kind === "restarted" ? "Restarted: " + i.reset + (i.prev_run_s >= 0 ? " after running " + fmtDur(i.prev_run_s) : "")
      : i.kind === "link" ? "Kept running; the AP-to-AP link dropped (radio or distance)"
      : i.kind === "unexplained" ? "Came back but did not say why within a minute" : "Unreachable now";
    cell(tr, what, i.kind === "down" ? "dangertext" : "");
    cell(tr, apName(hist, i.seen_by), "dim hide-sm");
    tb.append(tr);
  }
  $("incidents-table").hidden = inc.length === 0;
  $("incidents-empty").hidden = inc.length > 0;
}

/* ---- Handhelds: groups, who may announce, every handheld the grid has seen ---- */
function renderHandheldTab(status, sec, beaconById) {
  source("groups-src", sec); source("devices-src", sec);
  if (!status) {
    $("groups-empty").hidden = false;
    $("groups-empty").textContent = lastLoggedIn ? "Waiting for the first answer from an AP…" : "Log in to see the groups.";
    $("groups-table").hidden = true;
    $("announce-card").dataset.empty = "1";
    $("devices-empty").hidden = false;
    $("devices-table").hidden = true;
    applyTabs(lastLoggedIn);
    return;
  }
  $("announce-card").dataset.empty = "0";
  const names = {};
  for (const u of (status.users || [])) names[u.device] = u.name;
  const groups = status.groups || [];
  const gb = $("groups-body"); gb.replaceChildren();
  for (const g of groups) {
    const tr = el("tr");
    cell(tr, g.name);
    cell(tr, (g.members || []).map(d => names[d] || ("Handheld " + d)).join(", ") || "nobody");
    gb.append(tr);
  }
  $("groups-table").hidden = groups.length === 0;
  $("groups-empty").hidden = groups.length > 0;
  $("groups-empty").textContent = "No groups yet.";
  $("announce-who").textContent = status.announce_all !== false
    ? "Everyone may send an announcement, including handhelds the grid has not seen yet."
    : "May announce: " + ((status.announcers || []).map(d => names[d] || ("Handheld " + d)).join(", ") || "nobody");

  const db = $("devices-body"); db.replaceChildren();
  for (const d of (status.devices || [])) {
    const tr = el("tr");
    cell(tr, d.name || ("Handheld " + d.device));
    const st = el("td");
    st.append(el("span", "pill " + (d.state === "ONLINE" ? "ok" : "flat"), d.state));
    tr.append(st);
    cell(tr, d.state === "ONLINE" && d.node >= 0 ? (apNameFromStatus(status, d.node)) : "--");
    const b = beaconById[d.device];
    tr.append(batteryCell(b ? b.battery : null));
    db.append(tr);
  }
  $("devices-table").hidden = (status.devices || []).length === 0;
  $("devices-empty").hidden = (status.devices || []).length > 0;
  $("devices-empty").textContent = "No handhelds have connected yet.";
}
function apNameFromStatus(status, node) {
  return node === status.node ? status.node_name : (lastHistory ? apName(lastHistory, node) : "AP " + node);
}

/* ---- Traffic and performance: counts and sizes only, never a message or a sound ---- */
/* Limits: crossing one of these is worth saying out loud, in words, in red. */
const LIM = {
  heapWarnKB: 40, heapBadKB: 20, queueWarn: 8, queueBad: 16, loopWarnMs: 100, loopBadMs: 500,
  sendWaitWarnMs: 2000, cpuWarn: 85, linkFailPct: 5, stackLowB: 1024,
};
const CLASS_WORDS = {
  direct: "1:1 text", group: "group text", broadcast: "broadcast and urgent",
  voice: "voice (push to talk)", ack: "delivered and read reports", presence: "presence",
  announce: "groups, grid state, names", position: "positions", time: "time",
  other: "hellos, pings, diagnostics",
};
const FAULT_WORDS = {
  duplicate: "duplicates suppressed", table_full: "table full", unknown_recipient: "unknown recipient",
  decrypt_failed: "failed to decrypt", ttl_expired: "TTL expired", queue_full: "send queue full",
  send_timeout: "send timed out", voice_dropped: "voice frames dropped",
  malformed: "malformed messages", rejected: "rejected messages",
};
const FAULTS_THAT_MATTER = ["table_full", "decrypt_failed", "queue_full", "send_timeout", "voice_dropped"];
function bad(v) { return v ? "dangertext" : ""; }

function sparkline(buckets, bucketS) {
  const svg = svgEl("svg", { class: "spark", viewBox: "0 0 120 34", preserveAspectRatio: "none", role: "img",
                             "aria-label": "messages a minute, newest on the right" });
  const vals = buckets.map(b => b.messages);
  const max = Math.max(1, ...vals);
  const step = 120 / Math.max(1, vals.length - 1);
  let d = "";
  vals.forEach((v, i) => { d += (i ? "L" : "M") + (i * step).toFixed(1) + " " + (32 - 30 * v / max).toFixed(1) + " "; });
  svg.append(svgEl("path", { d }));
  const t = svgEl("title");
  t.textContent = "Messages in and out, one step every " + bucketS + " s; the newest is on the right, the busiest step was " + max + ".";
  svg.append(t);
  return svg;
}

function trafficNotes(t, name) {
  const out = [], f = t.faults, p = t.perf;
  if (f.voice_dropped > 0) out.push([name + " has dropped " + f.voice_dropped + " voice frames since it started: " +
    "push-to-talk sounds broken when it does. The AP drops voice rather than block the rest of the grid.", true]);
  if (f.queue_full > 0) out.push([name + "'s send queue filled " + f.queue_full + (f.queue_full === 1 ? " time" : " times") + ": it could not keep up with what it was asked to send.", true]);
  if (f.table_full > 0) out.push([name + " ran out of table room " + f.table_full + " times: messages were refused.", true]);
  if (f.send_timeout > 0) out.push([name + " timed out sending " + f.send_timeout + " times.", true]);
  if (f.decrypt_failed > 0) out.push([name + " could not decrypt " + f.decrypt_failed + " messages: an AP or handheld may be running different secrets.", true]);
  if (f.ttl_expired > 0) out.push([f.ttl_expired + " messages died of old age (TTL) at " + name + ": the path was too long or the grid was split.", false]);
  const junk = f.malformed + f.rejected;
  if (junk > 0) out.push([name + " threw away " + junk + (junk === 1 ? " message" : " messages") + " it could not make sense of.", false]);
  const heapKB = Math.round(p.heap_min / 1024);
  if (heapKB < LIM.heapBadKB) out.push([name + " has been down to " + heapKB + " KB of free memory: it is close to restarting itself.", true]);
  else if (heapKB < LIM.heapWarnKB) out.push([name + "'s lowest free memory was " + heapKB + " KB; watch it.", false]);
  if (p.queue_high >= LIM.queueBad) out.push([name + "'s core queue reached " + p.queue_high + " waiting messages: work is piling up.", true]);
  else if (p.queue_high >= LIM.queueWarn) out.push([name + "'s core queue reached " + p.queue_high + " waiting messages.", false]);
  if (p.loop_max_ms >= LIM.loopBadMs) out.push([name + " had a " + p.loop_max_ms + " ms pass through its main loop: something blocked it.", true]);
  else if (p.loop_max_ms >= LIM.loopWarnMs) out.push([name + "'s longest main-loop pass was " + p.loop_max_ms + " ms.", false]);
  if (Math.min(p.stack_core, p.stack_link) < LIM.stackLowB) out.push([name + " is down to " + Math.min(p.stack_core, p.stack_link) + " bytes of spare stack: a crash waiting to happen.", true]);
  if (p.cpu_busy != null && p.cpu_busy >= LIM.cpuWarn) out.push([name + " is " + p.cpu_busy + "% busy.", true]);
  if (p.espnow_errors > 0) out.push([name + " had " + p.espnow_errors + " ESP-NOW send errors.", false]);
  if (t.handhelds.slowest_send_ms >= LIM.sendWaitWarnMs) out.push([name + " kept a handheld waiting " + t.handhelds.slowest_send_ms + " ms to send.", false]);
  for (const l of t.links) {
    const pct = l.sent ? 100 * l.failures / l.sent : 0;
    if (!l.up) out.push(["The link from " + name + " to " + apLabel(l.ap) + " is down.", true]);
    else if (pct >= LIM.linkFailPct) out.push(["The link from " + name + " to " + apLabel(l.ap) + " is failing " + pct.toFixed(0) + "% of its sends.", true]);
  }
  return out;
}

function messageTable(messages, voiceDropped) {
  const table = el("table"), head = el("thead"), hr = el("tr");
  for (const h of ["Class", "In", "Out", "Relayed"]) hr.append(el("th", h === "Class" ? "" : "num", h));
  head.append(hr); table.append(head);
  const body = el("tbody");
  for (const cls of Object.keys(messages)) {
    const m = messages[cls];
    if (!m.in && !m.out && !m.relayed) continue;
    const tr = el("tr");
    const name = el("td", "", CLASS_WORDS[cls] || cls);
    if (cls === "voice" && voiceDropped) name.append(el("span", "dangertext", "  " + voiceDropped + " dropped"));
    tr.append(name);
    cell(tr, String(m.in), "num");
    cell(tr, String(m.out), "num");
    cell(tr, String(m.relayed), "num");
    body.append(tr);
  }
  table.append(body);
  return table;
}

function renderTraffic(list, sec) {
  source("traffic-src", sec);
  const box = $("traffic-body"); box.replaceChildren();
  const notes = $("traffic-notes"); notes.replaceChildren();
  if (!list || !list.length) {
    $("traffic-empty").hidden = false;
    $("traffic-empty").textContent = lastLoggedIn
      ? "Asking an AP for its counters… they arrive within a minute of this tab being open."
      : "Log in to see how much the grid is carrying and how well the APs are coping.";
    return;
  }
  $("traffic-empty").hidden = true;
  const all = [];
  for (const item of list) {
    const t = item.data, name = item.ap;
    for (const note of trafficNotes(t, name)) all.push(note);
    const card = el("div");
    card.append(el("h3", "sub-h", name + " · up " + fmtDur(t.uptime_s) + " · counters fetched " + ago(item.at_age_s)));
    const stats = el("div", "stats");
    const stat = (k, v, cls) => { const d = el("div", "stat"); d.append(el("div", "k", k), el("div", "v " + (cls || ""), v)); stats.append(d); };
    stat("Messages a minute", t.rate_1m == null ? "--" : String(t.rate_1m));
    stat("Over 5 minutes", t.rate_5m == null ? "--" : String(t.rate_5m));
    stat("Since it started", t.in_total + " in, " + t.out_total + " out");
    stat("Voice dropped", String(t.faults.voice_dropped), bad(t.faults.voice_dropped));
    card.append(stats);
    if (t.buckets.length) card.append(sparkline(t.buckets, Math.round(t.bucket_s)));

    card.append(el("h3", "sub-h", "Messages"), messageTable(t.messages, t.faults.voice_dropped));

    const ft = el("table"), fb = el("tbody");
    for (const k of Object.keys(t.faults)) {
      const v = t.faults[k];
      const tr = el("tr");
      cell(tr, FAULT_WORDS[k] || k);
      cell(tr, String(v), "num " + (v && FAULTS_THAT_MATTER.includes(k) ? "dangertext" : v ? "warntext" : "dim"));
      fb.append(tr);
    }
    ft.append(fb);
    card.append(el("h3", "sub-h", "Drops and faults"), ft);

    if (t.links.length) {
      const lt = el("table"), lhd = el("thead"), lh = el("tr");
      for (const h of ["Link to", "State", "Sent", "Received", "Failed", "Bytes out", "Bytes in", "Last heard"]) lh.append(el("th", "", h));
      lhd.append(lh); lt.append(lhd);
      const lb = el("tbody");
      for (const l of t.links) {
        const tr = el("tr");
        cell(tr, apLabel(l.ap));
        cell(tr, l.up ? "up, " + l.rssi + " dBm" : "down", l.up ? "oktext" : "dangertext");
        cell(tr, String(l.sent), "num");
        cell(tr, String(l.received), "num");
        cell(tr, String(l.failures), "num " + (l.sent && l.failures / l.sent * 100 >= LIM.linkFailPct ? "dangertext" : ""));
        cell(tr, fmtBytes(l.bytes_out), "num");
        cell(tr, fmtBytes(l.bytes_in), "num");
        cell(tr, l.heard_s == null ? "never" : fmtDur(l.heard_s) + " ago");
        lb.append(tr);
      }
      lt.append(lb);
      card.append(el("h3", "sub-h", "Backbone links"), lt);
    }

    const h = t.handhelds, hDl = el("dl");
    row(hDl, "Connected now", h.sessions + " session" + (h.sessions === 1 ? "" : "s") + ", " + h.registered + " registered");
    row(hDl, "Since it started", h.opened + " sessions, " + h.registrations + " registrations, " + h.disconnects + " disconnects");
    row(hDl, "Slowest send wait", h.slowest_send_ms + " ms", h.slowest_send_ms >= LIM.sendWaitWarnMs ? "warntext" : "");
    row(hDl, "Bytes", fmtBytes(h.bytes_in) + " in, " + fmtBytes(h.bytes_out) + " out");
    card.append(el("h3", "sub-h", "Handhelds on this AP"), hDl);

    const p = t.perf, pDl = el("dl");
    const heapKB = Math.round(p.heap_min / 1024);
    row(pDl, "Free memory", fmtKB(p.heap_free) + " now, lowest " + fmtKB(p.heap_min),
        heapKB < LIM.heapBadKB ? "dangertext" : heapKB < LIM.heapWarnKB ? "warntext" : "");
    row(pDl, "Largest free block", fmtKB(p.heap_largest));
    row(pDl, "Core queue", p.queue_depth + " waiting, highest " + p.queue_high,
        p.queue_high >= LIM.queueBad ? "dangertext" : p.queue_high >= LIM.queueWarn ? "warntext" : "");
    row(pDl, "Spare stack", "core " + fmtBytes(p.stack_core) + ", link " + fmtBytes(p.stack_link),
        Math.min(p.stack_core, p.stack_link) < LIM.stackLowB ? "dangertext" : "");
    row(pDl, "Main loop", "longest " + p.loop_max_ms + " ms, average " + (p.loop_avg_us / 1000).toFixed(2) + " ms",
        p.loop_max_ms >= LIM.loopBadMs ? "dangertext" : p.loop_max_ms >= LIM.loopWarnMs ? "warntext" : "");
    row(pDl, "Radio errors", "ESP-NOW " + p.espnow_errors + ", Wi-Fi side " + p.wifi_errors,
        bad(p.espnow_errors || p.wifi_errors));
    row(pDl, "CPU busy", p.cpu_busy == null ? "not measured" : p.cpu_busy + "%",
        p.cpu_busy != null && p.cpu_busy >= LIM.cpuWarn ? "dangertext" : "");
    card.append(el("h3", "sub-h", "This AP's performance"), pDl);
    box.append(card);
  }

  if (list.length > 1) {
    const total = {};
    let dropped = 0;
    for (const item of list) {
      dropped += item.data.faults.voice_dropped;
      for (const cls of Object.keys(item.data.messages)) {
        total[cls] = total[cls] || { in: 0, out: 0, relayed: 0 };
        for (const k of ["in", "out", "relayed"]) total[cls][k] += item.data.messages[cls][k];
      }
    }
    box.append(el("h3", "sub-h", "Across the " + list.length + " APs asked"), messageTable(total, dropped));
  }

  for (const [text, red] of all) notes.append(el("li", red ? "dangertext" : "warntext", text));
  if (!all.length) notes.append(el("li", "oktext", "Nothing over its limit: the APs are coping with what the grid is carrying."));

  /* The short version on the Overview. */
  const brief = list[0], bt = brief.data;
  $("traffic-brief").dataset.empty = "0";
  $("traffic-brief-src").textContent = "From " + brief.ap + ", fetched " + ago(brief.at_age_s) + ".";
  const bs = $("traffic-brief-stats"); bs.replaceChildren();
  const bstat = (k, v, cls) => { const d = el("div", "stat"); d.append(el("div", "k", k), el("div", "v " + (cls || ""), v)); bs.append(d); };
  bstat("Messages a minute", bt.rate_1m == null ? "--" : String(bt.rate_1m));
  bstat("Voice dropped", String(bt.faults.voice_dropped), bad(bt.faults.voice_dropped));
  bstat("Free memory", fmtKB(bt.perf.heap_free), bt.perf.heap_min < LIM.heapBadKB * 1024 ? "dangertext" : "");
  bstat("Handhelds connected", String(bt.handhelds.sessions));
  const bn = $("traffic-brief-notes"); bn.replaceChildren();
  for (const [text] of all.filter(x => x[1]).slice(0, 4)) bn.append(el("li", "dangertext", text));
}

/* ---- polling ---- */
let lastHistory = null, tilesEnabled = true, tilesOnline = null;

async function poll() {
  try {
    const r = await fetch("state.json?tab=" + tab, { cache: "no-store" });
    const s = await r.json();
    failures = 0;
    tilesEnabled = s.tiles.enabled; tilesOnline = s.tiles.online;
    const auth = s.auth || {}, link = s.link || {};
    lastLoggedIn = !!auth.logged_in;
    $("login-msg").className = "msg " + (auth.state === "failed" ? "err" : auth.state === "ok" ? "ok" : "");
    $("login-msg").textContent = auth.state === "ok" ? "" : auth.message || "";
    $("gt").textContent = s.grid_time ? new Date(s.grid_time * 1000).toLocaleString() : "not heard";
    const up = s.aps.filter(a => a.heard).length;
    let line = "An offline network, watched from this laptop without joining its Wi-Fi. " +
      up + " of " + s.aps.length + " AP(s) heard; listening for " + ago(s.listening_s).replace(" ago", "") + ".";
    if (link.message) line += " Link: " + link.message;
    $("sub").textContent = line;
    renderBanner(s.alert); renderAps(s.aps); renderHandhelds(s.handhelds); renderLog(s.events, s.stats);

    const sections = link.sections || {};
    const status = sections.status ? sections.status.data : null;
    const hist = sections.history ? sections.history.data : null;
    lastHistory = hist;
    if (status && status.grid_name) { $("title").textContent = status.grid_name + " — grid watch"; document.title = status.grid_name + " · grid watch"; }
    const beaconById = {};
    for (const h of s.handhelds) beaconById[h.device] = h;
    renderMap(status, sections.status);
    renderNetwork(status, hist, sections.history);
    renderHandheldTab(status, sections.status, beaconById);
    const traffic = sections.traffic ? sections.traffic.data : null;
    renderTraffic(traffic, sections.traffic);
    if (!traffic) $("traffic-brief").dataset.empty = "1";
    applyTabs(lastLoggedIn);
  } catch (e) {
    if (++failures >= 3) $("sub").textContent = "grid_watch.py is not answering; is it still running?";
  }
}
applyTabs(false);
poll(); setInterval(poll, 1000);
</script>
</body>
</html>
"""


TILE_HOST = "https://tile.openstreetmap.org"
TILE_UA = "LocalGrid-grid-watch/1.0 (offline network monitor; one laptop, no bulk downloads)"
TILE_CACHE_MAX = 512             # tiles kept in memory; nothing is written to disk
TILE_MAX_INFLIGHT = 2            # OpenStreetMap's tile policy: no heavy parallel fetching
TILE_RETRY_S = 15.0              # after a failure, stop asking for a while (offline laptop)


class TileCache:
    """Fetches OpenStreetMap tiles for the map, with a proper User-Agent and no prefetching.

    The browser asks this tool for /tile/z/x/y.png and this tool asks OpenStreetMap, so the page
    itself contacts nothing. Only tiles the open map needs are fetched, one screen at a time, at
    most two at once, and the answers are kept in memory only. A tile URL carries the tile's
    z/x/y grid numbers and nothing else: no coordinate, no query string, and no referrer.
    """

    def __init__(self, enabled=True):
        self.enabled = enabled
        self.lock = threading.Lock()
        self.cache = collections.OrderedDict()
        self.inflight = threading.Semaphore(TILE_MAX_INFLIGHT)
        self.failed_at = 0.0
        self.fetched = 0
        self.online = None

    def get(self, z, x, y):
        """Returns PNG bytes, or None when this laptop cannot reach the tile server."""
        if not self.enabled or not (0 <= z <= 19 and 0 <= x < 2 ** z and 0 <= y < 2 ** z):
            return None
        key = (z, x, y)
        with self.lock:
            hit = self.cache.get(key)
            if hit is not None:
                self.cache.move_to_end(key)
                return hit
            if time.time() - self.failed_at < TILE_RETRY_S:
                return None
        import urllib.error
        import urllib.request
        req = urllib.request.Request(f"{TILE_HOST}/{z}/{x}/{y}.png",
                                     headers={"User-Agent": TILE_UA, "Accept": "image/png",
                                              "Referer": ""})
        with self.inflight:
            try:
                with urllib.request.urlopen(req, timeout=6) as r:
                    body = r.read(1024 * 1024)
            except Exception:
                with self.lock:
                    self.failed_at = time.time()
                    self.online = False
                return None
        with self.lock:
            self.cache[key] = body
            self.fetched += 1
            self.online = True
            while len(self.cache) > TILE_CACHE_MAX:
                self.cache.popitem(last=False)
        return body


def make_server(state, port, auth=None, data=None, tiles=None):
    page = PAGE.encode("utf-8")
    auth = auth if auth is not None else AdminAuth()
    data = data if data is not None else LinkData()
    tiles = tiles if tiles is not None else TileCache()
    tile_path = re.compile(r"^/tile/(\d{1,2})/(\d{1,7})/(\d{1,7})\.png$")

    def snapshot():
        now = time.time()
        a = auth.snapshot()
        s = state.snapshot(now)
        s["auth"] = a
        s["link"] = data.snapshot(now, a["logged_in"])
        s["tiles"] = {"online": tiles.online, "enabled": tiles.enabled}
        return s

    class Handler(BaseHTTPRequestHandler):
        server_version = "LocalGridGridWatch"
        sys_version = ""

        def _send(self, body, ctype, status=200):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                self._send(page, "text/html; charset=utf-8")
                return
            if path == "/state.json":
                # The page says which tab is open, so the traffic counters are asked for only
                # while somebody is looking at them.
                query = self.path.split("?", 1)[1] if "?" in self.path else ""
                data.want_traffic = "tab=traffic" in query
                self._send(json.dumps(snapshot(), ensure_ascii=False).encode("utf-8"),
                           "application/json")
                return
            m = tile_path.match(path)
            if m:
                body = tiles.get(int(m.group(1)), int(m.group(2)), int(m.group(3)))
                if body is None:
                    self._send(b"", "text/plain", 504)
                else:
                    self._send(body, "image/png")
                return
            self.send_error(404)

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            try:
                n = int(self.headers.get("Content-Length") or 0)
                payload = json.loads(self.rfile.read(n) or b"{}")
            except (ValueError, OSError):
                payload = {}
            if path == "/login":
                # The password arrives over the loopback interface, is held in memory only, and is
                # never written to the log file, the console, or state.json.
                pw = payload.get("password") or ""
                if not pw:
                    auth.forget()
                else:
                    auth.set_password(pw)
                    data.request.set()
                self._send(json.dumps(auth.snapshot()).encode("utf-8"), "application/json")
                return
            if path == "/logout":
                auth.forget()
                for name in ("status", "history", "traffic"):
                    data.drop(name)
                data.note("idle", "Logged out.")
                self._send(json.dumps(auth.snapshot()).encode("utf-8"), "application/json")
                return
            if path == "/refresh":
                data.want_traffic = bool(payload.get("traffic"))
                data.request.set()
                self._send(b'{"ok":true}', "application/json")
                return
            self.send_error(404)

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

    def on_detect(device, adv):
        # The device carries the BLE address. It is handed to the grid state only so that the
        # admin link (D70) can open a connection to that AP; it is never printed, logged or
        # served, and nothing reads it here (D21).
        payloads = manufacturer_payloads(adv, company_id)
        if payloads:
            state.on_advert(payloads, adv.rssi, radio=device)

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


# ---------------------------------------------------------------- a mock AP, for --self-check
#
# No AP is needed to test the client: this stands in for one, speaking docs/ble-link.md over an
# in-process transport, with sample replies written from the C code's own formats.

MOCK_SALT = bytes(range(0x40, 0x50))
MOCK_ITERATIONS = 8000
MOCK_CHALLENGE = bytes((i * 7 + 3) & 0xFF for i in range(32))
MOCK_PASSWORD = "lakeside-trip-2026"


class MockAP:
    """One AP's side of the admin link. Chunks small on purpose, to exercise reassembly."""

    def __init__(self, key, password=MOCK_PASSWORD, ap=0, boot=41, session=0x9C3A,
                 status=b"{}", history=b"", traffic=b"", chunk=24):
        self.key = key
        self.ap = ap
        self.boot = boot
        self.session = session
        self.status = status
        self.history = history
        self.traffic = traffic
        self.chunk = chunk
        self.hash = login_hash(password, MOCK_SALT, MOCK_ITERATIONS)
        self.rx = 0
        self.tx = 0
        self.joiner = ChunkJoiner()
        self.logged_in = False
        self.notify = None
        self.seen = []
        self.failures = 0

    def subscribe(self):
        """What the AP notifies the moment the watcher turns notifications on."""
        self.notify(bytes([OP_SESSION, 0]) + (2).to_bytes(2, "little")
                    + self.session.to_bytes(2, "little"))

    async def write(self, data):
        opcode, flags, body = open_link(self.key, DIR_TO_AP, self.session, self.rx, bytes(data))
        self.rx += 1
        done = self.joiner.add(opcode, flags, body)
        if done is not None:
            await self._handle(*done)

    async def _reply(self, opcode, body=b""):
        for flags, part in split_chunks(bytes(body), self.chunk):
            self.notify(seal_link(self.key, DIR_TO_CLIENT, self.session, self.tx,
                                  opcode, flags, part))
            self.tx += 1

    async def _handle(self, opcode, body):
        self.seen.append(opcode)
        if opcode == OP_HELLO:
            out = (bytes([LINK_VERSION, self.ap]) + self.boot.to_bytes(4, "little") + bytes([1])
                   + MOCK_SALT + MOCK_ITERATIONS.to_bytes(4, "little") + MOCK_CHALLENGE)
            await self._reply(OP_HELLO_OK, out)
            return
        if opcode == OP_LOGIN:
            if hmac.compare_digest(bytes(body), login_proof(self.hash, MOCK_CHALLENGE)):
                self.logged_in = True
                await self._reply(OP_LOGIN_OK, b"")
            else:
                self.failures += 1
                await self._reply(OP_LOGIN_FAIL, (self.failures * 2).to_bytes(4, "little"))
            return
        if not self.logged_in:
            await self._reply(OP_ERROR, bytes([1]) + b"log in first")
            return
        if opcode == OP_GET_STATUS:
            await self._reply(OP_STATUS, self.status)
        elif opcode == OP_GET_HISTORY:
            await self._reply(OP_HISTORY, self.history)
        elif opcode == OP_GET_TRAFFIC:
            await self._reply(OP_TRAFFIC, self.traffic)
        else:
            await self._reply(OP_ERROR, bytes([2]) + b"unknown request")


def sample_status_json():
    """What h_status writes (firmware/node/main/web_admin.c), with positions in micro-degrees."""
    return json.dumps({
        "grid_name": "Lakeside Trip", "timezone": "Europe/Berlin", "posix_tz": "CET-1CEST,M3.5.0,M10.5.0/3",
        "node": 0, "node_name": "MAIN", "boot": 41, "uptime_s": 18000, "grid_time": 1789700000,
        "time_quality": 2, "heap_free": 118000, "heap_min": 96000, "handhelds": 3,
        "gps": {"started": True, "heard": True, "fix": True, "sats": 9, "pos": True,
                "lat_u": 47623450, "lon_u": 13045670},
        "links": [{"node": 1, "up": True, "rssi": -62, "age_ms": 400},
                  {"node": 2, "up": False, "rssi": -91, "age_ms": 95000}],
        "devices": [{"device": 1, "name": "Ana", "state": "ONLINE", "node": 0},
                    {"device": 3, "name": "Priya", "state": "ONLINE", "node": 2},
                    {"device": 4, "name": "Tom", "state": "OFFLINE", "node": -1}],
        "users": [{"device": 1, "name": "Ana"}, {"device": 3, "name": "Priya"},
                  {"device": 4, "name": "Tom"}],
        "announce_all": False, "announcers": [1], "groups_version": 7,
        "groups": [{"id": 1, "name": "Cooks", "members": [1, 3]},
                   {"id": 2, "name": "Lake walk", "members": [3, 4]}],
        "positions": [
            {"subject": 0, "name": "MAIN", "ap": True, "lat_u": 47623450, "lon_u": 13045670,
             "fix_time": 1789699940, "sats": 9},
            {"subject": 3, "name": "Priya", "ap": False, "lat_u": 47625100, "lon_u": 13048900,
             "fix_time": 1789699800, "sats": 7}],
    }, ensure_ascii=False).encode("utf-8")


def sample_history_blob():
    """The bytes h_history sends: layout 1, three APs and two incidents."""
    out = bytearray(HIST_HEADER)
    out[0], out[1], out[2], out[3] = 1, 0, 3, 2
    out[4:8] = (1789600000).to_bytes(4, "little")
    out[8], out[9], out[10] = 0, 0, 1
    out[12:16] = (29828333).to_bytes(4, "little")
    aps = [(0, True, "MAIN", 18000, 0, 2, 0, 0, 41, 0xFFFFFFFF, 1),
           (1, False, "NORTH", 17000, 3, 1, 1, 30, 7, 3600, 3),
           (2, False, "SOUTH", 600, 95, 1, 0, 1200, 12, 17000, 9)]
    for ap, me, name, uptime, heard, quality, sync_from, sync_age, boot, prev, reset in aps:
        e = bytearray(HIST_AP)
        e[0], e[1], e[2], e[3] = ap, 1 if me else 0, quality, 0 if me else 1
        e[4], e[5] = sync_from, reset
        e[6:8] = (0 if me else 7).to_bytes(2, "little", signed=True)
        e[8:12] = uptime.to_bytes(4, "little")
        e[12:16] = heard.to_bytes(4, "little")
        e[16:20] = sync_age.to_bytes(4, "little")
        e[20:24] = boot.to_bytes(4, "little")
        e[24:28] = prev.to_bytes(4, "little")
        e[28:28 + len(name)] = name.encode()
        # Two hours of minutes: SOUTH was unreachable for the last twenty.
        for m in range(HIST_AVAIL_MINUTES):
            v = 1
            if ap == 2 and m >= HIST_AVAIL_MINUTES - 20:
                v = 3
            elif ap == 1 and m == 4:
                v = 2
            e[44 + (m >> 2)] |= v << ((m % 4) * 2)
        out += e
    for down, dur, prev_min, boot, ap, kind, reset, seen in [
            (1789699000, 240, 283, 12, 2, 1, 9, 0), (1789698000, 65, 0xFFFF, 7, 1, 2, 0, 0)]:
        i = bytearray(HIST_INCIDENT)
        i[0:4] = down.to_bytes(4, "little")
        i[4:6] = dur.to_bytes(2, "little")
        i[6:8] = prev_min.to_bytes(2, "little")
        i[8:10] = boot.to_bytes(2, "little")
        i[10], i[11], i[12] = ap, (kind & 3) | (reset << 2), seen
        out += i
    return bytes(out)


def sample_traffic_blob(voice_dropped=12, heap_min=18 * 1024, queue_high=17):
    """A TRAFFIC reply in the layout traffic.c builds (see decode_traffic above)."""
    n_links, n_buckets = 2, 10
    classes = {                     # in, out, relayed, per lg_traffic_class_t
        "direct": (1420, 1380, 640), "group": (880, 870, 300), "broadcast": (42, 42, 20),
        "voice": (9600, 9400, 0), "ack": (2100, 2080, 0), "presence": (5200, 5100, 0),
        "announce": (6, 6, 0), "position": (310, 300, 0), "time": (120, 118, 0),
        "other": (17, 15, 0)}
    in_total = sum(c[0] for c in classes.values())
    out_total = sum(c[1] + c[2] for c in classes.values())
    out = bytearray(TRAFFIC_HEADER)
    out[0], out[1], out[2] = TRAFFIC_LAYOUT, 0, n_links
    out[3], out[4], out[5] = len(MSG_CLASSES), n_buckets, 61
    out[6:8] = (30000).to_bytes(2, "little")
    out[8:12] = (18000).to_bytes(4, "little")
    out[12:16] = (1789700000).to_bytes(4, "little")
    out[16:20] = in_total.to_bytes(4, "little")
    out[20:24] = out_total.to_bytes(4, "little")
    for cls in MSG_CLASSES:
        for value in classes[cls]:
            out += value.to_bytes(4, "little")
    faults = {"duplicate": 5100, "table_full": 0, "unknown_recipient": 3, "decrypt_failed": 0,
              "ttl_expired": 7, "queue_full": 2, "send_timeout": 0,
              "voice_dropped": voice_dropped, "malformed": 1, "rejected": 0}
    for name in FAULT_FIELDS:
        out += faults[name].to_bytes(4, "little")
    se = bytearray(TRAFFIC_SESS)
    se[0], se[1] = 3, 3
    se[4:8] = (19).to_bytes(4, "little")
    se[8:12] = (11).to_bytes(4, "little")
    se[12:16] = (8).to_bytes(4, "little")
    se[16:20] = (4_100_000).to_bytes(4, "little")
    se[20:24] = (3_900_000).to_bytes(4, "little")
    se[24:28] = (2400).to_bytes(4, "little")
    out += se
    perf = bytearray(TRAFFIC_PERF)
    perf[0:4] = (118000).to_bytes(4, "little")
    perf[4:8] = heap_min.to_bytes(4, "little")
    perf[8:12] = (64000).to_bytes(4, "little")
    perf[12:14] = (2).to_bytes(2, "little")
    perf[14:16] = queue_high.to_bytes(2, "little")
    perf[16:20] = (1800).to_bytes(4, "little")
    perf[20:24] = (2400).to_bytes(4, "little")
    perf[24:28] = (140).to_bytes(4, "little")
    perf[28:32] = (6).to_bytes(4, "little")
    perf[32:36] = (0).to_bytes(4, "little")
    perf[36:40] = (3).to_bytes(4, "little")
    out += perf
    for i in range(n_buckets):
        out += (30 + 4 * i).to_bytes(2, "little")
        out += (28 + 4 * i).to_bytes(2, "little")
    for ap, up, rssi, sent, recv, fail, heard_ms in [(1, True, -62, 20400, 20100, 60, 900),
                                                     (2, False, -91, 8000, 5100, 900, 95000)]:
        e = bytearray(TRAFFIC_LINK)
        e[0], e[1] = ap, 1 if up else 0
        e[2] = rssi & 0xFF
        e[4:8] = sent.to_bytes(4, "little")
        e[8:12] = (2_400_000).to_bytes(4, "little")
        e[12:16] = recv.to_bytes(4, "little")
        e[16:20] = (2_300_000).to_bytes(4, "little")
        e[20:24] = fail.to_bytes(4, "little")
        e[24:28] = heard_ms.to_bytes(4, "little")
        out += e
    return bytes(out)


async def run_mock_link(key, auth, data, password_ok=True, want_traffic=True, ap_name="MAIN"):
    """The whole conversation against MockAP, with no radio and no AP."""
    mock = MockAP(key, status=sample_status_json(), history=sample_history_blob(),
                  traffic=sample_traffic_blob())
    session = LinkSession(key, mock.write)
    mock.notify = session.feed
    mock.subscribe()
    ok = await pull_once(session, auth, data, ap_name, want_traffic)
    return mock, session, ok


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

    # ---------------------------------------------------------- the admin link (D70)
    lk = link_key(k)
    check("K_link is HKDF-SHA256 of the status key, with the link's own salt and info",
          len(lk) == 32 and lk != k and lk == link_key(k)
          and lk != HKDF(hashes.SHA256(), 32, b"LG-BLE-LINK-2", LINK_KDF_INFO).derive(k))
    check("the nonce is dir, session, 0, counter, four zeros",
          link_nonce(1, 0x2B7F, 0x01020304)
          == bytes([1, 0x7F, 0x2B, 0x00, 4, 3, 2, 1, 0, 0, 0, 0]))

    msg = seal_link(lk, DIR_TO_AP, 0x9C3A, 5, OP_GET_STATUS, 0, b"")
    check("a sealed message is a clear 4-byte header, the ciphertext and a full 16-byte tag",
          msg[:4] == bytes([OP_GET_STATUS, 0, 0, 0]) and len(msg) == LINK_HEADER + LINK_TAG)
    check("it opens again at the same session and counter",
          open_link(lk, DIR_TO_AP, 0x9C3A, 5, msg)[0] == OP_GET_STATUS)

    def refused(*args):
        try:
            open_link(*args)
            return False
        except LinkError:
            return True

    def refused_traffic(blob):
        try:
            decode_traffic(blob)
            return False
        except LinkError:
            return True

    tampered = bytearray(msg); tampered[-1] ^= 1
    check("a message with a flipped tag bit is refused",
          refused(lk, DIR_TO_AP, 0x9C3A, 5, bytes(tampered)))
    tampered = bytearray(msg); tampered[1] ^= 0x10
    check("a changed header (the AAD) is refused",
          refused(lk, DIR_TO_AP, 0x9C3A, 5, bytes(tampered)))
    check("the same message replayed at an earlier counter is refused",
          refused(lk, DIR_TO_AP, 0x9C3A, 4, msg))
    check("a message from another session is refused", refused(lk, DIR_TO_AP, 0x9C3B, 5, msg))
    check("a message sealed the other way round is refused",
          refused(lk, DIR_TO_CLIENT, 0x9C3A, 5, msg))
    check("a message shorter than a header and a tag is refused",
          refused(lk, DIR_TO_AP, 0x9C3A, 5, msg[:10]))

    payload = bytes((i * 31) & 0xFF for i in range(1000))
    joiner = ChunkJoiner()
    joined = None
    for n, (flags, part) in enumerate(split_chunks(payload, 24)):
        wire = seal_link(lk, DIR_TO_CLIENT, 7, n, OP_STATUS, flags, part)
        op, fl, body = open_link(lk, DIR_TO_CLIENT, 7, n, wire)
        joined = joiner.add(op, fl, body)
    check("a long reply is split into chunks and joined back in order, byte for byte",
          joined == (OP_STATUS, payload))

    # RFC-style known answer: PBKDF2-HMAC-SHA256("password", "salt", 4096, 32).
    check("PBKDF2-HMAC-SHA256 matches the published vector",
          login_hash("password", b"salt", 4096).hex()
          == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a")
    stored = login_hash(MOCK_PASSWORD, MOCK_SALT, MOCK_ITERATIONS)
    import hashlib as _hashlib
    check("the login proof is HMAC-SHA256(stored hash, challenge || \"lg-ble-admin\")",
          login_proof(stored, MOCK_CHALLENGE)
          == hmac.new(stored, MOCK_CHALLENGE + b"lg-ble-admin", _hashlib.sha256).digest())
    check("a different challenge makes a different proof (no replay of a proof)",
          login_proof(stored, MOCK_CHALLENGE) != login_proof(stored, bytes(32)))
    check("a wrong password makes a different proof",
          login_proof(login_hash("wrong-password-0", MOCK_SALT, MOCK_ITERATIONS), MOCK_CHALLENGE)
          != login_proof(stored, MOCK_CHALLENGE))

    # The whole conversation against a mock AP: no radio, no AP, no hardware.
    auth = AdminAuth()
    link = LinkData()
    check("before anyone logs in, nothing pulled is served, positions included",
          link.snapshot(t0, False)["sections"] == {})
    auth.set_password(MOCK_PASSWORD)
    mock, sess, ok = asyncio.run(run_mock_link(lk, auth, link))
    check("HELLO, LOGIN, GET_STATUS, GET_HISTORY and GET_TRAFFIC all ran in order",
          ok and mock.seen == [OP_HELLO, OP_LOGIN, OP_GET_STATUS, OP_GET_HISTORY, OP_GET_TRAFFIC])
    check("the session the AP picked is used, and each direction counts its own messages",
          sess.session == mock.session and sess.tx_counter == mock.rx and sess.rx_counter == mock.tx)
    lone = LinkSession(lk, None)
    lone.feed(bytes([OP_SESSION, 0]) + (2).to_bytes(2, "little") + (0x9C3A).to_bytes(2, "little"))
    check("the session arrives unsealed, before anything else, and opens the conversation",
          lone.session == 0x9C3A and lone.opened.is_set() and lone.queue.empty())
    early = LinkSession(lk, None)
    early.feed(seal_link(lk, DIR_TO_CLIENT, 0x9C3A, 0, OP_HELLO_OK, 0, b"x"))
    check("a sealed message before the session is refused, not guessed at",
          isinstance(early.queue.get_nowait(), LinkError))
    check("the login is a proof: the password itself never went over the link",
          auth.snapshot()["logged_in"] and MOCK_PASSWORD.encode() not in sample_status_json())

    served = link.snapshot(t0, True)["sections"]
    stat = served["status"]["data"]
    check("the AP's /api/status JSON decodes: this AP, groups, who may announce, handhelds",
          stat["node_name"] == "MAIN" and [g["name"] for g in stat["groups"]] == ["Cooks", "Lake walk"]
          and stat["announce_all"] is False and stat["announcers"] == [1]
          and len(stat["devices"]) == 3)
    pos = {p["name"]: p for p in stat["positions"]}
    check("positions decode in micro-degrees, APs and handhelds apart",
          pos["MAIN"]["ap"] and abs(pos["MAIN"]["lat_u"] / 1e6 - 47.62345) < 1e-6
          and not pos["Priya"]["ap"] and pos["Priya"]["sats"] == 7)

    hist = served["history"]["data"]
    check("the /api/history bytes decode: three APs, names, uptime, last restart",
          [a["name"] for a in hist["aps"]] == ["MAIN", "NORTH", "SOUTH"] and hist["aps"][0]["self"]
          and hist["aps"][2]["reset"] == "low supply voltage (brownout)"
          and hist["aps"][1]["sync_from"] == 1)
    south = hist["aps"][2]["avail"]
    check("availability unpacks to 120 minutes, two bits each, oldest first",
          len(south) == HIST_AVAIL_MINUTES and south[0] == 1 and south[-1] == 3
          and hist["aps"][1]["avail"][4] == 2)
    check("the outage log decodes: what happened, for how long, and who saw it",
          len(hist["incidents"]) == 2 and hist["incidents"][0]["kind"] == "restarted"
          and hist["incidents"][0]["reset"] == "low supply voltage (brownout)"
          and hist["incidents"][0]["prev_run_s"] == 283 * 60
          and hist["incidents"][1]["kind"] == "link" and hist["incidents"][1]["prev_run_s"] == -1)

    traf = served["traffic"]["data"][0]["data"]
    check("the traffic counters decode by class, in, out and relayed",
          traf["messages"]["direct"] == {"in": 1420, "out": 1380, "relayed": 640}
          and traf["messages"]["voice"]["in"] == 9600
          and list(traf["messages"]) == MSG_CLASSES
          and traf["total_messages"] == traf["in_total"] + traf["out_total"] == 39966)
    check("drops and faults decode, duplicates apart from the ones that matter",
          traf["faults"]["duplicate"] == 5100 and traf["faults"]["queue_full"] == 2
          and traf["faults"]["table_full"] == 0 and traf["faults"]["ttl_expired"] == 7
          and traf["faults"]["voice_dropped"] == 12)
    check("the backbone links decode, and a failing link is visible",
          [l["ap"] for l in traf["links"]] == [1, 2] and traf["links"][0]["up"]
          and traf["links"][0]["rssi"] == -62 and not traf["links"][1]["up"]
          and traf["links"][1]["failures"] == 900 and traf["links"][1]["heard_s"] == 95)
    check("the handheld side and the AP's own performance decode",
          traf["handhelds"]["sessions"] == 3 and traf["handhelds"]["slowest_send_ms"] == 2400
          and traf["perf"]["heap_min"] == 18 * 1024 and traf["perf"]["queue_high"] == 17
          and traf["perf"]["stack_core"] == 1800 and traf["perf"]["loop_max_ms"] == 140
          and traf["perf"]["cpu_busy"] == 61)
    check("rates come out of the buckets, a minute and five minutes",
          traf["bucket_s"] == 30 and len(traf["buckets"]) == 10
          and traf["rate_1m"] == 252.0 and traf["rate_5m"] == 188.0)
    check("a traffic reply in a layout this tool does not know is refused, not guessed",
          refused_traffic(bytes([9]) + sample_traffic_blob()[1:]))
    short = bytearray(sample_traffic_blob())
    short[3] = len(MSG_CLASSES) + 1
    check("a traffic reply counting classes this tool does not know is refused",
          refused_traffic(bytes(short)))

    # A wrong password: refused, and nothing is pulled.
    auth2, link2 = AdminAuth(), LinkData()
    auth2.set_password("definitely-not-it")
    ok2 = asyncio.run(run_mock_link(lk, auth2, link2))[2]
    check("a wrong admin password is refused and nothing is pulled",
          not ok2 and not auth2.snapshot()["logged_in"] and link2.snapshot(t0, True)["sections"] == {})
    auth3, link3 = AdminAuth(), LinkData()
    ok3 = asyncio.run(run_mock_link(lk, auth3, link3))[2]
    check("with no password at all, the link stops before asking for anything",
          not ok3 and link3.snapshot(t0, True)["sections"] == {})

    # The dashboard answers on loopback and serves JSON without addresses or hidden positions.
    tiles = TileCache(enabled=False)
    srv = make_server(st, 0, auth, link, tiles)
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
        check("the page needs no network: no external script, style or image",
              not re.search(r"(src|href)=\"https?://(?!www\.openstreetmap\.org/copyright)", page)
              and "cdn" not in page.lower())
        check("nothing served looks like a BLE address (D21)",
              not re.search(r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}", text + page))
        urls = re.findall(r"(?:src|href)=\"([^\"]*)\"", page)
        check("no URL in the page carries a coordinate",
              not any(re.search(r"-?\d{1,3}\.\d{3,}", u) for u in urls))
        check("the only thing fetched from the Internet is a map tile, asked for by grid square",
              "/tile/" in page and TILE_HOST not in page
              and re.search(r'"/tile/" \+ z \+ "/"', page) is not None)
        check("the tabs and their wording match the admin page",
              all(f'data-tab="{t}"' in page for t in ("overview", "map", "network", "handhelds",
                                                      "traffic"))
              and "Availability, last 2 hours" in page and "What happened to the APs" in page)
        check("the pulled sections are served with the AP's name and how old they are",
              data["link"]["sections"]["status"]["ap"] == "MAIN"
              and "at_age_s" in data["link"]["sections"]["history"])
        # Logging out hides everything the login unlocked, positions first.
        urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/logout",
                                                      data=b"{}"), timeout=5).read()
        out = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/state.json",
                                                timeout=5).read())
        check("logging out hides positions and every pulled tab again",
              out["link"]["sections"] == {} and "47.62" not in json.dumps(out)
              and out["auth"]["logged_in"] is False)
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/tile/12/2199/1424.png", timeout=5)
            tile_ok = False
        except Exception as e:
            tile_ok = getattr(e, "code", None) == 504     # --no-map-tiles: nothing is fetched
        check("with tiles switched off the map never reaches the Internet", tile_ok)
    finally:
        srv.shutdown()
        srv.server_close()

    failed = [n for n, ok in results if not ok]
    print(f"\nSELF-CHECK: {'PASS' if not failed else 'FAIL'} "
          f"({len(results) - len(failed)} of {len(results)} checks)")
    return 0 if not failed else 1


# ---------------------------------------------------------------- pairing a phone (D69)

PAIR_PREFIX = "LGW1:"


def pairing_code(company_id, disc, k):
    """The phone's pairing text: company ID, grid ID and the status key K only (docs/ble-status.md)."""
    import base64
    raw = company_id.to_bytes(2, "little") + bytes(disc) + bytes(k)
    return PAIR_PREFIX + base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def parse_pairing_code(text):
    import base64
    if not text.startswith(PAIR_PREFIX):
        raise ValueError("not a LocalGrid Watch pairing code")
    body = text[len(PAIR_PREFIX):]
    raw = base64.urlsafe_b64decode(body + "=" * (-len(body) % 4))
    if len(raw) != 38:
        raise ValueError("pairing code has the wrong length")
    return int.from_bytes(raw[:2], "little"), raw[2:6], raw[6:]


PAIR_PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pair LocalGrid Watch</title><style>
:root{color-scheme:dark}body{margin:0;background:#0f1412;color:#e6ece9;
font:16px/1.45 system-ui,sans-serif;display:flex;justify-content:center}
main{max-width:560px;padding:24px 16px;text-align:center}
.qr{background:#fff;display:inline-block;padding:16px;border-radius:12px;margin:16px 0}
.qr svg{width:min(320px,80vw);height:auto;display:block}
code{word-break:break-all;background:#1b2320;padding:8px;border-radius:8px;display:block;font-size:13px}
p.warn{color:#f0b35a}</style></head><body><main>
<h1>Pair a phone with LocalGrid Watch</h1>
<p>In the app, tap <b>Pair</b> and scan this code.</p>
<div class="qr">__QR__</div>
<p>Or paste this text into the app:</p><code>__CODE__</code>
<p class="warn">This code lets a phone read this grid's status until the grid's secrets are
regenerated. It cannot join the grid, send messages, or fake a status. Show it only to phones
you trust, and close this window when done.</p></main></body></html>"""


def run_pair(company_id, disc, k, port, open_browser):
    code = pairing_code(company_id, disc, k)
    assert parse_pairing_code(code) == (company_id, bytes(disc), bytes(k))
    try:
        import segno
    except ImportError:
        segno = None
    print("LocalGrid Watch pairing code (read-only status key; not the grid's secret):\n")
    print(f"  {code}\n")
    if segno is None:
        print("For a QR code: pip install segno, then run --pair again.")
        return 0
    qr = segno.make(code, error="m", mode="byte")
    import io
    buf = io.BytesIO()
    qr.save(buf, kind="svg", scale=1, border=0, xmldecl=False, svgns=True, nl=False)
    svg = buf.getvalue().decode("utf-8")
    # segno writes a fixed width and height; a viewBox lets the page scale the code up.
    svg = re.sub(r'width="(\d+)" height="(\d+)"', r'viewBox="0 0 \1 \2" shape-rendering="crispEdges"',
                 svg, count=1)
    page = PAIR_PAGE.replace("__QR__", svg).replace("__CODE__", code)

    class Pair(BaseHTTPRequestHandler):
        def do_GET(self):
            body = page.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    try:
        srv = ThreadingHTTPServer(("127.0.0.1", port), Pair)
    except OSError as e:
        print(f"Cannot serve on 127.0.0.1:{port}: {e}. Try --port.")
        return 2
    url = f"http://127.0.0.1:{port}/"
    print(f"QR code at {url} (this laptop only). Ctrl+C when the phone is paired.")
    if open_browser:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


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
    ap.add_argument("--no-map-tiles", action="store_true",
                    help="never fetch map tiles; the Map tab always draws the plan instead")
    ap.add_argument("--self-check", action="store_true",
                    help="offline: seal frames per docs/ble-status.md and check the decoder")
    ap.add_argument("--pair", action="store_true",
                    help="show the QR code that pairs the LocalGrid Watch Android app (D69)")
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

    if args.pair:
        return run_pair(company_id, disc, derive_key(backbone), args.port, not args.no_browser)

    k = derive_key(backbone)
    state = GridState(k, disc, bench_ap_names(), log_path=args.log)
    auth = AdminAuth()
    data = LinkData()
    tiles = TileCache(enabled=not args.no_map_tiles)
    try:
        srv = make_server(state, args.port, auth, data, tiles)
    except OSError as e:
        print(f"Cannot serve on 127.0.0.1:{args.port}: {e}. Try --port.")
        return 2
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    print(f"LocalGrid grid watch: listening over BLE for this grid's APs (company ID "
          f"0x{company_id:04X}); dashboard at {url}")
    print("Read-only: this laptop never joins the grid's Wi-Fi and carries no message (D25, D70). "
          "Type the admin password on the page for positions, groups, history and traffic. "
          "Ctrl+C to stop.")
    if not args.no_browser:
        webbrowser.open(url)

    async def run():
        stop = asyncio.Event()
        pull = asyncio.create_task(puller(state, link_key(k), auth, data, stop))
        try:
            await listen(state, company_id, stop, args.seconds)
        except asyncio.CancelledError:
            stop.set()
            raise
        finally:
            stop.set()
            pull.cancel()
            try:
                await pull
            except (asyncio.CancelledError, Exception):
                pass

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
