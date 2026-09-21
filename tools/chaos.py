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
  python tools/chaos.py --hours 4 --live --lora --no-alerts --handhelds 0   # night run with LoRa rounds

LoRa rounds (D71, docs/lora.md) use the AP console's own self-restoring hooks rather than the reset
line: `bb off`, `lora off`, `lora reset`. They need no handheld, because traffic during a round is
the AP console's `ping` echo flooded between APs. With no handheld on USB the whole run falls back
to those AP-to-AP echoes, and the report says so.

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
import secrets
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

# An AP-to-AP echo is one 288-byte frame: under a millisecond over ESP-NOW, about 3 s of airtime over
# LoRa plus a random backoff and a possible retry, so it gets a far longer deadline than a handheld
# message. A frame that has not crossed in this long has not crossed.
ECHO_DEADLINE_S = 40
ECHO_EVERY_S = 20              # echoes during a LoRa window: often enough to measure, not enough to fill the air
LORA_QUERY_S = 10              # seconds to wait for a `lora` status block on the ports already open
LORA_RECOVERY_S = 150          # heartbeats are every 30 s; peers back well inside this

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
    "ap_refused_direct": re.compile(r"\[MSG\] Refused by the AP \(([^)]*)\): 1:1 to (\d+) boot (\d+) seq (\d+)"),
    "received_direct": re.compile(r"\[MSG\] From (\d+) .*: 1:1 boot (\d+) seq (\d+)"),
    "volume": re.compile(r"Volume: (\w+)"),
    "time": re.compile(r"\[TIME\] (.+)"),
    # AP-to-AP traffic: the console's own diagnostic echo (`ping <token>`), flooded like any frame, so it
    # travels by whichever backbone is available. 1:1 text is never logged, but an echo is diagnostic.
    "echo_sent": re.compile(r"Echo sent to (\d+) link\(s\): (CX\S+)"),
    "echo_not_sent": re.compile(r"Echo not sent(?: \((\d+) links\))?: (CX\S+)"),
    "echo_rx": re.compile(r"\[BB\] Echo from node (\d+) after (\d+) hop\(s\): (CX\S+)"),
    # The self-restoring console hooks (docs/lora.md, "Chaos testing it").
    "bb_off": re.compile(r"\[BB\] ESP-NOW off for (\d+) s"),
    "bb_on": re.compile(r"\[BB\] ESP-NOW back on"),
    "lora_off": re.compile(r"\[LORA\] Off for (\d+) s"),
    "lora_on": re.compile(r"\[LORA\] Back on"),
    "lora_resetting": re.compile(r"\[LORA\] Resetting the module"),
    "lora_configured": re.compile(r"\[LORA\] Configured ([^:]+): address (\S+?), network (\S+?),"),
    "lora_link_up": re.compile(r"\[LORA\] Link up to AP (\d+), RSSI (-?\d+) dBm, SNR (-?\d+)"),
    "lora_link_down": re.compile(r"\[LORA\] Link down to AP (\d+) \(([^)]*)\)"),
    "lora_gone": re.compile(r"\[LORA\] The module stopped answering"),
}

# The `lora` status block, as the AP console prints it (firmware/node/main/lora.c, lora_print).
# These are printf lines, not ESP_LOG lines, so they carry no "I (1234) TAG:" prefix; a line may still
# have the console prompt in front of it.
LORA_STATUS = {
    "header": re.compile(r"LoRa \(D71\): (.+?), UART1 rx GPIO(-?\d+) tx GPIO(-?\d+)"),
    "absent": re.compile(r"LoRa: (not started|no module on this AP)"),
    "radio": re.compile(r"address (\d+), network (\d+), (\d+) Hz, SF(\d+) BW(\d+) CR4/(\d+), (-?\d+) dBm, "
                        r"broadcast (on|off)"),
    "last": re.compile(r"last frame (\d+) ms ago, RSSI (-?\d+) dBm, SNR (-?\d+)"),
    "quiet": re.compile(r"nothing received yet|no peer heard yet"),
    "peer": re.compile(r"^(\d+)\s+(UP|DOWN)\s+(-?\d+)\s+(-?\d+)\s+(\d+)$"),
    "frames": re.compile(r"frames out (\d+) in (\d+) \((\d+) of them arrived here first\)"),
    "parts": re.compile(r"parts out (\d+) in (\d+) dropped (\d+) \| reassembly timeouts (\d+) \| too large (\d+)"),
    "queue": re.compile(r"seal/open failures (\d+) \| queue (\d+) \(high (\d+), dropped (\d+)\) \| retries (\d+)"
                        r" \| airtime (\d+) ms \| module restarts (\d+)"),
}
# Counters the firmware adds up since that AP booted, so a restart zeroes them: only these are differenced.
LORA_COUNTERS = ("frames_out", "frames_in", "frames_first", "parts_out", "parts_in", "parts_dropped",
                 "reasm_timeouts", "refused_big", "seal_fail", "queue_dropped", "retries", "airtime_ms",
                 "restarts")


def parse_lora_line(line, st, peers=None):
    """One line of a `lora` status block into st (and a peer row into peers). True if it was one.

    Pure, so --self-check exercises it on sample lines with no board attached."""
    text = re.sub(r"^\s*(?:grid>\s*)?", "", line).strip()
    if m := LORA_STATUS["absent"].search(text):
        st.update(fitted=False, state=m.group(1), off=False, done=True)
        return True
    if m := LORA_STATUS["header"].search(text):
        state = m.group(1)
        st.update(state=state, off="[OFF" in state, fitted="fitted" in state and "no module" not in state,
                  rx_pin=int(m.group(2)), tx_pin=int(m.group(3)), done=False)
        return True
    if m := LORA_STATUS["radio"].search(text):
        st.update(address=int(m.group(1)), network=int(m.group(2)), sf=int(m.group(4)), bw=int(m.group(5)),
                  power_dbm=int(m.group(7)), broadcast=m.group(8) == "on")
        return True
    if m := LORA_STATUS["last"].search(text):
        st.update(heard_age_ms=int(m.group(1)), rssi=int(m.group(2)), snr=int(m.group(3)))
        return True
    if LORA_STATUS["quiet"].search(text):
        return True
    if m := LORA_STATUS["peer"].search(text):
        if peers is not None:
            peers[int(m.group(1))] = {"up": m.group(2) == "UP", "rssi": int(m.group(3)),
                                      "snr": int(m.group(4)), "age_ms": int(m.group(5))}
        return True
    if m := LORA_STATUS["frames"].search(text):
        st.update(frames_out=int(m.group(1)), frames_in=int(m.group(2)), frames_first=int(m.group(3)))
        return True
    if m := LORA_STATUS["parts"].search(text):
        st.update(parts_out=int(m.group(1)), parts_in=int(m.group(2)), parts_dropped=int(m.group(3)),
                  reasm_timeouts=int(m.group(4)), refused_big=int(m.group(5)))
        return True
    if m := LORA_STATUS["queue"].search(text):
        st.update(seal_fail=int(m.group(1)), queue_depth=int(m.group(2)), queue_high=int(m.group(3)),
                  queue_dropped=int(m.group(4)), retries=int(m.group(5)), airtime_ms=int(m.group(6)),
                  restarts=int(m.group(7)), done=True)   # the last line of the block
        return True
    return False
# USB vendor IDs of serial bridges that stay enumerated while the ESP32 behind them is held in reset.
SEPARATE_BRIDGES = {0x10C4: "CP210x", 0x1A86: "CH34x", 0x0403: "FTDI"}
NATIVE_USB = {0x303A: "Espressif native USB"}
def time_source(text, board):
    """Where an AP's [TIME] line says its grid time came from, when that is outside the grid: its GPS
    (D63), a handheld's clock or live GPS fix (D53, D60, D65), or the PC. None for a time carried from
    another AP, which says nothing about how the grid as a whole got it back."""
    if re.search(r"Grid time \d+ from GPS", text):
        return f"the GPS on {board}"
    if m := re.search(r"Took grid time \d+ from handheld (\d+)", text):
        return f"handheld {m.group(1)}'s clock"
    if re.search(r"Grid time set to \d+", text):
        return "the PC"
    return None


IDENTITY = re.compile(r"LGID: (\S+) role=(\w+) board=(\w+)(?: node=(\d+) (\S+))?(?: device=(\d+))?")
GROUP_ROW = re.compile(r"^\s*(\d+)\s+(\S+)\s+(member|not a member)\s*$")
# Printed by every role, so grid time is observable with no AP on USB (handheld-only runs).
GRID_TIME = re.compile(r"\[TIME\] Grid time (\d{10,})")
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")   # D21: never write a hardware address


# ---- schedule: pure, seeded, and the same with or without --live ----

def outage_seconds(rng):
    r = rng.random()
    if r < 0.5:
        return round(rng.uniform(5, 30))
    if r < 0.8:
        return round(rng.uniform(30, 120))
    return round(rng.uniform(120, 600))


LORA_KINDS = ("lora_only", "lora_down", "lora_wedge", "lora_blind")
LORA_WHAT = {
    "lora_only": "ESP-NOW off on every named AP: anything that still crosses went by LoRa",
    "lora_down": "LoRa off on one AP while Wi-Fi stays up: nothing may be lost and nothing may stall",
    "lora_wedge": "the module reset under it: does it come back configured, with its peers, and how fast",
    "lora_blind": "ESP-NOW off on two APs and LoRa off on one of them: one AP genuinely isolated",
}


def lora_window_seconds(rng):
    """A LoRa round's window. One 288-byte frame is about 3 s of airtime and the heartbeat is every 30 s,
    so a window that proves anything is minutes, not the seconds a reset outage takes."""
    return round(rng.uniform(90, 300))


def lora_round(rng, aps):
    """Which LoRa hook, on which APs. Victims are always APs: the hooks are AP console commands."""
    kinds = ["lora_down", "lora_wedge"]
    if len(aps) >= 2:
        kinds += ["lora_only", "lora_only", "lora_blind"]     # the one that proves LoRa carries, twice as often
    kind = rng.choice(kinds)
    victims = rng.sample(aps, 2) if kind in ("lora_only", "lora_blind") else [rng.choice(aps)]
    return {"victims": victims, "kind": kind,
            "outage_s": 0 if kind == "lora_wedge" else lora_window_seconds(rng)}


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
        if aps and getattr(args, "lora", False) and rng.random() < getattr(args, "lora_share", 0.0):
            # A LoRa round takes no board out: the APs stay powered and the hooks restore themselves,
            # so the AP count rules above do not apply to it.
            e = lora_round(rng, aps)
            plan.append({"n": len(plan) + 1, "at_s": round(t), **e})
            t += e["outage_s"] + 120     # LoRa peers come back on a 30 s heartbeat; leave room for it
            continue
        if args.scenario == "blackout":
            victims = list(aps)          # every experiment is the whole grid going away and coming back
        elif args.blackout and aps and r < 0.05:
            victims = list(aps)
        elif max_down >= 2 and r < 0.20:
            victims = rng.sample(aps, 2)
        elif max_down >= 1 and (r < 0.75 or len(hhs) < 2):
            victims = [rng.choice(aps)]
        elif len(hhs) >= 2:
            victims = [rng.choice(hhs)]       # never every handheld: probes need a sender and a receiver
        else:
            continue
        kind = "hold" if args.scenario == "blackout" or rng.random() < 0.7 else "pulse"
        outage = outage_seconds(rng) if kind == "hold" else 0
        plan.append({"n": len(plan) + 1, "at_s": round(t), "victims": list(victims),
                     "kind": kind, "outage_s": outage})
        t += outage + 60


def print_schedule(plan, args):
    print(f"Seed {args.seed}, {args.hours} h, gaps {args.min_gap}-{args.max_gap} min, "
          f"{'blackout allowed' if args.blackout else f'at most {args.max_aps_down} AP(s) down'}"
          + (f", {getattr(args, 'lora_share', 0):.0%} LoRa rounds" if getattr(args, "lora", False) else ""))
    for e in plan:
        at = str(datetime.timedelta(seconds=e["at_s"]))
        what = ("pulse" if e["kind"] == "pulse" else f"{e['kind']} {e['outage_s']} s" if e["outage_s"]
                else e["kind"])
        print(f"  #{e['n']:<3} {at:>8}  {what:<18} {', '.join(e['victims'])}")
    holds = [e["outage_s"] for e in plan if e["kind"] == "hold"]
    loras = [e for e in plan if e["kind"] in LORA_KINDS]
    extra = ""
    if loras:
        counts = ", ".join(f"{k} {sum(e['kind'] == k for e in loras)}" for k in LORA_KINDS
                           if any(e["kind"] == k for e in loras))
        extra = (f", {len(loras)} LoRa rounds ({counts}; total "
                 f"{sum(e['outage_s'] for e in loras) // 60} min of windows)")
    print(f"{len(plan)} experiments: {len(holds)} holds (total {sum(holds) // 60} min out), "
          f"{sum(e['kind'] == 'pulse' for e in plan)} pulses{extra}")


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
        self.grid_time = None            # last "[TIME] Grid time N" this board printed, from any role
        self.settings_version = None
        self.lowest_heap = None          # bytes on APs, KB on handhelds (as the firmware prints them)
        self.free_heap = None            # the last free-heap sample, same units
        self.lora = {}                   # AP: the last `lora` status block, parsed
        self.lora_peers = {}             # AP index -> {up, rssi, snr, age_ms} from that block
        self.lora_links = set()          # AP indices LoRa reports up, from the [LORA] Link up/down lines
        self.lora_off = None             # True/False once a hook has logged; None until then
        self.bb_off = None
        self.lora_reset_at = None        # monotonic time of the last "Resetting the module"
        self.lora_configured_at = None   # monotonic time of the last "[LORA] Configured ..."
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
        self.echoes = []                  # AP-to-AP diagnostic echoes, resolved
        self.echo_pending = {}            # token -> echo in flight
        self.phase = None                 # the experiment echoes belong to while one is running
        self.lora_last = {}               # board name -> the last `lora` status read, for the report
        self.lora_start = {}              # the same, read once before the first experiment
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
        self.time_source = None   # where the grid last got its time from outside itself; see time_source()
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
        if m := GRID_TIME.search(line):
            # Every role prints this, so grid time can be observed without an AP on USB. That is the
            # difference between a handheld-only run sending ordinary messages and sending nothing at
            # all: with no evidence of grid time, D6 leaves a handheld only urgent messages to send.
            b.grid_time = int(m.group(1))
        if CRASH.search(line):
            self.finding(f"{b.name}: crash output: {line.strip()}", board=b.name)
        if b.is_ap and parse_lora_line(line, b.lora, b.lora_peers):
            b.lora["at"] = self.now_s()
            return                       # a `lora` status line is nothing else
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
            b.free_heap = int(heap)
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
        elif m := RX["ap_refused_direct"].search(line):
            # The AP refused it (for example offline: it has never heard of the receiver). A refusal while both
            # ends are registered is still a finding, but it is not a message lost on the way (2026-09-18).
            token = self.direct_ids.get((b.index, int(m.group(3)), int(m.group(4))))
            p = self.pending.get(token)
            if p:
                del self.pending[token]
                self.resolve(p, "refused by the AP", reason=m.group(1))
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
        elif m := RX["echo_rx"].search(line):
            p = self.echo_pending.get(m.group(3))
            if p and b.name in p["receivers"] and b.name not in p["got"]:
                p["got"][b.name] = round(now - p["mono"], 2)
                p["hops"][b.name] = int(m.group(2))
                if len(p["got"]) == len(p["receivers"]):
                    del self.echo_pending[p["token"]]
                    self.resolve_echo(p)
        elif m := RX["echo_sent"].search(line):
            if p := self.echo_pending.get(m.group(2)):
                p["links_at_send"] = int(m.group(1))
        elif m := RX["echo_not_sent"].search(line):
            if p := self.echo_pending.pop(m.group(2), None):
                p["links_at_send"] = int(m.group(1)) if m.group(1) else None
                self.resolve_echo(p, refused=True)
        elif m := RX["bb_off"].search(line):
            b.bb_off = True
            self.event("bb_off", board=b.name, seconds=int(m.group(1)))
        elif RX["bb_on"].search(line):
            b.bb_off = False
            self.event("bb_on", board=b.name)
        elif m := RX["lora_off"].search(line):
            b.lora_off = True
            b.lora_links.clear()
            self.event("lora_off", board=b.name, seconds=int(m.group(1)))
        elif RX["lora_on"].search(line):
            b.lora_off = False
            self.event("lora_on", board=b.name)
        elif RX["lora_resetting"].search(line):
            b.lora_reset_at = now
            b.lora_links.clear()
            self.event("lora_resetting", board=b.name)
        elif m := RX["lora_configured"].search(line):
            b.lora_configured_at = now
            self.event("lora_configured", board=b.name, address=m.group(2), network=m.group(3))
        elif m := RX["lora_link_up"].search(line):
            b.lora_links.add(int(m.group(1)))
            self.event("lora_link_up", board=b.name, peer=int(m.group(1)), rssi=int(m.group(2)),
                       snr=int(m.group(3)))
        elif m := RX["lora_link_down"].search(line):
            b.lora_links.discard(int(m.group(1)))
            self.event("lora_link_down", board=b.name, peer=int(m.group(1)), why=m.group(2))
        elif RX["lora_gone"].search(line):
            self.finding(f"{b.name}: its LoRa module stopped answering", board=b.name)
        elif m := RX["volume"].search(line):
            if b.volume is None:
                b.volume = m.group(1)
        elif m := RX["time"].search(line):
            self.event("time", board=b.name, text=m.group(1))
            source = time_source(m.group(1), b.name)
            if source is not None and self.time_source is None:
                self.time_source = source   # the first source to give the grid time since it was cleared

    def on_down(self, b):
        """The board lost its state: a restart, a hold, or a lost port."""
        b.links.clear()
        b.status = None
        # A restart clears both chaos hooks and zeroes every LoRa counter; the links start again from nothing.
        b.lora_links.clear()
        b.bb_off = b.lora_off = None
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
            if not self.present(h):
                continue
            if not self.aps:
                # No AP is on USB, so this run cannot know which AP indices are alive. A handheld
                # that is registered with some AP is as much as can be checked, and is enough:
                # the APs it is choosing between are exactly what is under test.
                if h.registered_node is None:
                    why.append(f"{h.name} not registered with any AP")
            elif h.registered_node not in live_idx:
                why.append(f"{h.name} not registered with a live AP")
        versions = {a.settings_version for a in live_aps if a.settings_version is not None}
        if len(versions) > 1:
            why.append(f"APs disagree on settings version {sorted(versions)}")
        return why

    def grid_has_time(self):
        """True once any board that is up reports grid time it did not get from the PC.

        An AP says so in its status line. When no AP is on USB the handhelds are the only witnesses,
        and a handheld that prints grid time has it from the grid, never from this tool.
        """
        if any(self.present(a) and a.status is not None and a.status["time"] != "UNSET" for a in self.aps):
            return True
        return any(self.present(h) and h.grid_time for h in self.hhs)

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
                    if b.is_ap:
                        self.write(b, "lora")   # an hourly LoRa sample in the raw log, on the port already open
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
        for token, p in list(self.echo_pending.items()):
            if now - p["mono"] > ECHO_DEADLINE_S:
                del self.echo_pending[token]
                self.resolve_echo(p)
        if not self.probing or now < self.next_probe:
            return
        self.next_probe = now + self.args.probe_interval * self.rng.uniform(0.7, 1.3)
        if len(self.hhs) < 2:
            # No two handhelds on USB: the grid's traffic under test is the APs' own diagnostic echo.
            self.echo_probe()
            return
        self.handheld_probe(now)

    def echo_probe(self):
        """One AP floods a diagnostic echo (`ping <token>`); every other live AP should log it. This is
        AP-to-AP traffic, not a handheld message: it is what a LoRa round counts, and what the run falls
        back to when no handheld is reachable. It is not an alert, so --no-alerts allows it."""
        live = [a for a in self.aps if self.present(a)]
        if len(live) < 2:
            return None
        sender = self.rng.choice(live)
        self.probe_n += 1
        token = f"CX{self.run_id}-{self.probe_n:05d}"
        p = {"token": token, "kind": "echo", "sender": sender.name, "sender_node": sender.index,
             "receivers": [a.name for a in live if a is not sender], "got": {}, "hops": {},
             "mono": time.monotonic(), "t": self.now_s(), "phase": self.phase,
             "faults_at_send": self.active_faults()}
        self.echo_pending[token] = p     # before the write: a reply is never later than the command
        if not self.write(sender, f"ping {token}"):
            del self.echo_pending[token]
            return None
        return p

    def resolve_echo(self, p, refused=False):
        p["crossed"] = len(p["got"])
        p["result"] = ("refused" if refused else "delivered" if p["crossed"] == len(p["receivers"])
                       else "partial" if p["crossed"] else "lost")
        p["latency_s"] = max(p["got"].values()) if p["got"] else None
        rec = {k: v for k, v in p.items() if k != "mono"}
        self.echoes.append(rec)
        self.event("echo", **rec)
        if p["result"] != "delivered" and p["phase"] is None and not p["faults_at_send"]:
            self.finding(f"AP echo {p['token']} from {p['sender']} {p['result']} with no fault anywhere "
                         f"(reached {sorted(p['got'])} of {p['receivers']})", token=p["token"])

    def handheld_probe(self, now):
        senders = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        if not senders:
            return
        sender = self.rng.choice(senders)
        receiver = self.rng.choice([h for h in self.hhs if h is not sender])
        # --no-alerts (a night run): announcements and urgent broadcasts take over every handheld's screen,
        # and urgent ones sound even with the volume off (D40), so only 1:1 and group messages are sent, and
        # none at all while grid time is unset (then urgent is all a handheld may send, D6).
        if self.args.no_alerts and not self.time_set:
            return
        kinds = (["direct"] if self.args.no_alerts else ["broadcast", "direct"]) + (["group"] if self.group else [])
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
        if len(self.hhs) < 2:
            self.say("Fewer than two handhelds on USB: every message under test is the APs' own "
                     "diagnostic echo (`ping`) between APs. No handheld is driven and none is restarted.")
        self.lora_start = self.lora_query()
        for name, s in self.lora_start.items():
            self.say(f"{name} LoRa: {s.get('state', 'no answer')}"
                     + (f", address {s['address']} network {s['network']}" if s.get("address") is not None else "")
                     + (f", peers {sorted(s['peers'])}" if s.get("peers") else ", no peer heard yet"))
            if not s.get("fitted"):
                self.finding(f"{name}: no LoRa module answered at the start ({s.get('state', 'no answer')}); "
                             "LoRa rounds on it can prove nothing", board=name)
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
        if not self.aps and len(self.hhs) < 2:
            raise SystemExit("No AP and fewer than two handhelds answered; nothing could be injected")
        if not self.aps:
            # The mirror of the no-handheld case below. APs running on battery in other rooms are a
            # real bench topology (2026-09-21), and a run can still take handhelds out and measure
            # whether messages keep flowing and handhelds re-register. What it cannot test is AP
            # faults, the backbone, or LoRa rounds, and the report says so rather than implying the
            # grid survived something it was never asked to survive.
            self.say(f"No AP is on USB: {len(self.hhs)} handhelds only. Handheld faults and messages "
                     "are under test; AP outages, backbone healing and LoRa rounds are not.")

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
            return self.grid_has_time()

        # A GPS on MAIN (D63) or a handheld's clock usually gives the grid time within seconds; the PC
        # typing `time set` then only fights it (MAIN refuses while its GPS has a fix, another AP
        # would accept and hand-set time until MAIN's next fix took it back).
        if self.wait(STATUS_EVERY_S + 30, until=time_seen):
            self.time_set = True
            self.event("time_from_grid", source=self.time_source or "unknown")
            self.say(f"grid time came from {self.time_source or 'the grid'}; the PC did not set it")
            return
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
        if e["kind"] in LORA_KINDS:
            return self.run_lora_experiment(e)
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
        if blackout:
            self.time_source = None   # every AP loses its time: watch where the grid gets it back
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
        if blackout:
            # D53 says the handhelds carry grid time back to an AP that restarted without it. This is the
            # scenario that tests it, so the run watches for that before falling back to the PC's clock, and
            # records which of the two put the grid back in business.
            self.wait(90, until=self.backbone_whole)
            carried = self.wait(self.args.time_recovery, until=self.grid_has_time)
            self.event("time_after_blackout", carried=bool(carried), source=self.time_source,
                       waited_s=round(self.args.time_recovery if not carried else 0, 1))
            if carried:
                self.say(f"grid time came back from {self.time_source or 'the grid'} (D53, D63, D65)")
            else:
                self.finding("grid time did not come back after every AP restarted: no GPS fix and no handheld "
                             "clock (D53, D63, D65); the PC set it instead")
                if self.args.set_time:
                    self.set_time()
        self.settle(exp, released)

    def settle(self, exp, released):
        """Wait for the backbone and then for steady state, and record how long each took."""
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
        self.event("recovery", n=exp["n"], result=exp["result"], seconds=exp["recovery_s"],
                   failover=exp["failover"], unsteady=self.unsteady())
        if not steady:
            exp["unsteady"] = self.unsteady()
            self.finding(f"#{exp['n']} not recovered {self.args.recovery_timeout} s after release: "
                         + "; ".join(exp["unsteady"]), n=exp["n"])
        else:
            self.say(f"#{exp['n']} recovered in {exp['recovery_s']} s")
        return steady

    # ---- LoRa rounds (D71): console hooks, not the reset line ----

    def lora_query(self, boards=None):
        """Read `lora` on the ports this run already holds open. tools/console.py would open the port,
        which resets the board and zeroes every counter, so it is never used during a run."""
        boards = [b for b in (boards if boards is not None else self.aps) if self.present(b)]
        for b in boards:
            b.lora.pop("done", None)
            b.lora_peers.clear()
            self.write(b, "lora")
        self.wait(LORA_QUERY_S, until=lambda: all(b.lora.get("done") for b in boards))
        snap = {}
        for b in boards:
            snap[b.name] = {**b.lora, "boot": b.boot, "peers": dict(b.lora_peers),
                            "stale": not b.lora.get("done")}
            self.lora_last[b.name] = snap[b.name]
        missing = [n for n, s in snap.items() if s["stale"]]
        if missing:
            self.event("lora_status_missing", boards=missing)
        return snap

    @staticmethod
    def lora_delta(before, after):
        """Counters are cumulative since that AP booted, so a boot change means they were zeroed and
        nothing can be attributed to this experiment: the row says so instead of reporting a negative."""
        rows = {}
        for name, aft in after.items():
            bef = before.get(name) or {}
            restarted = bef.get("boot") != aft.get("boot") or not bef
            row = {"restarted": bool(restarted)}
            for k in LORA_COUNTERS:
                now_v, was_v = aft.get(k), bef.get(k)
                if now_v is None or (not restarted and was_v is None):
                    row[k] = None
                elif restarted:
                    row[k] = None          # since the restart only; not this experiment's
                else:
                    row[k] = max(0, now_v - was_v)
            row["queue_high"] = aft.get("queue_high")
            row["peers"] = aft.get("peers", {})
            rows[name] = row
        return rows

    def lora_peers_back(self, victims):
        """Every victim hears every other live AP over LoRa again."""
        live = {a.index for a in self.aps if self.present(a)}
        return all(v.lora_links >= live - {v.index} for v in victims)

    def echo_burst(self, seconds, every=ECHO_EVERY_S):
        """Keep AP-to-AP echoes going for a window, then let the last of them land."""
        end = time.monotonic() + seconds
        while time.monotonic() < end and time.monotonic() < self.deadline:
            self.echo_probe()
            self.wait(min(every, max(0.0, end - time.monotonic())))
        self.wait(ECHO_DEADLINE_S + 5, until=lambda: not self.echo_pending)

    def run_lora_experiment(self, e):
        """A LoRa round. No board is taken out: the AP console's own hooks are used, every one of which
        restores itself from the AP's clock, so a dead harness cannot leave a radio off."""
        slots = {b.slot: b for b in self.aps + self.hhs}
        victims = [slots[s] for s in e["victims"] if s in slots]
        if len(victims) != len(e["victims"]) or not all(v.is_ap and self.present(v) for v in victims):
            self.say(f"#{e['n']} skipped: {', '.join(e['victims'])} not all present APs")
            return
        e = {**e, "slots": e["victims"], "victims": [v.name for v in victims]}
        window = e["outage_s"]
        restore_s = min(3600, int(window) + 120)     # the AP puts it back itself even if this tool dies
        exp = {**e, "started": self.now_s(), "failover": [], "result": None, "unconfirmed": [],
               "what": LORA_WHAT[e["kind"]], "window_s": window}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        self.say(f"#{e['n']} {e['kind']} on {', '.join(e['victims'])}"
                 + (f" for {window} s" if window else "") + f": {exp['what']}")
        self.event("fault", **{k: v for k, v in e.items() if k != "at_s"})
        before = self.lora_query()
        exp["lora_before"] = {n: {k: v for k, v in s.items() if k != "peers"} for n, s in before.items()}
        try:
            if e["kind"] == "lora_only":
                for v in victims:
                    self.write(v, f"bb off {restore_s}")
                self.wait(6, until=lambda: all(v.bb_off for v in victims))
                exp["unconfirmed"] += [f"{v.name} bb off" for v in victims if not v.bb_off]
            elif e["kind"] == "lora_down":
                self.write(victims[0], f"lora off {restore_s}")
                self.wait(6, until=lambda: victims[0].lora_off)
                if not victims[0].lora_off:
                    exp["unconfirmed"].append(f"{victims[0].name} lora off")
            elif e["kind"] == "lora_blind":
                for v in victims:
                    self.write(v, f"bb off {restore_s}")
                self.write(victims[0], f"lora off {restore_s}")
                self.wait(6, until=lambda: all(v.bb_off for v in victims) and victims[0].lora_off)
                exp["unconfirmed"] += [f"{v.name} bb off" for v in victims if not v.bb_off]
                if not victims[0].lora_off:
                    exp["unconfirmed"].append(f"{victims[0].name} lora off")
            elif e["kind"] == "lora_wedge":
                v = victims[0]
                v.lora_configured_at = None
                asked = time.monotonic()
                self.write(v, "lora reset")
                back = self.wait(LORA_RECOVERY_S, until=lambda: v.lora_configured_at is not None)
                exp["reconfigured_s"] = round(v.lora_configured_at - asked, 1) if back else None
                if not back:
                    self.finding(f"#{e['n']} {v.name}: the module did not report itself configured "
                                 f"{LORA_RECOVERY_S} s after `lora reset`", n=e["n"], board=v.name)
                peers = self.wait(LORA_RECOVERY_S, until=lambda: self.lora_peers_back([v]))
                exp["peers_back_s"] = round(time.monotonic() - asked, 1) if peers else None
                if not peers:
                    self.finding(f"#{e['n']} {v.name}: its LoRa peers did not come back within "
                                 f"{LORA_RECOVERY_S} s of the reset (heard {sorted(v.lora_links)})",
                                 n=e["n"], board=v.name)
            if window:
                self.echo_burst(window)
            else:
                self.echo_burst(60)
        finally:
            released = time.monotonic()
            for v in victims:                     # explicit, although each hook restores itself
                if e["kind"] in ("lora_only", "lora_blind"):
                    self.write(v, "bb on")
                if e["kind"] == "lora_down" or (e["kind"] == "lora_blind" and v is victims[0]):
                    self.write(v, "lora on")
            self.phase = None
        self.event("released", n=e["n"], unconfirmed=exp["unconfirmed"])
        mine = [x for x in self.echoes if x.get("phase") == f"#{e['n']}"]
        exp["echoes"] = {"sent": len(mine),
                         "delivered": sum(x["result"] == "delivered" for x in mine),
                         "partial": sum(x["result"] == "partial" for x in mine),
                         "lost": sum(x["result"] in ("lost", "refused") for x in mine),
                         "crossings": sum(x["crossed"] for x in mine),
                         "wanted": sum(len(x["receivers"]) for x in mine)}
        times = sorted(t for x in mine for t in x["got"].values())
        exp["cross_s"] = {"median": round(statistics.median(times), 2), "max": times[-1]} if times else None
        self.settle(exp, released)
        if e["kind"] != "lora_wedge":
            if not self.wait(LORA_RECOVERY_S, until=lambda: self.lora_peers_back(victims)):
                self.finding(f"#{e['n']} LoRa links did not all come back after the window on "
                             f"{', '.join(v.name for v in victims)}", n=e["n"])
        for v in victims:
            if v.bb_off or v.lora_off:
                self.finding(f"#{e['n']} {v.name}: a chaos hook is still engaged after the round "
                             f"(bb off {v.bb_off}, lora off {v.lora_off})", n=e["n"], board=v.name)
        after = self.lora_query()
        exp["lora"] = self.lora_delta(before, after)
        first = [r["frames_first"] for r in exp["lora"].values() if r["frames_first"] is not None]
        exp["frames_first"] = sum(first) if first else None
        self.event("lora_round", n=e["n"], echoes=exp["echoes"], cross_s=exp["cross_s"],
                   frames_first=exp["frames_first"], lora=exp["lora"])
        if e["kind"] in ("lora_only", "lora_blind") and exp["echoes"]["crossings"] == 0 \
                and exp["echoes"]["sent"]:
            self.finding(f"#{e['n']} {e['kind']}: not one of {exp['echoes']['sent']} echoes crossed while "
                         "the ESP-NOW backbone was off; LoRa carried nothing", n=e["n"])
        if e["kind"] == "lora_down" and exp["echoes"]["lost"] + exp["echoes"]["partial"]:
            self.finding(f"#{e['n']} lora_down: {exp['echoes']['lost']} lost and "
                         f"{exp['echoes']['partial']} partial echoes while Wi-Fi was up and only LoRa was "
                         "off; nothing should have been lost", n=e["n"])

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
            budget = e["outage_s"] + self.args.recovery_timeout + (
                LORA_RECOVERY_S if e["kind"] in LORA_KINDS else 0)
            if time.monotonic() + budget > run_end:
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
        echo_ok = [x for x in self.echoes if x["result"] == "delivered"]
        echo_lat = sorted(x["latency_s"] for x in echo_ok if x["latency_s"] is not None)
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
               ""] + ([
                   "**No AP was on USB, so no AP was restarted and the backbone was never broken on "
                   "purpose.** Handheld faults and ordinary messages are what this run measured; AP "
                   "outages, backbone healing and LoRa rounds were not tested. Rows about them below "
                   "are empty because nothing was injected, not because nothing failed.", ""]
                   if not self.aps else []) + [
               "## Result", "",
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
               f"| AP-to-AP echoes | {len(echo_ok)} of {len(self.echoes)} reached every other live AP; "
               f"latency {spread(echo_lat)} |",
               f"| Findings | {len(self.findings)} |", ""]
        out += ["## What did not recover", ""]
        bad = [e for e in exps if e.get("result") == "not recovered"]
        if bad:
            out += [f"- **#{e['n']} {e['kind']} {', '.join(e['victims'])}: "
                    + "; ".join(e.get("unsteady", [])) + "**" for e in bad]
        else:
            out += ["Every experiment reached steady state again." if exps else "No experiment ran."]
        out += ["", "## Traffic under test", "",
                ("Handhelds messaged each other." if len(self.hhs) >= 2 else
                 "**No handheld was on USB, so no handheld was driven and none was restarted.** Every "
                 "message under test was the AP console's own diagnostic echo (`ping`), flooded AP to AP; "
                 "handhelds appear only as the presence, battery and roaming the APs report."),
                "", ("Alerts and announcements were not sent (--no-alerts)." if self.args.no_alerts
                     else "Alerts and announcements were part of the traffic."), ""]
        out += ["## Findings", ""]
        out += [f"- {f['wall']} (t={f['t']} s): {f['text']}" for f in self.findings] or ["None."]
        out += ["", "## Experiments", "", "| # | Start (s) | Fault | Victims | Result | Relink (s) | Steady (s) | Failover |",
                "|---|---|---|---|---|---|---|---|"]
        for e in exps:
            fault = ("pulse" if e["kind"] == "pulse" else f"{e['kind']} {e['outage_s']} s" if e["outage_s"]
                     else e["kind"])
            fo = "; ".join(f"{f['handheld']} to AP {f['node']} in {f['seconds']} s" for f in e["failover"]) or ""
            out.append(f"| {e['n']} | {e['started']} | {fault} | {', '.join(e['victims'])} | {e.get('result') or 'running'} "
                       f"| {e.get('relink_s') or ''} | {e.get('recovery_s', '')} | {fo} |")
        out += self.lora_report()
        out += ["", "## Memory", "",
                "Free heap now and the lowest seen during the run: bytes on APs (status line), KB on handhelds "
                "('Online' line). A lowest value that keeps falling across long uptimes points at a leak; compare "
                "raw logs hour by hour. MAIN is the one to watch: it carries a GPS and a LoRa module.", ""]
        out += [f"- {b.name}: free {b.free_heap if b.free_heap is not None else 'no sample'}, "
                f"lowest {b.lowest_heap if b.lowest_heap is not None else 'no sample'}" for b in self.boards]
        out += ["", "Raw serial logs: raw-NN.log per hour. Every event: events.jsonl.", ""]
        (self.out / "summary.md").write_text("\n".join(out), encoding="utf-8")

    def lora_report(self):
        """The LoRa section: what each round proved, and every counter the modules kept."""
        rounds = [e for e in self.experiments if e["kind"] in LORA_KINDS]
        if not rounds and not self.lora_last:
            return []
        out = ["", "## LoRa (D71)", ""]
        if not rounds:
            out += ["No LoRa round ran (add --lora). The counters below are the state at the end.", ""]
        else:
            first = [e["frames_first"] for e in rounds if e.get("frames_first") is not None]
            out += [f"{len(rounds)} rounds. **Frames that arrived over LoRa before Wi-Fi had delivered them: "
                    f"{sum(first) if first else 'not attributable'}** (the firmware's own "
                    "`frames out N in N (N of them arrived here first)`, differenced across each round; a round "
                    "in which an AP restarted is left out because its counters were zeroed).", "",
                    "| # | Round | APs | Window (s) | Echoes sent | Crossed fully | Crossings | Cross time "
                    "median/max (s) | LoRa-first frames | Module back (s) | Result |",
                    "|---|---|---|---|---|---|---|---|---|---|---|"]
            for e in rounds:
                ec = e.get("echoes", {})
                ct = e.get("cross_s") or {}
                back = e.get("peers_back_s")
                out.append(
                    f"| {e['n']} | {e['kind']} | {', '.join(e['victims'])} | {e.get('window_s', 0)} | "
                    f"{ec.get('sent', '')} | {ec.get('delivered', '')} | "
                    f"{ec.get('crossings', '')} of {ec.get('wanted', '')} | "
                    f"{ct.get('median', '')}/{ct.get('max', '')} | "
                    f"{e.get('frames_first') if e.get('frames_first') is not None else ''} | "
                    f"{back if back is not None else ''} | {e.get('result') or 'running'} |")
            out += ["", "What each round does: "
                    + "; ".join(f"**{k}** {LORA_WHAT[k]}" for k in LORA_KINDS
                                if any(e["kind"] == k for e in rounds)), ""]
            out += ["Counters per round, per AP (parts dropped / reassembly timeouts / seal failures / "
                    "queue dropped / retries / airtime ms / module restarts):", "",
                    "| # | AP | parts out/in | dropped | reasm t/o | seal fail | queue high | q dropped | "
                    "retries | airtime ms | restarts |", "|---|---|---|---|---|---|---|---|---|---|---|"]
            for e in rounds:
                for name, r in (e.get("lora") or {}).items():
                    note = " (AP restarted: counters zeroed)" if r.get("restarted") else ""
                    def v(k):
                        return "" if r.get(k) is None else r[k]
                    out.append(f"| {e['n']} | {name}{note} | {v('parts_out')}/{v('parts_in')} | "
                               f"{v('parts_dropped')} | {v('reasm_timeouts')} | {v('seal_fail')} | "
                               f"{v('queue_high')} | {v('queue_dropped')} | {v('retries')} | "
                               f"{v('airtime_ms')} | {v('restarts')} |")
        out += ["", "### Each AP's LoRa module at the end", ""]
        for name, s in self.lora_last.items():
            peers = ", ".join(f"AP {i} {'up' if p['up'] else 'DOWN'} RSSI {p['rssi']} dBm SNR {p['snr']} "
                              f"({p['age_ms']} ms ago)" for i, p in sorted(s.get("peers", {}).items()))
            out.append(f"- **{name}**: {s.get('state', 'no answer')}"
                       + (f", address {s['address']} network {s['network']}, SF{s.get('sf')} "
                          f"BW{s.get('bw')} {s.get('power_dbm')} dBm" if s.get("address") is not None else "")
                       + (f"; frames out {s.get('frames_out')} in {s.get('frames_in')} "
                          f"({s.get('frames_first')} arrived here first), parts out {s.get('parts_out')} "
                          f"in {s.get('parts_in')} dropped {s.get('parts_dropped')}, reassembly timeouts "
                          f"{s.get('reasm_timeouts')}, seal failures {s.get('seal_fail')}, queue high "
                          f"{s.get('queue_high')} dropped {s.get('queue_dropped')}, retries {s.get('retries')}, "
                          f"airtime {s.get('airtime_ms')} ms, module restarts {s.get('restarts')}"
                          if s.get("frames_out") is not None else "")
                       + (f"; peers: {peers}" if peers else "; no peer heard"))
        return out


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
    ("ap_refused_direct", "W (5000) MSG: [MSG] Refused by the AP (offline): 1:1 to 2 boot 44 seq 7",
     ("offline", "2", "44", "7")),
    ("received_direct", "I (5000) MSG: [MSG] From 1 (Handheld 1): 1:1 boot 44 seq 7, 12 bytes", ("1", "44", "7")),
    ("received_direct", "I (5000) MSG: [MSG] From 2 (Handheld 2) URGENT: 1:1 boot 9 seq 1, 3 bytes", ("2", "9", "1")),
    ("volume", "Volume: medium", ("medium",)),
    ("echo_sent", "Echo sent to 2 link(s): CX0130-00021", ("2", "CX0130-00021")),
    ("echo_not_sent", "Echo not sent (0 links): CX0130-00022", ("0", "CX0130-00022")),
    ("echo_rx", "I (5000) BB: [BB] Echo from node 0 after 1 hop(s): CX0130-00021", ("0", "1", "CX0130-00021")),
    ("bb_off", "W (5000) BB: [BB] ESP-NOW off for 240 s (chaos hook); anything that still crosses went by LoRa",
     ("240",)),
    ("bb_on", "W (5000) BB: [BB] ESP-NOW back on", ()),
    ("lora_off", "W (5000) lora: [LORA] Off for 240 s (chaos hook); it comes back on its own", ("240",)),
    ("lora_on", "W (5000) lora: [LORA] Back on", ()),
    ("lora_resetting", "W (5000) lora: [LORA] Resetting the module", ()),
    ("lora_configured", "I (1200) lora: [LORA] Configured RYLR998_REYAX_V1.2.6: address 0, network 7, "
     "parameters 9,7,1,12, 868500000 Hz, 22 dBm", ("RYLR998_REYAX_V1.2.6", "0", "7")),
    ("lora_link_up", "I (3000) lora: [LORA] Link up to AP 2, RSSI -15 dBm, SNR 9", ("2", "-15", "9")),
    ("lora_link_down", "W (9000) lora: [LORA] Link down to AP 2 (no heartbeat for 95 s)",
     ("2", "no heartbeat for 95 s")),
    ("lora_gone", "W (9000) lora: [LORA] The module stopped answering; this AP carries on with one backbone", ()),
]

# A whole `lora` status block as MAIN prints it, prompt and all. RSSI and SNR are the figures measured on
# the bench on 2026-09-20 (MAIN<->SOUTH -27 dBm, NORTH<->SOUTH -15 dBm, SNR 8-10).
LORA_BLOCK = """grid> lora
LoRa (D71): fitted and configured, UART1 rx GPIO32 tx GPIO33 reset wired
  address 0, network 7, 868500000 Hz, SF9 BW125 CR4/5, 22 dBm, broadcast on, RYLR998_REYAX_V1.2.6
  last frame 4120 ms ago, RSSI -27 dBm, SNR 9
  AP    STATE  RSSI  SNR  AGE_MS
  1     UP      -15    8    4120
  2     DOWN    -27   10   91000
  frames out 184 in 173 (12 of them arrived here first)
  parts out 552 in 519 dropped 3 | reassembly timeouts 1 | too large 0
  seal/open failures 0 | queue 0 (high 4, dropped 2) | retries 7 | airtime 512340 ms | module restarts 1
  one full part is about 930 ms on the air; it can take a 512-byte payload (no room for a long payload to \
be sent from here; it can still receive one); live voice never comes this way"""

LORA_BLOCK_WANT = {"fitted": True, "off": False, "address": 0, "network": 7, "sf": 9, "bw": 125,
                   "power_dbm": 22, "broadcast": True, "rssi": -27, "snr": 9, "heard_age_ms": 4120,
                   "frames_out": 184, "frames_in": 173, "frames_first": 12, "parts_out": 552,
                   "parts_in": 519, "parts_dropped": 3, "reasm_timeouts": 1, "refused_big": 0,
                   "seal_fail": 0, "queue_depth": 0, "queue_high": 4, "queue_dropped": 2, "retries": 7,
                   "airtime_ms": 512340, "restarts": 1, "done": True}
LORA_PEERS_WANT = {1: {"up": True, "rssi": -15, "snr": 8, "age_ms": 4120},
                   2: {"up": False, "rssi": -27, "snr": 10, "age_ms": 91000}}
LORA_HEADERS = [
    ("LoRa (D71): fitted and configured [OFF: chaos hook], UART1 rx GPIO4 tx GPIO5 reset wired",
     {"fitted": True, "off": True}),
    ("LoRa (D71): no module fitted, UART1 rx GPIO4 tx GPIO5 reset not used", {"fitted": False, "off": False}),
    ("LoRa (D71): module stopped answering, UART1 rx GPIO4 tx GPIO5 reset wired", {"fitted": False}),
    ("LoRa: not started", {"fitted": False, "done": True}),
    ("LoRa: no module on this AP", {"fitted": False, "done": True}),
]


def check_lora_parser():
    """The `lora` status parser, on lines as the console really prints them. No board is opened."""
    failures = 0
    st, peers = {}, {}
    for line in LORA_BLOCK.splitlines():
        parse_lora_line(line, st, peers)
    for k, want in LORA_BLOCK_WANT.items():
        if st.get(k) != want:
            failures += 1
            print(f"FAIL lora status {k}: {st.get(k)!r}, want {want!r}")
    if peers != LORA_PEERS_WANT:
        failures += 1
        print(f"FAIL lora peers: {peers}, want {LORA_PEERS_WANT}")
    for line, want in LORA_HEADERS:
        st = {}
        if not parse_lora_line(line, st):
            failures += 1
            print(f"FAIL lora header not parsed: {line!r}")
            continue
        for k, v in want.items():
            if st.get(k) != v:
                failures += 1
                print(f"FAIL lora header {k} in {line!r}: {st.get(k)!r}, want {v!r}")
    # Lines that are not part of a status block must fall through to the ordinary parsers.
    for line in ("I (30000) node: [GRID] links 2 handhelds 1 heap 81234 min 74400 time GPS",
                 "I (5000) MSG: [MSG] From 2 (Handheld 2): CX0130-00014",
                 "  AP    STATE  RSSI  SNR  AGE_MS"):
        if parse_lora_line(line, {}, {}):
            failures += 1
            print(f"FAIL lora parser swallowed {line!r}")
    return failures


def check_lora_schedule():
    """LoRa rounds: only when asked for, only on APs, and the right number of them."""
    failures = 0
    for n_aps, n_hhs in ((3, 0), (3, 2), (2, 0), (1, 2)):
        for share, want_any in ((0.0, False), (0.4, True), (1.0, True)):
            args = argparse.Namespace(hours=12, min_gap=3, max_gap=15, blackout=False, max_aps_down=2,
                                      scenario="mixed", lora=share > 0, lora_share=share)
            seen = set()
            for seed in range(60):
                for e in make_schedule(random.Random(seed), n_aps, n_hhs, args):
                    if e["kind"] not in LORA_KINDS:
                        if share == 1.0:
                            failures += 1
                            print(f"FAIL --scenario lora seed {seed}: a {e['kind']} slipped in")
                        continue
                    seen.add(e["kind"])
                    if share == 0.0:
                        failures += 1
                        print(f"FAIL {n_aps} APs seed {seed}: a LoRa round without --lora")
                    if any(not v.startswith("AP") for v in e["victims"]):
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: a handheld as a victim {e['victims']}")
                    want = 2 if e["kind"] in ("lora_only", "lora_blind") else 1
                    if len(set(e["victims"])) != want:
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: {e['victims']}, want {want} distinct APs")
                    if n_aps < 2 and e["kind"] in ("lora_only", "lora_blind"):
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: needs two APs, the grid has {n_aps}")
                    if e["kind"] == "lora_wedge" and e["outage_s"] != 0:
                        failures += 1
                        print(f"FAIL lora_wedge seed {seed}: a window of {e['outage_s']} s")
                    if e["kind"] != "lora_wedge" and not 90 <= e["outage_s"] <= 300:
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: window {e['outage_s']} s outside 90-300")
            if want_any and n_aps >= 2 and not seen:
                failures += 1
                print(f"FAIL {n_aps} APs share {share}: no LoRa round was ever scheduled")
    return failures


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
    for n_aps, n_hhs in ((3, 2), (2, 2), (4, 3), (1, 2), (3, 1), (3, 0)):
        args = argparse.Namespace(hours=12, min_gap=3, max_gap=15, blackout=False, max_aps_down=2,
                                  scenario="mixed", lora=False, lora_share=0.0)
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
    failures += check_lora_parser()
    failures += check_lora_schedule()
    print(f"self-check: {len(SAMPLES) + len(identities) + len(rows) + 4} parser and bridge samples, "
          f"{len(LORA_BLOCK_WANT) + len(LORA_PEERS_WANT) + len(LORA_HEADERS)} LoRa status checks, "
          f"{schedules} schedules plus the LoRa round rules, {failures} failure(s)")
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
    ap.add_argument("--scenario", choices=["mixed", "blackout", "lora"], default="mixed",
                    help="mixed: random faults. blackout: every experiment takes all APs out together "
                         "(implies --blackout). lora: every experiment is a LoRa round (implies --lora)")
    ap.add_argument("--lora", action="store_true",
                    help="add LoRa rounds (D71): the AP console hooks bb off / lora off / lora reset, with "
                         "AP-to-AP echoes as the traffic. No board is taken out by them")
    ap.add_argument("--lora-share", type=float, default=0.35,
                    help="fraction of experiments that are LoRa rounds when --lora is given")
    ap.add_argument("--time-recovery", type=float, default=120,
                    help="seconds after a blackout to wait for grid time to come back from a handheld (D53) "
                         "before the PC sets it")
    ap.add_argument("--recovery-timeout", type=float, default=180, help="seconds after release to reach steady state")
    ap.add_argument("--probe-interval", type=float, default=45, help="seconds between handheld messages")
    ap.add_argument("--no-set-time", dest="set_time", action="store_false",
                    help="do not set grid time from the PC clock at the start (1:1 and group need it, D6)")
    ap.add_argument("--no-alerts", action="store_true",
                    help="never send announcements or urgent broadcasts (they wake every handheld); for night runs")
    ap.add_argument("--quiet-handhelds", action="store_true",
                    help="set handheld volume off for the run and restore it at the end")
    args = ap.parse_args()
    if args.scenario == "blackout":
        args.blackout = True
    if args.scenario == "lora":
        args.lora, args.lora_share = True, 1.0
    args.lora_share = max(0.0, min(1.0, args.lora_share))

    if args.self_check:
        return self_check()
    if args.seed is None:
        args.seed = secrets.randbelow(1 << 31)   # the OS's randomness: every night a new schedule; printed for replay
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
