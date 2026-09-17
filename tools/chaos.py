#!/usr/bin/env python3
"""Chaos run on the bench grid: take APs and handhelds out at random and measure how the network recovers.

Without --live this only prints the seeded fault schedule and touches no port. With --live it holds
every bench port open for the whole run, injects faults, has the handhelds message each other, and
writes a report. Run inside the ESP-IDF Python environment (pyserial).

Examples:
  python tools/chaos.py --hours 10 --seed 7                    # print the schedule only (3 APs, 2 handhelds)
  python tools/chaos.py --self-check                           # check the log parsers, no boards
  python tools/chaos.py --hours 0.2 --live                     # 12-minute smoke run
  python tools/chaos.py --hours 10 --live --quiet-handhelds    # overnight, notification sound off

Boards are discovered, not listed: every USB serial port is opened and reset, and each board's own
LGID line says what it is (ID, role, board code, AP index and name or handheld device index). The
device map only lends a board its bench name. Group membership comes from each handheld's 'groups'.

Faults, all from the PC side of the USB serial port (no firmware change):
  pulse  pulse EN through RTS: a crash and immediate restart, on every board
  hold   hold EN low through RTS for the outage, then release: a power loss. Only where the port's USB
         ID is a separate bridge chip (CP210x, CH34x, FTDI). A port that is the chip's own USB (Espressif
         303A) would disappear while the chip is held in reset, so those boards get a pulse instead.

Safety rules, enforced while the run is live:
  - at most --max-aps-down APs out at once (default: all but one, so grid time survives), unless --blackout
  - never every handheld at once
  - no new fault until the grid is back to steady state; a missed --recovery-timeout is a finding
  - on exit, crash, or Ctrl+C every held board is released and every port closed with DTR and RTS low

What the PC does: it resets boards and types console commands. Every message is sent and received
by the handhelds themselves; the PC never joins the grid. See .claude/skills/chaos/SKILL.md on D25.

Output (gitignored): chaos-runs/<start time>/ with raw-NN.log per hour, events.jsonl, summary.md.
"""
import argparse
import datetime
import json
import os
import pathlib
import queue
import random
import re
import signal
import statistics
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import flash  # noqa: E402
from flash import PANIC, acquire_lock, load_map  # noqa: E402

# Run from a git worktree, this file's own tools/ would hold a lock no other session looks at. The lock lives
# in the main checkout, where flash.py and build.py run.
_common = ROOT / ".git"
if _common.is_file():                       # a worktree: ".git" is a file naming the real git directory
    _gitdir = pathlib.Path(_common.read_text(encoding="utf-8").split("gitdir:", 1)[1].strip())
    flash.LOCK = _gitdir.parent.parent.parent / "tools" / ".flash.lock"
# Faults a debug build reports without a Guru Meditation line.
CRASH = re.compile(PANIC.pattern + r"|CORRUPT HEAP|Task watchdog got triggered|Asserted at|Stack smashing|"
                   r"stack overflow in task", re.IGNORECASE)

RUNS = ROOT / "chaos-runs"
STATUS_EVERY_S = 30            # APs print "[GRID] links ... time ..." this often (STATUS_LOG_MS)
PROBE_DEADLINE_S = 30          # a message not received by then is lost
FAULT_GRACE_S = 40             # a restart this soon after an injected fault is ours
QUERY_EVERY_S = 3600           # hourly 'nodes' on APs and 'mem' on handhelds
SUMMARY_EVERY_S = 600
PORT_RETRY_S = 30
# A board that says nothing for this long (APs print a line every second) is checked from the PC side. Twice a
# CP210x port went silent with no error while its board was fine: once node-main never seemed to restart after a
# hold, once node-south returned no bytes to a fresh open. Silence is recovered and recorded, not left to look
# like a dead AP.
SILENT_AP_S = 90
SILENT_HANDHELD_S = 240

# Log lines, matched anywhere in the line (ESP-IDF adds "I (1234) TAG: " in front).
RX = {
    "ap_boot": re.compile(r"\[GRID\] LocalGrid node (\d+) (\S+) starting, boot (\d+)"),
    "hh_boot": re.compile(r"\[NET\] Handheld service started: device (\d+) .*boot (\d+)"),
    "restart": re.compile(r"\[GRID\] Last restart: ([^,]+)"),
    "link_up": re.compile(r"\[BB\] Link up to node (\d+)"),
    "link_lost": re.compile(r"\[BB\] (?:Link lost to|One-way link to) node (\d+)"),
    "peer_rebooted": re.compile(r"\[BB\] Node (\d+) rebooted"),
    "link_kept": re.compile(r"\[BB\] No HELLO from node (\d+) .*link kept"),
    "ap_status": re.compile(r"\[GRID\] links (\d+) handhelds (\d+) heap (\d+) min (\d+) time (\w+)"),
    "settings": re.compile(r"\[GRID\] (?:Adopted settings|Settings) version (\d+)"),
    "registered": re.compile(r"\[GRID\] Registered with node (\d+) as device (\d+)"),
    "hh_down": re.compile(r"\[NET\] (.+); searching again in"),
    "hh_online": re.compile(r"\[NET\] Online; free heap (\d+) KB, lowest (\d+) KB"),
    "not_sent": re.compile(r"\[MSG\] Not sent \(([^)]*)\): (CX\S+)"),
    "received": re.compile(r"\[MSG\] From (\d+) .*: (CX\S+)"),
    # 1:1 text is never logged (AGENTS.md), so direct probes are matched by (author, boot, seq) instead of token.
    "sent_direct": re.compile(r"\[MSG\] Sent(?: URGENT)? to device (\d+): 1:1 boot (\d+) seq (\d+)"),
    "not_sent_direct": re.compile(r"\[MSG\] Not sent \(([^)]*)\): 1:1 to device (\d+)"),
    "received_direct": re.compile(r"\[MSG\] From (\d+) .*: 1:1 boot (\d+) seq (\d+)"),
    "volume": re.compile(r"Volume: (\w+)"),
    "time": re.compile(r"\[TIME\] (.+)"),
}
# USB vendor IDs of serial bridges that stay enumerated while the ESP32 behind them is held in reset.
SEPARATE_BRIDGES = {0x10C4: "CP210x", 0x1A86: "CH34x", 0x0403: "FTDI"}
NATIVE_USB = {0x303A: "Espressif native USB"}
IDENTITY = re.compile(r"LGID: (\S+) role=(\w+) board=(\w+)(?: node=(\d+) (\S+))?(?: device=(\d+))?")
GROUP_ROW = re.compile(r"^\s*(\d+)\s+(\S+)\s+(member|not a member)\s*$")
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")   # D21: never write a hardware address


# ---- schedule: pure, seeded, and the same with or without --live ----

def outage_seconds(rng):
    r = rng.random()
    if r < 0.5:
        return round(rng.uniform(5, 30))
    if r < 0.8:
        return round(rng.uniform(30, 120))
    return round(rng.uniform(120, 600))


def make_schedule(rng, n_aps, n_hhs, args):
    """Experiments with planned start offsets, naming victims by slot ("AP1", "HH2"). Slots are filled from the
    discovered boards in AP index and device index order. A live run starts each experiment only once the grid
    is steady, and a hold on a board that cannot be held becomes a pulse."""
    aps = [f"AP{i + 1}" for i in range(n_aps)]
    hhs = [f"HH{i + 1}" for i in range(n_hhs)]
    max_down = len(aps) if args.blackout else min(args.max_aps_down, max(len(aps) - 1, 0))
    plan, t, end = [], 0.0, args.hours * 3600
    while True:
        t += rng.uniform(args.min_gap, args.max_gap) * 60
        if t >= end:
            return plan
        r = rng.random()
        if args.blackout and aps and r < 0.05:
            victims = list(aps)
        elif max_down >= 2 and r < 0.20:
            victims = rng.sample(aps, 2)
        elif max_down >= 1 and (r < 0.75 or len(hhs) < 2):
            victims = [rng.choice(aps)]
        elif len(hhs) >= 2:
            victims = [rng.choice(hhs)]       # never every handheld: probes need a sender and a receiver
        else:
            continue
        kind = "hold" if rng.random() < 0.7 else "pulse"
        outage = outage_seconds(rng) if kind == "hold" else 0
        plan.append({"n": len(plan) + 1, "at_s": round(t), "victims": list(victims),
                     "kind": kind, "outage_s": outage})
        t += outage + 60


def print_schedule(plan, args):
    print(f"Seed {args.seed}, {args.hours} h, gaps {args.min_gap}-{args.max_gap} min, "
          f"{'blackout allowed' if args.blackout else f'at most {args.max_aps_down} AP(s) down'}")
    for e in plan:
        at = str(datetime.timedelta(seconds=e["at_s"]))
        what = f"hold {e['outage_s']} s" if e["kind"] == "hold" else "pulse"
        print(f"  #{e['n']:<3} {at:>8}  {what:<12} {', '.join(e['victims'])}")
    holds = [e["outage_s"] for e in plan if e["kind"] == "hold"]
    print(f"{len(plan)} experiments: {len(holds)} holds (total {sum(holds) // 60} min out), "
          f"{len(plan) - len(holds)} pulses")


# ---- live run ----

class Board:
    """One USB serial port, and what the board behind it said about itself."""

    def __init__(self, port, vid):
        self.port = port
        self.set_bridge(vid)
        self.name = self.slot = port     # until the board says who it is
        self.id = self.role = self.board = self.index = self.ap_name = None
        self.expected_id = None          # set once discovery is over: a different ID later means a swapped board
        self.groups = {}                 # handheld: group name -> member, from its 'groups' command
        self.ser = None
        self.lost_since = None
        self.last_retry = 0.0
        self.held = False
        self.wrong = False               # the port answered with another board's ID: never touched again
        self.injected_at = -1e9          # monotonic time of the last fault or harness reset
        self.boot = None
        self.id_seen = None
        self.links = set()
        self.status = None               # AP: last "[GRID] links" sample since boot
        self.registered_node = None      # handheld: node it is registered with
        self.settings_version = None
        self.lowest_heap = None          # bytes on APs, KB on handhelds (as the firmware prints them)
        self.volume = None
        self.failover = None             # (start time, experiment) while an AP under it is out
        self.drops = 0                   # handheld: times it lost its AP or restarted
        self.last_line_at = time.monotonic()
        self.boot_at = 0.0                 # monotonic time of the last boot banner
        self.silence_step = 0            # 0 quiet is fine, 1 lines released again, 2 port reopened

    def set_bridge(self, vid):
        self.vid = vid
        self.bridge = SEPARATE_BRIDGES.get(vid) or NATIVE_USB.get(vid) or f"USB vendor {vid:04X}"
        self.holdable = vid in SEPARATE_BRIDGES

    @property
    def is_ap(self):
        return self.role == "N"


class Chaos:
    def __init__(self, args, names):
        import serial   # the ESP-IDF Python environment has it; a schedule-only run and --self-check do not need it
        self.serial = serial
        self.args = args
        self.names = names                # device ID -> bench name, from the device map when it has one
        self.rng = random.Random(args.seed)
        self.boards, self.aps, self.hhs, self.by_name = [], [], [], {}
        self.group = None
        self.lines = queue.Queue()
        self.readers = []
        self.stopping = threading.Event()
        self.start_wall = datetime.datetime.now()
        self.t0 = time.monotonic()
        self.run_id = self.start_wall.strftime("%H%M")
        self.out = RUNS / self.start_wall.strftime("%Y%m%d-%H%M%S")
        self.out.mkdir(parents=True, exist_ok=True)
        self.events = open(self.out / "events.jsonl", "a", encoding="utf-8")
        self.raw, self.raw_hour = None, -1
        self.findings, self.experiments, self.probes = [], [], []
        self.pending = {}                 # token -> probe
        self.direct_ids = {}              # (author, boot, seq) -> token, for direct probes once sent
        # (author, boot, seq) -> (receiver, time) for a 1:1 receipt logged before its sender's "Sent" line reached
        # the PC. The two ports deliver independently, and run 2 reported a delivered message as lost that way.
        self.early_direct = {}
        self.probe_n = 0
        self.next_probe = time.monotonic() + 60
        self.next_query = time.monotonic() + QUERY_EVERY_S
        self.next_summary = time.monotonic() + SUMMARY_EVERY_S
        self.time_set = False
        self.link_saves = 0
        self.known_ports = set()
        self.probing = False
        # Hard stop for every wait: the planned end plus the longest hold and a full recovery.
        self.deadline = time.monotonic() + args.hours * 3600 + 600 + args.recovery_timeout + 300

    # ---- records ----

    def now_s(self):
        return round(time.monotonic() - self.t0, 1)

    def event(self, what, /, **fields):
        rec = {"t": self.now_s(), "wall": datetime.datetime.now().isoformat(timespec="seconds"), "event": what, **fields}
        self.events.write(json.dumps(rec, ensure_ascii=False) + "\n")
        self.events.flush()
        return rec

    def finding(self, text, **fields):
        self.findings.append(self.event("finding", text=text, **fields))
        self.say(f"FINDING: {text}")

    def say(self, text):
        line = f"[chaos {self.now_s():>8.1f}s] {text}"
        print(line, flush=True)
        self.write_raw(line)

    def write_raw(self, line):
        hour = int((time.monotonic() - self.t0) // 3600)
        if hour != self.raw_hour:
            if self.raw:
                self.raw.close()
            self.raw = open(self.out / f"raw-{hour:02d}.log", "a", encoding="utf-8")
            self.raw_hour = hour
        self.raw.write(line + "\n")

    # ---- ports ----

    def open(self, b):
        b.ser = self.serial.Serial(b.port, 115200, timeout=0.1, write_timeout=0.5)
        b.injected_at = time.monotonic()      # opening resets CP210x and CH340 boards: not a finding
        b.lost_since = None
        self.known_ports.add(b.port)
        t = threading.Thread(target=self.reader, args=(b, b.ser), daemon=True)
        t.start()
        self.readers.append(t)

    def reader(self, b, ser):
        buf = b""
        while not self.stopping.is_set():
            try:
                chunk = ser.read(1024)
            except (self.serial.SerialException, OSError, TypeError, AttributeError):
                self.lines.put((b, ser, None))
                return
            if not ser.is_open:
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.put((b, ser, MAC_RE.sub("xx:xx:xx:xx:xx:xx", line.decode("utf-8", "replace").rstrip("\r"))))

    def write(self, b, command):
        if b.ser is None:
            return False
        self.write_raw(f"[{b.name}] >>> {command}")
        try:
            b.ser.write((command + "\r\n").encode())
            return True
        except (self.serial.SerialException, OSError):
            return False

    def pulse(self, b):
        """EN low through RTS for 0.2 s with IO0 high. DTR is rewritten after each change for usbser.sys."""
        b.injected_at = time.monotonic()
        b.ser.dtr = False
        b.ser.rts = True
        b.ser.dtr = False
        time.sleep(0.2)
        b.ser.rts = False
        b.ser.dtr = False

    def hold(self, b):
        b.injected_at = time.monotonic()
        b.held = True
        b.ser.dtr = False
        b.ser.rts = True
        b.ser.dtr = False
        self.on_down(b)

    def release_hold(self, b):
        b.injected_at = time.monotonic()
        b.held = False
        if b.ser is not None:
            b.ser.rts = False
            b.ser.dtr = False

    def close_all(self):
        """Every held board released and every port closed with both lines low (a plain close holds boards in reset).
        Readers stop first: closing a port under a thread blocked in read() crashed Python (exit 139) after a run."""
        self.stopping.set()
        for t in self.readers:
            t.join(timeout=1)
        for b in self.boards:
            self.release_port(b)
            b.held = False

    def port_lost(self, b):
        if b.lost_since is not None:
            return
        try:
            b.ser.close()
        except (self.serial.SerialException, OSError, AttributeError):
            pass
        b.ser, b.lost_since = None, time.monotonic()
        self.on_down(b)
        self.finding(f"{b.name}: serial port {b.port} disappeared", board=b.name)

    def retry_ports(self):
        from serial.tools import list_ports
        now = time.monotonic()
        for b in self.boards:
            if b.lost_since is None or now - b.last_retry < PORT_RETRY_S:
                continue
            b.last_retry = now
            candidates = [b.port]
            if now - b.lost_since > 300:
                # A re-plugged board comes back under a new COM name. Only ports that did not exist at the
                # start are tried, so the harness never opens (and resets) a port that is not a bench board.
                candidates += [p.device for p in list_ports.comports() if p.vid and p.device not in self.known_ports]
            vids = {p.device: p.vid for p in list_ports.comports()}
            for port in candidates:
                try:
                    b.port = port
                    b.set_bridge(vids.get(port, b.vid))
                    self.open(b)
                except (self.serial.SerialException, OSError):
                    continue
                self.say(f"{b.name}: port {port} open again; waiting for its ID")
                self.event("port_back", board=b.name, port=port)
                b.id_seen = None
                break

    # ---- parsing ----

    def handle(self, b, line):
        now = time.monotonic()
        b.last_line_at = now
        if b.silence_step:
            self.event("port_speaks_again", board=b.name, step=b.silence_step)
            b.silence_step = 0
        self.write_raw(f"[{b.name}] {line}")
        if m := IDENTITY.search(line):
            b.id_seen = m.group(1)
            if b.expected_id is None:
                device_id, role, board, node, ap_name, device = m.groups()
                b.id, b.role, b.board, b.ap_name = device_id, role, board, ap_name
                b.index = int(node) if node is not None else int(device) if device is not None else None
                b.name = self.names.get(device_id) or (f"AP-{ap_name}" if ap_name else f"{role}-{device_id}")
            elif b.id_seen != b.expected_id and not b.wrong:
                self.finding(f"{b.name}: port {b.port} now answers as {b.id_seen}; faults on it are stopped",
                             board=b.name)
                b.wrong = True
        elif m := GROUP_ROW.search(line):
            b.groups[m.group(2)] = m.group(3) == "member"
        if CRASH.search(line):
            self.finding(f"{b.name}: crash output: {line.strip()}", board=b.name)
        if (m := RX["ap_boot"].search(line)) or (m := RX["hh_boot"].search(line)):
            ours = now - b.injected_at < FAULT_GRACE_S
            b.boot = int(m.groups()[-1])
            b.boot_at = now
            self.on_down(b)
            self.event("boot", board=b.name, boot=b.boot, injected=ours)
            if not ours:
                self.finding(f"{b.name}: restarted without an injected fault (boot {b.boot})", board=b.name)
        elif m := RX["restart"].search(line):
            reason = m.group(1).strip()
            self.event("restart_reason", board=b.name, reason=reason)
            if reason != "power-on or reset":
                self.finding(f"{b.name}: last restart was '{reason}'", board=b.name)
        elif m := RX["link_up"].search(line):
            b.links.add(int(m.group(1)))
            self.event("link_up", board=b.name, peer=int(m.group(1)))
        elif (m := RX["link_lost"].search(line)) or (m := RX["peer_rebooted"].search(line)):
            b.links.discard(int(m.group(1)))
            self.event("link_down", board=b.name, peer=int(m.group(1)), line=line.split("] ", 1)[-1])
        elif RX["link_kept"].search(line):
            self.link_saves += 1
        elif m := RX["ap_status"].search(line):
            links, handhelds, heap, low, quality = m.groups()
            b.status = {"links": int(links), "handhelds": int(handhelds), "heap": int(heap), "min": int(low),
                        "time": quality}
            b.lowest_heap = int(low) if b.lowest_heap is None else min(b.lowest_heap, int(low))
            self.event("ap_status", board=b.name, **b.status)
        elif m := RX["settings"].search(line):
            b.settings_version = int(m.group(1))
        elif m := RX["registered"].search(line):
            b.registered_node = int(m.group(1))
            self.event("registered", board=b.name, node=b.registered_node)
            if b.failover:
                start, exp = b.failover
                exp["failover"].append({"handheld": b.name, "node": b.registered_node,
                                        "seconds": round(now - start, 1)})
                b.failover = None
        elif m := RX["hh_down"].search(line):
            if b.registered_node is not None:
                self.event("handheld_down", board=b.name, node=b.registered_node, why=m.group(1))
                b.drops += 1
            b.registered_node = None
        elif m := RX["hh_online"].search(line):
            low = int(m.group(2))
            b.lowest_heap = low if b.lowest_heap is None else min(b.lowest_heap, low)
        elif m := RX["not_sent"].search(line):
            p = self.pending.pop(m.group(2), None)
            if p:
                self.resolve(p, "refused", reason=m.group(1))
        elif m := RX["sent_direct"].search(line):
            if p := self.unsent_direct(b, int(m.group(1))):
                p["msg_id"] = (b.index, int(m.group(2)), int(m.group(3)))
                self.direct_ids[p["msg_id"]] = p["token"]
                early = self.early_direct.pop(p["msg_id"], None)
                if early and early[0] == p["receiver"] and p["token"] in self.pending:
                    del self.pending[p["token"]]
                    self.resolve(p, "delivered", latency_s=round(max(0.0, early[1] - p["mono"]), 2))
        elif m := RX["not_sent_direct"].search(line):
            if p := self.unsent_direct(b, int(m.group(2))):
                del self.pending[p["token"]]
                self.resolve(p, "refused", reason=m.group(1))
        elif m := RX["received_direct"].search(line):
            key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            token = self.direct_ids.get(key)
            p = self.pending.get(token)
            if p and p["receiver"] == b.name:
                del self.pending[token]
                self.resolve(p, "delivered", latency_s=round(now - p["mono"], 2))
            elif token is None:
                self.early_direct = {k: v for k, v in self.early_direct.items() if now - v[1] < PROBE_DEADLINE_S}
                self.early_direct[key] = (b.name, now)
        elif m := RX["received"].search(line):
            p = self.pending.pop(m.group(2), None)
            if p and p["receiver"] == b.name:
                self.resolve(p, "delivered", latency_s=round(now - p["mono"], 2))
            elif p:
                self.pending[m.group(2)] = p
        elif m := RX["volume"].search(line):
            if b.volume is None:
                b.volume = m.group(1)
        elif m := RX["time"].search(line):
            self.event("time", board=b.name, text=m.group(1))

    def on_down(self, b):
        """The board lost its state: a restart, a hold, or a lost port."""
        b.links.clear()
        b.status = None
        if b.registered_node is not None or not b.is_ap:
            b.drops += 1
        b.registered_node = None

    def drain(self, seconds=0.05):
        end = time.monotonic() + seconds
        while True:
            try:
                b, ser, line = self.lines.get(timeout=max(0.0, end - time.monotonic()))
            except queue.Empty:
                return
            if line is not None:
                self.handle(b, line)
            elif ser is b.ser:           # a reader of a port already closed and reopened is not news
                self.port_lost(b)

    # ---- steady state ----

    def present(self, b):
        return b.ser is not None and b.lost_since is None and not b.held and not b.wrong

    def unsteady(self):
        """Reasons the grid is not back to normal; empty means steady."""
        why = []
        live_aps = [a for a in self.aps if self.present(a)]
        live_idx = {a.index for a in live_aps}
        for a in live_aps:
            missing = live_idx - {a.index} - a.links
            if missing:
                why.append(f"{a.name} has no link to AP {sorted(missing)}")
            if a.status is None:
                why.append(f"{a.name} has not reported status since it started")
            elif self.time_set and a.status["time"] == "UNSET":
                why.append(f"{a.name} grid time UNSET")
        for h in self.hhs:
            if self.present(h) and h.registered_node not in live_idx:
                why.append(f"{h.name} not registered with a live AP")
        versions = {a.settings_version for a in live_aps if a.settings_version is not None}
        if len(versions) > 1:
            why.append(f"APs disagree on settings version {sorted(versions)}")
        return why

    def backbone_whole(self):
        live = [a for a in self.aps if self.present(a)]
        return all(a.links >= {o.index for o in live} - {a.index} for a in live)

    def wait(self, seconds, until=None):
        end = time.monotonic() + seconds
        while time.monotonic() < end and time.monotonic() < self.deadline:
            self.drain()
            self.background()
            if until and until():
                return True
        return bool(until and until())

    def check_silence(self, now):
        for b in self.boards:
            if not self.present(b):
                continue
            quiet = now - max(b.last_line_at, b.injected_at)
            limit = SILENT_AP_S if b.is_ap else SILENT_HANDHELD_S
            if b.silence_step == 0 and quiet > limit:
                b.silence_step = 1
                self.finding(f"{b.name}: no output for {int(quiet)} s on {b.port}; releasing its reset lines again "
                             "(harness or USB, not yet firmware)", board=b.name, harness=True)
                try:
                    b.ser.rts = False
                    b.ser.dtr = False
                except (self.serial.SerialException, OSError, ValueError):
                    pass
            elif b.silence_step == 1 and quiet > limit + 30:
                b.silence_step = 2
                self.finding(f"{b.name}: still silent after releasing its lines; reopening {b.port}",
                             board=b.name, harness=True)
                self.release_port(b)
                self.on_down(b)
                try:
                    self.open(b)
                    self.pulse(b)
                except (self.serial.SerialException, OSError) as e:
                    self.finding(f"{b.name}: could not reopen {b.port}: {e}", board=b.name, harness=True)
                    b.lost_since = now
                b.last_line_at = time.monotonic()

    def background(self):
        now = time.monotonic()
        self.retry_ports()
        self.check_silence(now)
        self.probe_tick(now)
        if now >= self.next_query:
            self.next_query = now + QUERY_EVERY_S
            for b in self.boards:
                if self.present(b):
                    self.write(b, "nodes" if b.is_ap else "mem")
        if now >= self.next_summary:
            self.next_summary = now + SUMMARY_EVERY_S
            self.write_summary(final=False)

    # ---- probes: the handhelds message each other ----

    def active_faults(self):
        now = time.monotonic()
        return [b.name for b in self.boards if b.held or now - b.injected_at < FAULT_GRACE_S]

    def probe_tick(self, now):
        for token, p in list(self.pending.items()):
            if now - p["mono"] > PROBE_DEADLINE_S:
                del self.pending[token]
                self.resolve(p, "lost")
        if not self.probing or now < self.next_probe or len(self.hhs) < 2:
            return
        self.next_probe = now + self.args.probe_interval * self.rng.uniform(0.7, 1.3)
        senders = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        if not senders:
            return
        sender = self.rng.choice(senders)
        receiver = self.rng.choice([h for h in self.hhs if h is not sender])
        kinds = ["broadcast", "direct"] + (["group"] if self.group else [])
        kind = self.rng.choice(kinds) if self.time_set else "urgent"
        target = {"broadcast": "all", "direct": str(receiver.index), "group": self.group,
                  "urgent": "urgent"}[kind]
        self.probe_n += 1
        token = f"CX{self.run_id}-{self.probe_n:05d}"
        p = {"token": token, "kind": kind, "sender": sender.name, "receiver": receiver.name, "mono": now,
             "t": self.now_s(), "sender_node": sender.registered_node, "receiver_node": receiver.registered_node,
             "faults_at_send": self.active_faults(), "drops": (sender.drops, receiver.drops)}
        if self.write(sender, f"send {target} {token}"):
            self.pending[token] = p

    def unsent_direct(self, sender, target):
        """The oldest direct probe from this sender to this device with no message ID yet. The firmware handles
        'send' commands in order, so the next Sent or Not sent line for that device belongs to this probe."""
        for p in self.pending.values():
            if (p["kind"] == "direct" and p["sender"] == sender.name and "msg_id" not in p
                    and self.by_name[p["receiver"]].index == target):
                return p
        return None

    def resolve(self, p, result, **extra):
        if "msg_id" in p:
            self.direct_ids.pop(p["msg_id"], None)
        sender, receiver = self.by_name[p["sender"]], self.by_name[p["receiver"]]
        p.update(result=result, faults_at_result=self.active_faults(), **extra)
        # Clean: both ends were registered when it was sent and neither lost its AP before the result. Faults
        # elsewhere in the grid do not excuse a loss: carrying messages around a dead AP is what is under test.
        p["clean"] = p["receiver_node"] is not None and p.pop("drops") == (sender.drops, receiver.drops)
        rec = {k: v for k, v in p.items() if k != "mono"}
        self.probes.append(rec)
        self.event("probe", **rec)
        if p["clean"] and result != "delivered":
            self.finding(f"{p['kind']} message {p['token']} {p['sender']} -> {p['receiver']} {result} "
                         f"while both were registered (APs {p['sender_node']} and {p['receiver_node']}"
                         f"{'; faults on ' + ', '.join(p['faults_at_send']) if p['faults_at_send'] else ''})",
                         token=p["token"])

    # ---- the run ----

    def start(self):
        self.say(f"Output in {self.out}")
        self.discover()
        self.event("start", seed=self.args.seed, hours=self.args.hours,
                   boards=[{"slot": b.slot, "name": b.name, "id": b.id, "port": b.port, "role": b.role,
                            "board": b.board, "index": b.index, "bridge": b.bridge, "holdable": b.holdable}
                           for b in self.boards])
        if not self.wait(90, until=self.backbone_whole):
            self.finding("APs did not all link within 90 s of the start")
        if self.args.set_time:
            self.set_time()
        if self.args.quiet_handhelds:
            for h in self.hhs:
                self.write(h, "volume")
            self.wait(3)
            for h in self.hhs:
                self.write(h, "volume off")
        if not self.wait(self.args.recovery_timeout, until=lambda: not self.unsteady()):
            self.finding("Grid not steady before the first experiment: " + "; ".join(self.unsteady()))
        for h in self.hhs:
            self.write(h, "groups")
        self.wait(5, until=lambda: all(h.groups for h in self.hhs))
        shared = [g for g in (self.hhs[0].groups if self.hhs else {}) if all(h.groups.get(g) for h in self.hhs)]
        self.group = shared[0] if shared else None
        self.say(f"Group messages go to {self.group}, which every handheld belongs to" if self.group else
                 "No group has every handheld as a member; group messages are left out")
        self.event("group", group=self.group, groups={h.name: h.groups for h in self.hhs})
        self.probing = True

    def discover(self):
        """Open every USB serial port, reset it, and keep the ones whose board says who it is."""
        from serial.tools import list_ports
        found = []
        for p in sorted(list_ports.comports(), key=lambda p: p.device):
            if not p.vid:
                continue                  # no USB ID: not a bench board (COM3 here is Intel AMT)
            b = Board(p.device, p.vid)
            try:
                self.open(b)
            except (self.serial.SerialException, OSError) as e:
                self.say(f"{p.device}: cannot open ({e}); skipped")
                continue
            found.append(b)
        self.boards = list(found)
        for b in found:
            self.pulse(b)   # a known starting point: every board boots with the harness listening
        self.wait(15, until=lambda: all(b.id for b in found))
        for b in found:
            if not b.id:
                self.write(b, "id")       # a board whose boot banner was missed can still answer
        self.wait(10, until=lambda: all(b.id for b in found))
        for b in found:
            if not b.id:
                self.say(f"{b.port}: no ID yet; pulsing its reset line again (CP210x ports sometimes ignore one)")
                self.pulse(b)
        self.wait(15, until=lambda: all(b.id for b in found))
        wanted = set(self.args.devices or [])
        for b in found:
            keep = b.id and b.role in ("N", "H") and b.index is not None and (
                not wanted or b.name in wanted or b.id in wanted)
            if not keep:
                why = ("gave no LocalGrid ID" if not b.id else "has no AP or device index" if b.index is None
                       else "not in --devices")
                self.say(f"{b.port}: {why}; released and left alone")
                self.release_port(b)
                self.boards.remove(b)
        names = [b.name for b in self.boards]
        if len(names) != len(set(names)):
            raise SystemExit(f"Two boards share a name {names}; nothing was injected")
        self.aps = sorted((b for b in self.boards if b.is_ap), key=lambda b: b.index)
        self.hhs = sorted((b for b in self.boards if not b.is_ap), key=lambda b: b.index)
        self.by_name = {b.name: b for b in self.boards}
        for i, b in enumerate(self.aps):
            b.slot = f"AP{i + 1}"
        for i, b in enumerate(self.hhs):
            b.slot = f"HH{i + 1}"
        for b in self.aps + self.hhs:
            b.expected_id = b.id
            where = f"AP {b.index} {b.ap_name}" if b.is_ap else f"device {b.index}"
            self.say(f"{b.slot}: {b.name} {b.id} on {b.port}, board {b.board}, {where}, {b.bridge}"
                     f"{'' if b.holdable else ' (pulse only: cannot be held)'}")
        if not self.aps:
            raise SystemExit("No AP answered; nothing was injected")

    def release_port(self, b):
        """Both lines low, then close: a plain close holds CP210x and CH34x boards in reset."""
        ser, b.ser = b.ser, None
        if ser is None:
            return
        try:
            ser.dtr = False
            ser.rts = False
            time.sleep(0.1)
        except (self.serial.SerialException, OSError, ValueError):
            pass
        try:
            ser.close()
        except (self.serial.SerialException, OSError):
            pass

    def set_time(self):
        """Set grid time on one AP and wait until an AP reports it. A command typed while the AP is still
        starting is lost without an answer, so it is typed again until a status line shows the time."""
        def time_seen():
            return any(a.status and a.status["time"] != "UNSET" for a in self.aps if self.present(a))

        for attempt in range(1, 5):
            ap = next((a for a in self.aps if self.present(a)), None)
            if ap is None:
                break
            for a in self.aps:
                a.status = None          # only a status line printed after this command counts
            self.write(ap, f"time set {int(time.time())}")
            if self.wait(STATUS_EVERY_S + 10, until=time_seen):
                self.time_set = True
                self.event("time_set", board=ap.name, attempt=attempt)
                return
        self.finding("Grid time could not be set: no AP reported it after 4 attempts")

    def confirm(self, v, what, done, redo, window_s):
        """Wait for a fault to show on the board itself, applying it again up to twice. CP210x ports sometimes
        ignored an RTS change: a pulse that did not reset, a release that left node-main held for minutes."""
        for attempt in range(1, 4):
            if self.wait(window_s, until=done):
                if attempt > 1:
                    self.finding(f"{v.name}: {what} took effect only on attempt {attempt} on {v.port}",
                                 board=v.name, harness=True)
                return True
            if attempt < 3:
                redo()
        self.finding(f"{v.name}: {what} did not take effect after 3 attempts on {v.port}", board=v.name, harness=True)
        return False

    def run_experiment(self, e):
        slots = {b.slot: b for b in self.aps + self.hhs}
        if any(s not in slots for s in e["victims"]):
            self.say(f"#{e['n']} skipped: no board in slot {', '.join(s for s in e['victims'] if s not in slots)}")
            return
        victims = [slots[s] for s in e["victims"]]
        e = {**e, "slots": e["victims"], "victims": [v.name for v in victims]}
        if e["kind"] == "hold" and not all(v.holdable for v in victims):
            e.update(kind="pulse", outage_s=0, note="hold planned, but a victim's port is the chip's own USB")
        if not all(self.present(v) for v in victims):
            self.say(f"#{e['n']} skipped: {', '.join(v.name for v in victims if not self.present(v))} absent")
            return
        exp = {**e, "started": self.now_s(), "failover": [], "result": None}
        self.experiments.append(exp)
        down_idx = {v.index for v in victims if v.is_ap}
        for h in self.hhs:
            if self.present(h) and h.registered_node in down_idx:
                h.failover = (time.monotonic(), exp)
        self.say(f"#{e['n']} {e['kind']} {', '.join(e['victims'])}"
                 + (f" for {e['outage_s']} s" if e["kind"] == "hold" else ""))
        self.event("fault", **{k: v for k, v in e.items() if k != "at_s"})
        blackout = bool(self.aps) and all(a in victims for a in self.aps)
        exp["unconfirmed"] = []
        try:
            for v in victims:
                if e["kind"] == "hold":
                    self.hold(v)
                    if not self.confirm(v, "hold (board still printing)", lambda v=v: time.monotonic() - v.last_line_at > 2.0,
                                        lambda v=v: self.hold(v), 4):
                        exp["unconfirmed"].append(f"{v.name} hold")
                else:
                    t = time.monotonic()
                    self.pulse(v)
                    # Its links, status and registration died with the reset. Kept, they made the grid look
                    # steady 0.1 s after a pulse, before the board had even printed its boot banner.
                    self.on_down(v)
                    if not self.confirm(v, "reset pulse (no boot banner)", lambda v=v, t=t: v.boot_at > t,
                                        lambda v=v: (self.pulse(v), self.on_down(v)), 12):
                        exp["unconfirmed"].append(f"{v.name} pulse")
            if e["kind"] == "hold":
                self.wait(e["outage_s"])
        finally:
            released_at = time.monotonic()
            for v in victims:
                if v.held:
                    self.release_hold(v)
        released = time.monotonic()
        if e["kind"] == "hold":
            for v in victims:
                # A release the port did not apply leaves the chip in reset; toggling the line again frees it.
                if not self.confirm(v, "release (no boot banner)", lambda v=v: v.boot_at > released_at,
                                    lambda v=v: self.pulse(v), 12):
                    exp["unconfirmed"].append(f"{v.name} release")
        self.event("released", n=e["n"], unconfirmed=exp["unconfirmed"])
        if blackout and self.args.set_time:
            self.wait(90, until=self.backbone_whole)
            self.set_time()
        linked = self.wait(self.args.recovery_timeout, until=self.backbone_whole)
        exp["relink_s"] = round(time.monotonic() - released, 1) if linked else None
        # Steady also needs each AP's 30 s status line since its restart, so recovery_s can trail relink_s by 30 s.
        steady = self.wait(max(0.0, released + self.args.recovery_timeout - time.monotonic()),
                           until=lambda: not self.unsteady())
        exp["recovery_s"] = round(time.monotonic() - released, 1)
        exp["result"] = "recovered" if steady else "not recovered"
        for h in self.hhs:
            if h.failover and h.failover[1] is exp:
                h.failover = None
        self.event("recovery", n=e["n"], result=exp["result"], seconds=exp["recovery_s"], failover=exp["failover"],
                   unsteady=self.unsteady())
        if not steady:
            exp["unsteady"] = self.unsteady()
            self.finding(f"#{e['n']} not recovered {self.args.recovery_timeout} s after release: "
                         + "; ".join(exp["unsteady"]), n=e["n"])
        else:
            self.say(f"#{e['n']} recovered in {exp['recovery_s']} s")

    def run(self):
        self.start()
        plan = make_schedule(random.Random(self.args.seed), len(self.aps), len(self.hhs), self.args)
        print_schedule(plan, self.args)
        self.event("schedule", plan=plan)
        run_start = time.monotonic()
        run_end = run_start + self.args.hours * 3600
        for e in plan:
            wait_s = e["at_s"] - (time.monotonic() - run_start)
            if wait_s > 0:
                self.wait(wait_s)
            if time.monotonic() + e["outage_s"] + self.args.recovery_timeout > run_end:
                self.say(f"#{e['n']} and later not started: they could not finish before the planned end")
                break
            if self.unsteady() and not self.wait(self.args.recovery_timeout, until=lambda: not self.unsteady()):
                self.finding(f"#{e['n']} not started: grid still not steady: " + "; ".join(self.unsteady()))
                continue
            self.run_experiment(e)
        self.wait(max(0.0, run_end - time.monotonic()))
        self.next_query = 0
        self.background()
        self.wait(10)

    def finish(self):
        for h in self.hhs:
            if self.args.quiet_handhelds and h.volume and self.present(h):
                self.write(h, f"volume {h.volume.lower()}")
        self.deadline = time.monotonic() + 5
        self.probing = False
        self.wait(2)
        self.event("end")
        self.write_summary(final=True)
        self.say(f"Summary: {self.out / 'summary.md'}")

    # ---- report ----

    def write_summary(self, final):
        exps, probes = self.experiments, self.probes
        clean = [p for p in probes if p["clean"]]
        clean_ok = [p for p in clean if p["result"] == "delivered"]
        lat = sorted(p["latency_s"] for p in clean_ok)
        dirty = [p for p in probes if not p["clean"]]
        fail_s = sorted(f["seconds"] for e in exps for f in e["failover"])
        rec_s = sorted(e["recovery_s"] for e in exps if e.get("result") == "recovered")
        relink = sorted(e["relink_s"] for e in exps if e.get("relink_s") is not None and any(
            self.by_name[n].is_ap for n in e["victims"]))

        def spread(v):
            if not v:
                return "none"
            return (f"median {statistics.median(v):.1f} s, p95 {v[min(len(v) - 1, int(len(v) * 0.95))]:.1f} s, "
                    f"max {v[-1]:.1f} s")

        out = [f"# Chaos run {self.start_wall:%Y-%m-%d %H:%M}", "",
               f"{'Final' if final else 'Interim'} report after {datetime.timedelta(seconds=int(self.now_s()))}. "
               f"Seed {self.args.seed}, planned {self.args.hours} h, boards: {', '.join(b.name for b in self.boards)}.",
               "", "## Result", "",
               "| Measure | Value |", "|---|---|",
               f"| Experiments | {len(exps)}, recovered {sum(e.get('result') == 'recovered' for e in exps)}, "
               f"not recovered {sum(e.get('result') == 'not recovered' for e in exps)} |",
               f"| Backbone whole again after an AP fault | {spread(relink)} |",
               f"| Steady state after release | {spread(rec_s)} |",
               f"| Handheld failover off a lost AP | {len(fail_s)} moves, {spread(fail_s)} |",
               f"| Messages with both ends registered throughout | {len(clean_ok)} of {len(clean)} delivered; "
               f"latency {spread(lat)} |",
               f"| Messages where an end lost its AP | {len(dirty)}: "
               + ", ".join(f"{r} {sum(p['result'] == r for p in dirty)}" for r in ("delivered", "refused", "lost"))
               + " |",
               f"| Backbone links kept by keepalive ACKs | {self.link_saves} |",
               f"| Findings | {len(self.findings)} |", "",
               "## Findings", ""]
        out += [f"- {f['wall']} (t={f['t']} s): {f['text']}" for f in self.findings] or ["None."]
        out += ["", "## Experiments", "", "| # | Start (s) | Fault | Victims | Result | Relink (s) | Steady (s) | Failover |",
                "|---|---|---|---|---|---|---|---|"]
        for e in exps:
            fault = f"hold {e['outage_s']} s" if e["kind"] == "hold" else "pulse"
            fo = "; ".join(f"{f['handheld']} to AP {f['node']} in {f['seconds']} s" for f in e["failover"]) or ""
            out.append(f"| {e['n']} | {e['started']} | {fault} | {', '.join(e['victims'])} | {e.get('result') or 'running'} "
                       f"| {e.get('relink_s') or ''} | {e.get('recovery_s', '')} | {fo} |")
        out += ["", "## Memory", "",
                "Lowest free heap seen during the run: bytes on APs (status line), KB on handhelds ('Online' line). "
                "A lowest value that keeps falling across long uptimes points at a leak; compare raw logs hour by hour.", ""]
        out += [f"- {b.name}: {b.lowest_heap if b.lowest_heap is not None else 'no sample'}" for b in self.boards]
        out += ["", "Raw serial logs: raw-NN.log per hour. Every event: events.jsonl.", ""]
        (self.out / "summary.md").write_text("\n".join(out), encoding="utf-8")


# ---- offline checks ----

SAMPLES = [
    ("ap_boot", "I (812) node: [GRID] LocalGrid node 1 NORTH starting, boot 365", ("1", "NORTH", "365")),
    ("hh_boot", "I (900) hh: [NET] Handheld service started: device 2 (Handheld 2), boot 44", ("2", "44")),
    ("restart", "W (820) node: [GRID] Last restart: low supply voltage (brownout), after 3021 s running",
     ("low supply voltage (brownout)",)),
    ("restart", "I (820) node: [GRID] Last restart: power-on or reset", ("power-on or reset",)),
    ("link_up", "I (2100) BB: [BB] Link up to node 2, RSSI -41, 1 clients there", ("2",)),
    ("link_lost", "W (9000) BB: [BB] Link lost to node 0 (no HELLO for 6012 ms, no keepalive ACK either)", ("0",)),
    ("link_lost", "W (9000) BB: [BB] One-way link to node 2 expired (no HELLO for 6000 ms)", ("2",)),
    ("peer_rebooted", "W (9000) BB: [BB] Node 0 rebooted; link down until confirmed", ("0",)),
    ("link_kept", "W (9000) BB: [BB] No HELLO from node 1 for 3400 ms, but it ACKs keepalives; link kept", ("1",)),
    ("ap_status", "I (30000) node: [GRID] links 2 handhelds 1 heap 81234 min 74400 time CARRIED",
     ("2", "1", "81234", "74400", "CARRIED")),
    ("settings", "I (1000) grid: [GRID] Adopted settings version 3 (made on AP 0) via AP 0", ("3",)),
    ("settings", "I (1000) grid: [GRID] Settings version 1 from AP 0 (set up)", ("1",)),
    ("registered", "I (2800) GRID: [GRID] Registered with node 1 as device 2 (Handheld 2)", ("1", "2")),
    ("hh_down", "W (5000) hh: [NET] The node stopped answering; searching again in 2 s", ("The node stopped answering",)),
    ("hh_online", "I (2900) hh: [NET] Online; free heap 123 KB, lowest 116 KB", ("123", "116")),
    ("not_sent", "W (5000) MSG: [MSG] Not sent (outbox full): CX0130-00012", ("outbox full", "CX0130-00012")),
    ("received", "I (5000) MSG: [MSG] From 1 (Handheld 1) URGENT: CX0130-00013", ("1", "CX0130-00013")),
    ("received", "I (5000) MSG: [MSG] From 2 (Handheld 2): CX0130-00014", ("2", "CX0130-00014")),
    ("sent_direct", "I (5000) MSG: [MSG] Sent to device 2: 1:1 boot 44 seq 7, 12 bytes", ("2", "44", "7")),
    ("sent_direct", "I (5000) MSG: [MSG] Sent URGENT to device 1: 1:1 boot 3 seq 19, 12 bytes", ("1", "3", "19")),
    ("not_sent_direct", "W (5000) MSG: [MSG] Not sent (outbox full): 1:1 to device 2, 12 bytes",
     ("outbox full", "2")),
    ("received_direct", "I (5000) MSG: [MSG] From 1 (Handheld 1): 1:1 boot 44 seq 7, 12 bytes", ("1", "44", "7")),
    ("received_direct", "I (5000) MSG: [MSG] From 2 (Handheld 2) URGENT: 1:1 boot 9 seq 1, 3 bytes", ("2", "9", "1")),
    ("volume", "Volume: medium", ("medium",)),
]


def self_check():
    failures = 0
    for key, line, want in SAMPLES:
        m = RX[key].search(line)
        got = m.groups() if m else None
        if got != want:
            failures += 1
            print(f"FAIL {key}: {line!r} -> {got}, want {want}")
    identities = [
        ("LGID: LG-N-ELG-1ME3HN5EDP role=N board=ELG node=0 MAIN",
         ("LG-N-ELG-1ME3HN5EDP", "N", "ELG", "0", "MAIN", None)),
        ("LGID: LG-H-F4B-9DTCV8M0R1 role=H board=F4B device=1", ("LG-H-F4B-9DTCV8M0R1", "H", "F4B", None, None, "1")),
        ("LGID: LG-H-HY3-MPT765WSFG role=H board=HY3", ("LG-H-HY3-MPT765WSFG", "H", "HY3", None, None, None)),
    ]
    for line, want in identities:
        m = IDENTITY.search(line)
        if (m.groups() if m else None) != want:
            failures += 1
            print(f"FAIL identity: {line!r} -> {m.groups() if m else None}")
    rows = [("  1   FAMILY            member", ("1", "FAMILY", "member")),
            ("  3   LEADERS           not a member", ("3", "LEADERS", "not a member"))]
    for line, want in rows:
        m = GROUP_ROW.search(line)
        if (m.groups() if m else None) != want:
            failures += 1
            print(f"FAIL group row: {line!r}")
    for vid, holdable in ((0x10C4, True), (0x1A86, True), (0x303A, False), (0x1234, False)):
        if Board("COMx", vid).holdable != holdable:
            failures += 1
            print(f"FAIL bridge {vid:04X}: holdable should be {holdable}")
    # Safety rules on many seeds and grid sizes: never every AP without --blackout, never every handheld.
    schedules = 0
    for n_aps, n_hhs in ((3, 2), (2, 2), (4, 3), (1, 2), (3, 1)):
        args = argparse.Namespace(hours=12, min_gap=3, max_gap=15, blackout=False, max_aps_down=2)
        for seed in range(100):
            schedules += 1
            for e in make_schedule(random.Random(seed), n_aps, n_hhs, args):
                v = e["victims"]
                if n_aps > 1 and sum(x.startswith("AP") for x in v) >= n_aps:
                    failures += 1
                    print(f"FAIL {n_aps} APs seed {seed} #{e['n']}: every AP out without --blackout")
                if n_aps == 1 and any(x.startswith("AP") for x in v):
                    failures += 1
                    print(f"FAIL 1 AP seed {seed} #{e['n']}: the only AP taken out")
                if n_hhs > 1 and sum(x.startswith("HH") for x in v) >= n_hhs:
                    failures += 1
                    print(f"FAIL {n_hhs} handhelds seed {seed} #{e['n']}: every handheld out")
    print(f"self-check: {len(SAMPLES) + len(identities) + len(rows) + 4} parser and bridge samples, "
          f"{schedules} schedules, {failures} failure(s)")
    return 1 if failures else 0


def keep_awake():
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)   # ES_CONTINUOUS | ES_SYSTEM_REQUIRED


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--live", action="store_true", help="open the ports and inject faults (default: print the schedule)")
    ap.add_argument("--self-check", action="store_true", help="check parsers and schedule rules offline, then exit")
    ap.add_argument("--hours", type=float, default=10)
    ap.add_argument("--seed", type=int, help="schedule seed (default: from the clock, printed so a run can be replayed)")
    ap.add_argument("--devices", nargs="+", help="bench names or device IDs to use (default: every board that answers)")
    ap.add_argument("--aps", type=int, default=3, help="APs assumed by a schedule-only run; a live run counts them")
    ap.add_argument("--handhelds", type=int, default=2, help="handhelds assumed by a schedule-only run")
    ap.add_argument("--min-gap", type=float, default=3, help="minutes between experiments, lower bound")
    ap.add_argument("--max-gap", type=float, default=15, help="minutes between experiments, upper bound")
    ap.add_argument("--max-aps-down", type=int, default=2, help="APs out at once; capped at all APs but one")
    ap.add_argument("--blackout", action="store_true", help="allow every AP out at once (grid time is lost and set again)")
    ap.add_argument("--recovery-timeout", type=float, default=180, help="seconds after release to reach steady state")
    ap.add_argument("--probe-interval", type=float, default=45, help="seconds between handheld messages")
    ap.add_argument("--no-set-time", dest="set_time", action="store_false",
                    help="do not set grid time from the PC clock at the start (1:1 and group need it, D6)")
    ap.add_argument("--quiet-handhelds", action="store_true",
                    help="set handheld volume off for the run and restore it at the end")
    args = ap.parse_args()

    if args.self_check:
        return self_check()
    if args.seed is None:
        args.seed = int(time.time()) % 100000
    if not args.live:
        print_schedule(make_schedule(random.Random(args.seed), args.aps, args.handhelds, args), args)
        print(f"\nSchedule only, assuming {args.aps} APs and {args.handhelds} handhelds. Nothing was opened. "
              "A live run finds the boards, fills the slots in AP and device index order, and schedules again "
              "with the same seed.")
        return 0

    try:
        names = {d["id"]: d["name"] for d in load_map().get("devices", []) if d.get("id")}
    except (OSError, ValueError):
        names = {}
    acquire_lock()   # the same lock as flash.py: no flash can start while the grid is under test
    keep_awake()
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, signal.default_int_handler)   # console window closed
    chaos = Chaos(args, names)
    try:
        chaos.run()
    except KeyboardInterrupt:
        chaos.say("Stopped by Ctrl+C")
    finally:
        try:
            chaos.finish()
        finally:
            chaos.close_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())
