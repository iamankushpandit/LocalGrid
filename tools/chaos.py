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
import asyncio
import datetime
import hashlib
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
# A handheld with no Wi-Fi session is sampled this often, and only a fallback that outlasts the grace
# period is a finding: joining an AP legitimately takes a few seconds, and a roam takes longer.
# D87: the hours in which this run may raise an alert that makes a noise. Outside them it raises none,
# because an announcement or an urgent broadcast sounds on every handheld in the house - and an urgent
# one sounds even with the volume off (D40), which is exactly what makes it worth testing and exactly
# what makes it unwelcome at four in the morning. Owner, 2026-10-02: "between nine PM at night to two PM
# in the afternoon, there should be no audible test."
AUDIBLE_FROM_H = 14
AUDIBLE_TO_H = 21

FALLBACK_EVERY_S = 10
FALLBACK_GRACE_S = 45
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
    # A handheld's own self-restoring hook (D78), the mirror of the APs' `bb off` and `lora off`.
    "wifi_off": re.compile(r"\[NET\] Wi-Fi off for (\d+) s \(chaos hook\)"),
    # Two ways it comes back, and the round has to recognise both: the timer running out, and `wifi on`
    # typed at it. Watching only the first reported "Wi-Fi is still switched off after the round" every
    # time the round put it back itself, which is the one thing it always does.
    "wifi_on": re.compile(r"\[NET\] Wi-Fi back on: the timed outage is over|"
                          r"\[NET\] Wi-Fi switched ON"),
    "lora_resetting": re.compile(r"\[LORA\] Resetting the module"),
    "lora_configured": re.compile(r"\[LORA\] Configured ([^:]+): address (\S+?), network (\S+?),"),
    "lora_link_up": re.compile(r"\[LORA\] Link up to AP (\d+), RSSI (-?\d+) dBm, SNR (-?\d+)"),
    "lora_link_down": re.compile(r"\[LORA\] Link down to AP (\d+) \(([^)]*)\)"),
    "lora_gone": re.compile(r"\[LORA\] The module stopped answering"),
    # Voice (D61, D88). A talk is a stream with no token and no ack, so it is measured at both ends
    # instead: the sender says how many frames it sent under a talk id, every listener says how many
    # of them it played under the same id, and the difference is the loss for a real voice. "Talk not
    # sent" is the refusal a handheld on LoRa must give, because D71 keeps voice off that radio.
    "voice_caps": re.compile(r"\[MSG\] Push-to-talk: (talk and listen|listen only|off)"),
    "voice_begin": re.compile(r"\[MSG\] Talking to (everyone|group|device) (\d+)"),
    "voice_sent": re.compile(r"\[MSG\] Talk ended: talk (\d+), (\d+) frame\(s\), (\d+) dropped before "
                             r"the AP, (\d+) ms"),
    "voice_heard": re.compile(r"\[MSG\] Heard talk (\d+) from (\d+): (\d+) frame\(s\), (\d+) missing"),
    "voice_refused": re.compile(r"\[MSG\] Talk not sent: (.+)"),
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

# The same command on a handheld prints a different block (firmware/handheld/main/hh_console.c, cmd_lora):
# a handheld has one kind of peer (APs) and no "arrived here first", and its counters are laid out by line
# rather than packed. Parsed into the same keys, so lora_delta and the report need to know nothing about
# which side of the grid a board is on.
LORA_HH_STATUS = {
    # The middle group is lazy, not "anything but a comma": ", not configured yet" has a comma in it.
    "header": re.compile(r"LoRa \(D77\): (module fitted|no module answering)(.*?), address (\d+), "
                         r"network (\d+)"),
    "absent": re.compile(r"LoRa \(D77\): this board has no connector for a module"),
    "module": re.compile(r"^module:\s+(\S+)"),
    "peers": re.compile(r"^APs heard: (none yet|[\d ]+)"),
    "last": re.compile(r"^last in:\s+(?:never|(\d+) s ago, RSSI (-?\d+) dBm, SNR (-?\d+))"),
    "frames": re.compile(r"^frames:\s+(\d+) out, (\d+) in"),
    "parts": re.compile(r"^parts:\s+(\d+) out, (\d+) in, (\d+) dropped, (\d+) retries"),
    "refused": re.compile(r"^refused:\s+(\d+) by type \(not carried over LoRa\), (\d+) could not be opened"),
    "queue": re.compile(r"^queue:\s+(\d+) dropped; reassembly timeouts (\d+)"),
    "airtime": re.compile(r"^airtime:\s+(\d+) ms; module resets (\d+)"),
}


def parse_lora_hh_line(text, st, peers=None):
    """One line of a handheld's `lora` block. Same keys as the AP's, so nothing downstream cares."""
    if LORA_HH_STATUS["absent"].search(text):
        st.update(fitted=False, connector=False, state="no connector", off=False, done=True)
        return True
    if m := LORA_HH_STATUS["header"].search(text):
        extra = m.group(2) or ""
        st.update(state=(m.group(1) + extra).strip(), connector=True,
                  fitted=m.group(1) == "module fitted" and "no module" not in m.group(1),
                  off="switched OFF" in extra, configured="not configured" not in extra,
                  address=int(m.group(3)), network=int(m.group(4)), done=False)
        # A handheld with no module prints one more line and stops, so the block is already complete.
        if not st["fitted"]:
            st["done"] = True
        return True
    if m := LORA_HH_STATUS["module"].search(text):
        st["version"] = m.group(1)
        return True
    if m := LORA_HH_STATUS["peers"].search(text):
        if peers is not None:
            for ap in ([] if m.group(1).startswith("none") else m.group(1).split()):
                # A handheld reports which APs it has heard recently and nothing per peer: it keeps no
                # per-link state, only "heard recently" or not, which is what UP means here. The RSSI it
                # prints belongs to the last frame from any AP, so attributing it to a peer would be a
                # figure that reads like a measurement and is not one.
                peers[int(ap)] = {"up": True, "rssi": None, "snr": None, "age_ms": None}
        return True
    if m := LORA_HH_STATUS["last"].search(text):
        if m.group(1) is not None:
            st.update(heard_age_ms=int(m.group(1)) * 1000, rssi=int(m.group(2)), snr=int(m.group(3)))
        return True
    if m := LORA_HH_STATUS["frames"].search(text):
        st.update(frames_out=int(m.group(1)), frames_in=int(m.group(2)), frames_first=None)
        return True
    if m := LORA_HH_STATUS["parts"].search(text):
        st.update(parts_out=int(m.group(1)), parts_in=int(m.group(2)), parts_dropped=int(m.group(3)),
                  retries=int(m.group(4)))
        return True
    if m := LORA_HH_STATUS["refused"].search(text):
        st.update(refused_type=int(m.group(1)), seal_fail=int(m.group(2)))
        return True
    if m := LORA_HH_STATUS["queue"].search(text):
        st.update(queue_dropped=int(m.group(1)), reasm_timeouts=int(m.group(2)))
        return True
    if m := LORA_HH_STATUS["airtime"].search(text):
        st.update(airtime_ms=int(m.group(1)), restarts=int(m.group(2)), done=True)   # last line of the block
        return True
    return False
# Counters the firmware adds up since that AP booted, so a restart zeroes them: only these are differenced.
LORA_COUNTERS = ("frames_out", "frames_in", "frames_first", "parts_out", "parts_in", "parts_dropped",
                 "reasm_timeouts", "refused_big", "seal_fail", "queue_dropped", "retries", "airtime_ms",
                 "restarts")


# ---- the access points, seen over BLE (D68) --------------------------------------------------------
#
# Why this exists. Once the APs run on battery (D81) the harness holds no port to them, and it was blind:
# it could fault handhelds and it could not see an AP restart, lose a backbone link, or brown out. That
# made a whole-system night impossible in the only way that mattered - not because the faults could not
# be injected (each AP attacks itself, D82, armed over BLE, D85) but because this side could not tell a
# message lost to an AP outage from a message lost to a bug, and would have called the first a finding.
#
# A wrong finding is worse than a missing one; tonight proved that twice. So the harness listens to the
# same sealed status beacon the laptop dashboard reads, and treats an AP whose beacon says it is in
# trouble exactly as it treats a board it reset itself: an active fault, which excuses nothing but is
# recorded against every probe in flight.
#
# It takes no port, joins no Wi-Fi, carries no message, and never holds a BLE address (D21, D25, D70).


class BleHands:
    """Asks an access point to perform one fault, over BLE, behind the admin login (D90).

    The counterpart of BleEyes. The eyes only listen; these connect, log in, send OP_FAULT and
    disconnect, one short session per fault, using chaos_ble's own session code so there is exactly one
    implementation of the link. Each fault restores itself on the access point's clock, so a harness
    that dies mid-run leaves nothing broken behind it - which is why there is no disarm here at all.

    A BLE adapter does one thing at a time well. The eyes' scanner is stopped around each session and
    started again after, which costs a few seconds of beacons and nothing else: BleEyes keeps its
    baseline and boot history across a restart of its listener.
    """

    def __init__(self, secrets_path, password, names):
        self._secrets = secrets_path
        self._password = password
        self._names = names
        self.aps = {}          # AP index -> BLE device, from one scan at the start
        self.error = None
        self._cb = self._gw = self._key = self._auth = None

    def start(self):
        try:
            sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
            import grid_watch as gw
            import chaos_ble as cb
            backbone_key, _disc = gw.parse_secrets(self._secrets)
            self._key = gw.link_key(gw.derive_key(backbone_key))
            self._auth = gw.AdminAuth()
            self._auth.set_password(self._password)
            self._gw, self._cb = gw, cb
            found = asyncio.run(cb.find_all_aps(gw.parse_company_id(), self._key))
            self.aps = {ap: device for ap, device in found}
        except Exception as e:
            self.error = f"{type(e).__name__}: {e}"
            return False
        return bool(self.aps)

    def fault(self, ap, kind, seconds):
        """kind is a grid_watch.FAULT_KINDS name. Returns what the AP acknowledged, or raises."""
        gw, cb = self._gw, self._cb

        async def do(s):
            return gw.decode_fault_ok(await s.ask(gw.OP_FAULT, gw.OP_FAULT_OK, gw.fault_body(kind, seconds)))

        return asyncio.run(cb.talk(self.aps[ap], self._key, self._auth, do, ap))


class BleEyes:
    """What the beacons say about each AP, for a run that holds no cable to them.

    Runs grid_watch's own listener on a thread - the same decoder, the same sealed-frame checks - and
    exposes the few facts this harness reasons about: whether an AP is being heard, its boot counter, how
    many backbone links it has, how many handhelds are attached, and its brownout and restart counts.
    """

    class _Stop:
        """What grid_watch.listen expects of a stop signal, driven from another thread.

        It calls `stop.is_set()` and **awaits** `stop.wait()`. A threading.Event satisfies the first and
        not the second - its wait() returns a bool, so awaiting it raises TypeError and the listener dies
        on its first pass. That is exactly what happened the first time this ran, and the only reason it
        was caught is that the harness said it had heard no access point instead of assuming it had.
        """

        def __init__(self, flag):
            self._flag = flag

        def is_set(self):
            return self._flag.is_set()

        async def wait(self):
            while not self._flag.is_set():
                await asyncio.sleep(0.2)
            return True

    def __init__(self, secrets_path, names):
        self.state = None
        self.stop = None
        self.thread = None
        self.error = None
        self._secrets = secrets_path
        self._names = names
        self.started_at = None
        self.baseline = {}        # AP index -> the first full reading, to difference against
        self.boot_at = {}         # AP index -> (boot, when this boot counter was first seen)
        self._last = {}           # AP index -> the last reading while it was still being heard

    def start(self):
        try:
            sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
            import grid_watch as gw
        except Exception as e:
            self.error = f"grid_watch could not be imported ({e})"
            return False
        try:
            backbone_key, disc = gw.parse_secrets(self._secrets)
            company = gw.parse_company_id()
        except Exception as e:
            self.error = f"the grid's secrets could not be read ({e})"
            return False
        self._gw = gw
        self.state = gw.GridState(gw.derive_key(backbone_key), disc, ap_names=self._names, echo=False)
        self.stop = threading.Event()

        def run():
            try:
                asyncio.run(gw.listen(self.state, company, BleEyes._Stop(self.stop)))
            except Exception as e:          # a laptop with its Bluetooth off, most likely
                self.error = f"{type(e).__name__}: {e}"

        self.thread = threading.Thread(target=run, name="ble-eyes", daemon=True)
        self.thread.start()
        self.started_at = time.monotonic()
        return True

    def close(self):
        if self.stop is not None:
            self.stop.set()
        if self.thread is not None:
            self.thread.join(timeout=5)

    def aps(self):
        """AP index -> what the beacon says, or {} when nothing has been heard yet."""
        if self.state is None:
            return {}
        try:
            snap = self.state.snapshot()
        except Exception:
            return {}
        out = {}
        for a in snap.get("aps", []):
            h = a.get("health") or {}
            out[a["index"]] = {
                "name": a.get("name"), "heard": bool(a.get("heard")),
                "silent_s": a.get("silent_s"), "boot": a.get("boot"),
                # None, not 0, when no sealed health frame has been read yet: zero links and "nothing
                # said yet" are different claims, and conflating them marked every AP as having lost its
                # backbone in the first seconds of a run.
                "links": len(h["links"]) if h.get("links") is not None else None,
                "has_health": bool(h), "handhelds": h.get("handhelds_here"),
                "brownouts": h.get("brownouts"), "restarts": h.get("restarts"),
                "heap_kb": h.get("heap_kb"), "reset": h.get("reset"),
                "uptime": h.get("uptime"),
                # What it advertises room for, from the discovery payload. A grid whose access points
                # all say "no room" has handhelds with nowhere to go, and that is not a fault.
                "free_slots": (a.get("disc") or {}).get("free_slots")}
        for i, row in out.items():
            if i not in self.baseline and row["boot"] is not None and row["brownouts"] is not None:
                self.baseline[i] = dict(row)
            # When this boot counter first appeared, which is what tells a restart that just happened
            # from one that happened an hour ago. The baseline must not be moved to do this job: the
            # report differences restarts and brownouts against it for the whole run.
            if row["boot"] is not None and self.boot_at.get(i, (None,))[0] != row["boot"]:
                self.boot_at[i] = (row["boot"], time.time())
            if row["boot"] is not None and (row["silent_s"] or 0) <= BLE_SILENT_S:
                # Kept so an access point that has gone flat can still be reported: its last uptime is
                # how long its battery held, which is the whole point of a run that waits for it to die.
                self._last[i] = dict(row, at=time.time())
        return out

    def troubled(self):
        """APs the beacon says are in trouble right now, as reasons keyed by AP index.

        Deliberately generous about what counts: a link short of the others, a boot counter that has
        moved, or a beacon gone quiet. An AP that is mid-fault must never look healthy here, because the
        whole point is to stop blaming the grid for what a fault was doing.
        """
        rows = self.aps()
        if not rows:
            return {}
        # Only access points that are *currently* being heard set the bar. A dead one keeps its last
        # reading, and counting that was a real cost: when Everest ran out of battery its stale "2 links"
        # became the standard the survivors were judged against, so Fuji and Denali - correctly down to
        # one link each, because the third was gone - looked permanently broken. The grid never counted as
        # steady again and the run skipped twelve experiments in a row. A grid with an access point out of
        # battery is not an unsteady grid; it is a smaller grid.
        live = [r for r in rows.values()
                if r["heard"] and (r["silent_s"] or 0) <= BLE_SILENT_S and r["links"] is not None]
        most = max((r["links"] for r in live), default=0)
        out = {}
        for i, r in rows.items():
            why = []
            if not r["heard"] or (r["silent_s"] or 0) > BLE_SILENT_S:
                if (r["silent_s"] or 0) > DEAD_AP_S:
                    # Long gone. Said once in the report, not held against the grid for ever.
                    continue
                why.append(f"not heard for {r['silent_s']} s")
            elif not r["has_health"]:
                # Heard, but its sealed health frame has not arrived or has gone stale. That is its own
                # kind of trouble and is reported as itself rather than as a link count of zero.
                why.append("heard, but no fresh status frame from it")
            if r["links"] is not None and r["links"] < most:
                why.append(f"{r['links']} backbone link(s) where another AP has {most}")
            # A restart makes an access point troubled *while it is recent*. It used to be compared
            # against the baseline, which is the first reading of the run and never moves - so the first
            # time an access point restarted it became permanently troubled, the grid was never steady
            # again, and every remaining experiment was skipped with "grid still not steady". Seen on
            # 2026-10-01: all three access points restarted 18 minutes into a run (their own armed chaos,
            # D85), and experiments #1 and #2 were refused with the identical boot numbers five minutes
            # apart - the same restart reported twice as though it were still happening. A night of
            # chaos restarts access points on purpose; if each one poisoned the grid for ever, arming
            # them would guarantee that nothing was ever tested.
            boot, since = self.boot_at.get(i, (None, None))
            base = self.baseline.get(i)
            fresh = since is not None and time.time() - since < AP_RESTART_GRACE_S
            first = base["boot"] if base else None
            if fresh and boot is not None and first is not None and boot != first:
                why.append(f"restarted {int(time.time() - since)} s ago "
                           f"(boot {boot}, {r['reset']})")
            if why:
                out[i] = "; ".join(why)
        return out

    def has_room(self):
        """True if any access point still being heard advertises a free slot."""
        for r in self.aps().values():
            if not r["heard"] or (r["silent_s"] or 0) > BLE_SILENT_S:
                continue
            if r["free_slots"] is None or r["free_slots"] > 0:
                return True       # unknown counts as room: never excuse a real failure on a guess
        return False

    def silence(self):
        """Seconds since each AP was last heard, by AP index. Only APs heard at least once appear."""
        out = {}
        for i, r in self.aps().items():
            if r["boot"] is None:
                continue
            out[i] = r["silent_s"] if r["silent_s"] is not None else 0
        return out

    def last_seen(self):
        """The last full reading of each AP, kept so a dead one can still be accounted for."""
        return dict(self._last)

    def changes(self):
        """What has moved since the first full reading: restarts and brownouts, per AP."""
        rows, out = self.aps(), {}
        for i, r in rows.items():
            base = self.baseline.get(i)
            if not base:
                continue
            d_restarts = (r["restarts"] or 0) - (base["restarts"] or 0)
            d_brownouts = (r["brownouts"] or 0) - (base["brownouts"] or 0)
            if d_restarts or d_brownouts:
                out[i] = {"name": r["name"], "restarts": d_restarts, "brownouts": d_brownouts,
                          "boot": r["boot"], "reset": r["reset"], "heap_kb": r["heap_kb"]}
        return out


def parse_lora_line(line, st, peers=None):
    """One line of a `lora` status block into st (and a peer row into peers). True if it was one.

    Pure, so --self-check exercises it on sample lines with no board attached."""
    text = re.sub(r"^\s*(?:grid>\s*)?", "", line).strip()
    if parse_lora_hh_line(text, st, peers):
        return True
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
# Every image prints the first characters of its own ELF's SHA-256 as it starts. That is the one fact
# that says which build a board is running, and it is what the firmware gate compares.
ELF_SHA = re.compile(r"ELF file SHA256:\s+([0-9a-f]{6,})")
# The trailing MEMBERS column ("1 2 3") arrived with the group editor (D52). The pattern used to end at
# "member", so from that day it matched no row at all: every handheld looked as if it had no groups,
# the harness said "No group has every handheld as a member", and group messages and group voice were
# silently left out of every run - while its own self-check passed, on two rows written without the
# column. Found on 2026-10-04 by reading a real `groups` reply against it.
GROUP_ROW = re.compile(r"^\s*(\d+)\s+(\S+)\s+(member|not a member)(?:\s+[\d ]*)?\s*$")
MAC_RE = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")   # D21: never write a hardware address


# ---- schedule: pure, seeded, and the same with or without --live ----

def outage_seconds(rng):
    r = rng.random()
    if r < 0.5:
        return round(rng.uniform(5, 30))
    if r < 0.8:
        return round(rng.uniform(30, 120))
    return round(rng.uniform(120, 600))


LORA_KINDS = ("lora_only", "lora_down", "lora_wedge", "lora_blind", "lora_handheld")
# Rounds that take no board out and break no radio link, but change what the grid is allowed to lean on.
#
# no_net is the answer to "the backup hid the fault". Every other round leaves LoRa available, so a
# handheld with a module can carry its traffic through a Wi-Fi fault and the run reports success. This one
# switches the module off on every handheld that has one, which makes those handhelds behave like the ones
# that will ship without a module - and then any loss is a Wi-Fi failure with nowhere to hide.
#
# alerts walks the sequence a person actually uses in an emergency: an announcement, an SOS, and the all
# clear that stands it down. The last of those had never been sent on this bench, because the console had
# no way to send it until today.
CHAOS_GROUP = "chaos"            # the group the harness makes when no group holds every handheld under test
EXTRA_KINDS = ("no_net", "alerts", "voice")
# D90: faults an access point performs on request over BLE, when it has no cable for the PC to type at.
# ble_bb_off stands where "hold" would (the AP leaves the backbone for the window, keeping its
# handhelds); ble_restart stands where "pulse" would. Neither can cut power, which D82 already recorded.
BLE_KINDS = ("ble_bb_off", "ble_restart")
LORA_WHAT = {
    "lora_only": "ESP-NOW off on every named AP: anything that still crosses went by LoRa",
    "lora_down": "LoRa off on one AP while Wi-Fi stays up: nothing may be lost and nothing may stall",
    "lora_wedge": "the module reset under it: does it come back configured, with its peers, and how fast",
    "lora_blind": "ESP-NOW off on two APs and LoRa off on one of them: one AP genuinely isolated",
    "lora_handheld": "Wi-Fi off on one handheld that has a module: its own messages must still cross and "
                     "be acknowledged, not wait in its outbox for Wi-Fi to come back",
}

# The AP-side rounds use AP console hooks, so they need an AP on USB. This one does not: it is typed at a
# handheld, which is why it is the only LoRa round that can run while the APs are on battery - and it is
# the one that covers the leg the AP rounds never touch. The bench found on 2026-09-30 that a LoRa-only
# handheld could receive and not send, and no AP-side round would ever have shown it.
LORA_HH_KINDS = ("lora_handheld",)


# The handheld LoRa round. A frame is seconds of airtime at SF9, so probes go in at a walking pace and the
# round waits for the last of them rather than calling a message lost that was still on the air.
BLE_SILENT_S = 20              # a D68 beacon goes out every 500 ms; this much silence is a fault
# Past this, an access point is not "in trouble" any more - it is gone, and the grid it left behind is a
# smaller grid that can still be tested. Held well short of --ap-dead-after so the two never disagree.
DEAD_AP_S = 180
# How long after an access point restarts it still counts as "in trouble". Long enough for it to rejoin
# the backbone and announce its health, short enough that the grid becomes steady again afterwards - and
# it MUST become steady again, see BleEyes.troubled.
AP_RESTART_GRACE_S = 120
LORA_HH_PROBE_GAP_S = 12
LORA_HH_SETTLE_S = 45
HH_WIFI_OFF_MAX_S = 3600       # firmware/handheld/main/service/hh_service.h caps it at the same number
HH_LORA_OFF_MAX_S = 3600       # firmware/handheld/main/service/hh_lora.h caps it at the same number
HH_REJOIN_S = 60               # how long a handheld may take to find an AP again once Wi-Fi is back


def lora_window_seconds(rng):
    """A LoRa round's window. One 288-byte frame is about 3 s of airtime and the heartbeat is every 30 s,
    so a window that proves anything is minutes, not the seconds a reset outage takes."""
    return round(rng.uniform(90, 300))


def lora_round(rng, aps, lora_hhs=(), n_hhs=0):
    """Which LoRa hook, and on what.

    AP victims for the AP-side hooks, which are AP console commands. `lora_handheld` instead takes one
    handheld that has a module, and needs a second handheld to message it - with only one handheld there
    is nobody for its traffic to cross to, and nothing would be proved.
    """
    kinds = []
    if aps:
        kinds += ["lora_down", "lora_wedge"]
        if len(aps) >= 2:
            kinds += ["lora_only", "lora_only", "lora_blind"]   # the one that proves LoRa carries, twice as often
    if lora_hhs and n_hhs >= 2:
        kinds += ["lora_handheld", "lora_handheld"]   # the handheld leg, as often as the AP one that pairs with it
    if not kinds:
        return None
    kind = rng.choice(kinds)
    if kind == "lora_handheld":
        victims = [rng.choice(list(lora_hhs))]
    else:
        victims = rng.sample(aps, 2) if kind in ("lora_only", "lora_blind") else [rng.choice(aps)]
    return {"victims": victims, "kind": kind,
            "outage_s": 0 if kind == "lora_wedge" else lora_window_seconds(rng)}


def parse_hours(text):
    """"HH-HH" into a (from, to) pair of hours; the D87 window by default, "0-0" for all day."""
    if not text:
        return (AUDIBLE_FROM_H, AUDIBLE_TO_H)
    try:
        lo, hi = (int(x) for x in text.split("-", 1))
    except ValueError:
        raise SystemExit(f"--audible-hours wants HH-HH, for example 14-21, not {text!r}")
    if not (0 <= lo <= 23 and 0 <= hi <= 23):
        raise SystemExit("--audible-hours wants hours between 0 and 23")
    return (lo, hi)


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
        if getattr(args, "lora", False) and rng.random() < getattr(args, "lora_share", 0.0):
            # A LoRa round takes no board out: every board stays powered and every hook restores itself,
            # so the AP count rules above do not apply to it.
            e = lora_round(rng, aps, lora_hhs=[h for h in hhs if h in getattr(args, "_lora_hhs", hhs)],
                           n_hhs=len(hhs))
            if e is None:
                continue
            plan.append({"n": len(plan) + 1, "at_s": round(t), **e})
            t += e["outage_s"] + 120     # LoRa peers come back on a 30 s heartbeat; leave room for it
            continue
        # no_net and alerts (EXTRA_KINDS). Placed after the LoRa branch on purpose: with
        # --scenario lora every round must be a LoRa one, and that branch always continues there.
        # Neither takes a board out, so the AP-count rules above do not apply to them either.
        if hhs and rng.random() < getattr(args, "extra_share", 0.0):
            choices = []
            if len(hhs) >= 2 and getattr(args, "_lora_hhs", hhs):
                choices.append("no_net")   # at parity with alerts: this is the one that proves Wi-Fi alone
            if len(hhs) >= 2 and not getattr(args, "no_alerts", False):
                choices += ["alerts"]
            # Voice needs one handheld to talk and one to hear, which is the same two as the rest.
            # It makes the listeners audible, so it obeys the quiet hours (D87) like an alert round.
            if len(hhs) >= 2 and not getattr(args, "no_voice", False):
                choices += ["voice"]
            if choices:
                kind = rng.choice(choices)
                outage = lora_window_seconds(rng) if kind == "no_net" else 0
                plan.append({"n": len(plan) + 1, "at_s": round(t), "victims": [], "kind": kind,
                             "outage_s": outage})
                t += outage + 120
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
        if getattr(args, "_ble_aps", False) and all(v in aps for v in victims):
            # The access points are on battery and reached over BLE (D90): the same victims, the same
            # schedule, the same seed - the fault is simply asked for over the air instead of typed.
            kind = "ble_bb_off" if kind == "hold" else "ble_restart"
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
    extras = [e for e in plan if e["kind"] in EXTRA_KINDS]
    if extras:
        counts = ", ".join(f"{k} {sum(e['kind'] == k for e in extras)}" for k in EXTRA_KINDS
                           if any(e["kind"] == k for e in extras))
        extra += f", {len(extras)} backup-and-alert rounds ({counts})"
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
        self.elf_sha = None              # what the board printed as it started: which build it is running
        self.groups = {}                 # handheld: group name -> member, from its 'groups' command
        self.group_ids = {}              # handheld: group name -> the grid's id for it
        self.can_hear = None             # from the board's own boot line, not a table: None until said
        self.can_talk = None
        self.voice_sent = {}             # talk id -> (frames, dropped, ms), as the sender reported
        self.voice_heard = {}            # talk id -> (author, played, missing), as this one played it
        self.voice_refused = None        # the last 'Talk not sent' reason; cleared when a round starts
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
        self.last_node = None            # handheld: the AP it was last registered with, kept after a drop
        self.settings_version = None
        self.lowest_heap = None          # bytes on APs, KB on handhelds (as the firmware prints them)
        self.free_heap = None            # the last free-heap sample, same units
        self.lora = {}                   # AP: the last `lora` status block, parsed
        self.lora_peers = {}             # AP index -> {up, rssi, snr, age_ms} from that block
        self.lora_links = set()          # AP indices LoRa reports up, from the [LORA] Link up/down lines
        self.lora_off = None             # True/False once a hook has logged; None until then
        self.bb_off = None
        self.wifi_off = None             # handheld: its Wi-Fi switched off on purpose (D78)
        self.grid_time = None            # the newest grid time this board printed, 0/None if never
        self.lora_reset_at = None        # monotonic time of the last "Resetting the module"
        self.lora_configured_at = None   # monotonic time of the last "[LORA] Configured ..."
        self.volume = None
        self.failover = None             # (start time, experiment) while an AP under it is out
        self.drops = 0                   # handheld: times it lost its AP or restarted
        self.last_line_at = time.monotonic()
        self.boot_at = 0.0                 # monotonic time of the last boot banner
        self.silence_step = 0            # 0 quiet is fine, 1 lines released again, 2 port reopened
        # Closing a port while its reader thread sits in read() takes the whole process down with a
        # segmentation fault (exit 139). close_all() stops every reader first, but a port released
        # DURING discovery - one not in --devices, or one that gave no ID - had no way to stop just
        # its own reader, and releasing an unusable port crashed the run before a fault was injected.
        self.stop_reader = threading.Event()
        self.reader_thread = None

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
        self.rounds_reached = 0           # rounds whose turn came, run or not
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
        self.eyes = None            # BleEyes once started: what the beacons say about APs on battery
        self.ap_death_at = None     # when every AP was first declared out of battery
        self.fallback = {}           # handheld name -> the fallback episode open right now
        self.fallback_log = []       # every finished episode, for the report and events.jsonl
        self.next_fallback = 0.0
        self.no_net_until = 0.0      # while a "no safety net" round has LoRa switched off on purpose
        self.audible_hours = parse_hours(getattr(args, "audible_hours", None))
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
        b.stop_reader.clear()
        t = threading.Thread(target=self.reader, args=(b, b.ser), daemon=True)
        t.start()
        b.reader_thread = t
        self.readers.append(t)

    def reader(self, b, ser):
        buf = b""
        while not self.stopping.is_set() and not b.stop_reader.is_set():
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
        # A port that was unplugged while the harness was waiting on it has no handle. On 2026-10-04 two
        # cables were moved mid-run and this raised AttributeError, ending the run 20 minutes early.
        if b.ser is None:
            return
        try:
            b.ser.dtr = False
            b.ser.rts = True
            b.ser.dtr = False
            time.sleep(0.2)
            b.ser.rts = False
            b.ser.dtr = False
        except (self.serial.SerialException, OSError):
            pass

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
        self.close_eyes()      # the BLE listener is a thread too, and it must not outlive the run
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
        if m := ELF_SHA.search(line):
            b.elf_sha = m.group(1)
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
            b.group_ids[m.group(2)] = int(m.group(1))
        if CRASH.search(line):
            self.finding(f"{b.name}: crash output: {line.strip()}", board=b.name)
        # Any board, not only an AP. Handhelds have had their own `lora` block since D77, and gating this
        # on is_ap is why the first live run of the handheld LoRa round reported "handhelds with a LoRa
        # module: none" while two of them had one fitted and working. The two blocks are told apart by
        # their own headers (D71 on an AP, D77 on a handheld), so neither can be read as the other.
        if parse_lora_line(line, b.lora, b.lora_peers):
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
            b.last_node = b.registered_node
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
        elif m := RX["wifi_off"].search(line):
            b.wifi_off = True
            # And it therefore has no Wi-Fi session, which has to be recorded here rather than waited
            # for. The handheld does log losing its AP, but not always and not promptly, and until this
            # was set the harness went on believing the session existed with the radio switched off.
            # Two consequences, both seen live on 2026-10-01 within ten minutes of a run starting:
            #
            #   * ordinary traffic was aimed at the handheld as though it were on Wi-Fi, counted as
            #     "both ends registered", and the loss blamed on the grid - two false findings, 12 s and
            #     60 s after the radio went off, while LoRa had not even finished coming up.
            #   * worse, the fallback audit keys on exactly this field, so it could not see a handheld on
            #     the backup during a LoRa round at all - blind to 85 of the 236 rounds in that schedule,
            #     which are the ones whose backup use most needs judging.
            #
            # last_node is deliberately kept: which AP it was on is still the useful fact about it.
            b.registered_node = None
            self.event("wifi_off", board=b.name, seconds=int(m.group(1)))
        elif RX["wifi_on"].search(line):
            b.wifi_off = False
            self.event("wifi_on", board=b.name)
        elif m := RX["lora_off"].search(line):
            b.lora_off = True
            b.lora_links.clear()
            self.event("lora_off", board=b.name, seconds=int(m.group(1)))
        elif RX["lora_on"].search(line):
            b.lora_off = False
            self.event("lora_on", board=b.name)
        elif RX["lora_resetting"].search(line):
            b.lora_reset_at = now
            # The links are not cleared here. The firmware keeps its peer table across a module reset and
            # logs no "Link up" afterwards, so clearing this copy made every reset round report "its LoRa
            # peers did not come back" - three times on 2026-10-04, with the links up throughout. A link
            # that really drops says so with "Link down", which is handled below.
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
        elif m := RX["voice_caps"].search(line):
            b.can_talk = m.group(1) == "talk and listen"
            b.can_hear = m.group(1) != "off"
        elif m := RX["voice_sent"].search(line):
            b.voice_sent[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
        elif m := RX["voice_heard"].search(line):
            b.voice_heard[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
        elif m := RX["voice_refused"].search(line):
            b.voice_refused = m.group(1)
        elif RX["lora_gone"].search(line):
            self.finding(f"{b.name}: its LoRa module stopped answering", board=b.name)
        elif m := RX["volume"].search(line):
            if b.volume is None:
                b.volume = m.group(1)
        elif m := RX["time"].search(line):
            # A board printing a grid time is the only evidence of the clock available when the APs are
            # on battery and nothing can be asked over a cable.
            if g := re.search(r"Grid time (\d+)", m.group(1)):
                b.grid_time = int(g.group(1))
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
        b.bb_off = b.lora_off = b.wifi_off = None
        b.grid_time = None
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
        cabled_idx = set(live_idx)
        # An access point with no cable is still an access point. With Fuji's USB dead on 2026-10-04 and
        # the other two on cables, every handheld registered with Fuji was "not registered with a live AP"
        # and no round ever started. One the BLE listener is hearing counts as live for a handheld.
        if self.eyes is not None:
            live_idx |= {i for i, r in self.eyes.aps().items()
                         if r.get("silent_s") is not None and r["silent_s"] <= DEAD_AP_S}
        for a in live_aps:
            missing = cabled_idx - {a.index} - a.links
            if missing:
                why.append(f"{a.name} has no link to AP {sorted(missing)}")
            if a.status is None:
                why.append(f"{a.name} has not reported status since it started")
            elif self.time_set and a.status["time"] == "UNSET":
                why.append(f"{a.name} grid time UNSET")
        for h in self.hhs:
            if not self.present(h):
                continue
            if h.wifi_off:
                continue      # its radio is off because this run switched it off (D78)
            if self.aps:
                if h.registered_node not in live_idx:
                    why.append(f"{h.name} not registered with a live AP")
            elif self.eyes is not None and self.eyes.troubled():
                pass   # an AP is mid-fault: this is not the moment to call the grid unsteady or steady
            elif self.eyes is not None and not self.eyes.has_room():
                # Every access point that is left says it is full, so this handheld has nowhere to go.
                # That is the grid being as healthy as it can be, not the grid being broken - and the
                # distinction matters, because the night ended with one access point on battery holding
                # four handhelds and two more that could not attach. Treating that as unsteady meant
                # nothing was ever tested again, which is the opposite of what a chaos run is for.
                pass
            elif h.wifi_off:
                # Its Wi-Fi is off because this run switched it off (D78). Being unregistered is the
                # round working, not the grid failing, and before registered_node was cleared on that
                # hook this branch could not be reached to say so.
                pass
            elif h.registered_node is None:
                # The APs are on battery, so this run holds no AP port and has no live AP index to
                # compare against. "Registered with a live AP" can then never be true, and the first
                # live run of the handheld LoRa round stalled on exactly that: the grid was healthy and
                # the gate said otherwise, so nothing was ever injected. A handheld being registered at
                # all is the evidence available here, and it is evidence about the same thing - an AP
                # accepted it, so that AP is alive whether or not a cable can reach it.
                why.append(f"{h.name} is not registered with any AP")
        if not self.aps and self.eyes is not None:
            # No cable to any AP, so their own beacons are the only account of them. An AP short of a
            # backbone link, or one that has just restarted, means the grid is not back yet.
            rows = self.eyes.aps()
            for i, reason in sorted(self.eyes.troubled().items()):
                name = rows.get(i, {}).get("name") or f"AP {i}"
                why.append(f"{name} (over BLE): {reason}")
        versions = {a.settings_version for a in live_aps if a.settings_version is not None}
        if len(versions) > 1:
            why.append(f"APs disagree on settings version {sorted(versions)}")
        return why

    def grid_has_time(self):
        """True once any AP that is up reports grid time it did not get from the PC."""
        return any(self.present(a) and a.status is not None and a.status["time"] != "UNSET" for a in self.aps)

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
            if self.aps_dead():
                # Nothing below this is worth waiting for: the grid has no access points left. Said here
                # rather than only in the loop, because a gap between experiments is minutes long and a
                # run should not sit through one after the thing it was testing has gone.
                return bool(until and until())
        return bool(until and until())

    def aps_dead(self):
        """True once every access point this run ever heard has been silent long enough to call it flat.

        Only in --until-aps-die, and only about access points *on battery*: a cable makes this meaningless
        and a restart makes it a false alarm, which is why the threshold is minutes. A beacon goes out
        every 500 ms, so ten minutes of nothing from all of them is not a fault any of them could survive.
        """
        if not self.args.until_aps_die or self.eyes is None or self.aps:
            return False
        quiet = self.eyes.silence()
        if not quiet:
            return False      # none heard yet: that is a start-up problem, not a death
        limit = self.args.ap_dead_after * 60
        if any(s is None or s < limit for s in quiet.values()):
            return False
        if self.ap_death_at is None:
            self.ap_death_at = time.monotonic()
            last = self.eyes.last_seen()
            for i in sorted(quiet):
                r = last.get(i, {})
                self.say(f"{r.get('name') or ('AP ' + str(i))} last heard {quiet[i]} s ago, "
                         f"after {r.get('uptime') or 'an unknown time'} on battery")
            self.event("aps_dead", silence_s=quiet,
                       last={str(i): last.get(i, {}) for i in quiet})
            self.finding("Every access point has gone silent: taking them as out of battery, and "
                         "ending the run. This is the measurement the run was for, not a fault.",
                         harness=True)
        return True

    def battery_section(self):
        """How long each access point's battery held, which is what --until-aps-die exists to measure."""
        if not self.args.until_aps_die:
            return []
        if self.eyes is None:
            return ["", "## Battery", "", "The access points were not watched, so nothing was measured.",
                    ""]
        last = self.eyes.last_seen()
        if not last:
            return ["", "## Battery", "", "No access point was ever heard, so nothing was measured.", ""]
        out = ["", "## How long the batteries held", "",
               "Uptime is the access point's own, from its status beacon, so it is time since it last "
               "started - a restart part way through (its own chaos, or a crash) resets it and the "
               "**Restarts** column says whether that happened.", "",
               "| AP | Last heard | Uptime then | Restarts during the run | Lowest heap | Verdict |",
               "|---|---|---|---|---|---|"]
        quiet = self.eyes.silence()
        moved = self.eyes.changes()
        for i in sorted(last):
            r = last[i]
            s = quiet.get(i)
            # Judged per access point. It used to need *every* access point to be dead before any was
            # called flat, and one that had dropped out of the listener altogether (no figure at all) was
            # printed as "still up at the end" hours after its battery had gone.
            if s is None:
                verdict = "flat (silent too long to time)"
            elif s >= self.args.ap_dead_after * 60:
                verdict = "flat"
            else:
                verdict = "still up at the end"
            out.append(f"| {r.get('name') or i} | {int(s) if s is not None else '?'} s ago | "
                       f"{r.get('uptime') or '?'} | {moved.get(i, {}).get('restarts', 0)} | "
                       f"{r.get('heap_kb')} KB | {verdict} |")
        out += ["", "A flat battery is read from silence, not measured: these boards cannot report their "
                "own charge, so the figure is how long the access point kept beaconing. It is a floor on "
                "the battery's life, not the battery's life.", ""]
        return out

    def audible_ok(self):
        """May this run make a noise right now? (D87)

        The window is read from the laptop's own clock, not the grid's: what matters is the hour where
        the handhelds are, and that is where this laptop is too. `--no-alerts` silences them whatever
        the hour, and `--audible-hours` moves the window for a run somebody is sitting with.
        """
        if self.args.no_alerts:
            return False
        lo, hi = self.audible_hours
        if lo == hi:
            return True            # an explicit all-day window
        hour = datetime.datetime.now().hour
        return lo <= hour < hi if lo < hi else (hour >= lo or hour < hi)

    def quiet_note(self):
        """Why no alert was raised, in words the report can print."""
        lo, hi = self.audible_hours
        return (f"outside the audible hours {lo:02d}:00-{hi:02d}:00 (D87)" if not self.args.no_alerts
                else "alerts are switched off for this run (--no-alerts)")

    def wifi_should_have_worked(self, h):
        """Could this handheld have had a Wi-Fi session right now? Returns (yes, why not).

        The question behind the whole audit. A handheld carrying its traffic on LoRa looks perfectly
        healthy from the outside - messages arrive, nothing logs an error - so a Wi-Fi fault underneath
        it is invisible unless something asks whether the backup was *needed*. It usually was: an AP
        this run is holding down, a radio it switched off, a grid with no room left. When it was not,
        that is a bug, and on a handheld with no second radio it is an outage.
        """
        now = time.monotonic()
        if h.wifi_off:
            return False, "its Wi-Fi is switched off by this run (D78)"
        if h.held or now - h.injected_at < FAULT_GRACE_S:
            return False, "it is under a fault from this run"
        if self.eyes is not None and self.eyes.troubled():
            return False, "an AP is in trouble on its own"
        live = [a for a in self.aps
                if self.present(a) and not a.held and now - a.injected_at >= FAULT_GRACE_S]
        heard = self.eyes.aps() if self.eyes is not None else {}
        up = [i for i, r in heard.items()
              if r["heard"] and (r["silent_s"] or 0) <= BLE_SILENT_S] if heard else []
        if not live and not up:
            return False, "no AP was up"
        if self.eyes is not None and heard and not self.eyes.has_room():
            return False, "every AP it can hear is full"
        where = (", ".join(a.name for a in live) or ", ".join(f"AP {i}" for i in sorted(up)))
        return True, f"{where} was up with room"

    def close_fallback(self, h, ep, now):
        """A handheld got its Wi-Fi session back: file the episode."""
        ep["seconds"] = round(now - ep.pop("since"), 1)
        ep["node"] = h.registered_node
        self.fallback.pop(h.name, None)
        self.fallback_log.append(ep)
        self.event("fallback_ended", **ep)

    def fallback_audit(self, now):
        """Watch every handheld that has no Wi-Fi session, and say whether it should have had one.

        This is the check that the night of 2026-10-01 needed and did not have. A handheld sat on LoRa
        for minutes with a usable AP at -47 dBm in earshot, because of a scoring bug, and the run called
        it a success: every message arrived, carried by the second radio. The backup hid the fault. Not
        every handheld will have that backup, so an unjustified fallback is reported as a finding whether
        or not anything was lost.
        """
        if now < self.next_fallback:
            return
        self.next_fallback = now + FALLBACK_EVERY_S
        for h in self.hhs:
            ep = self.fallback.get(h.name)
            if not self.present(h):
                continue
            if h.registered_node is not None:
                if ep is not None:
                    self.close_fallback(h, ep, now)
                continue
            ok, why = self.wifi_should_have_worked(h)
            # The board's profile is the fallback answer, because `lora` has not been typed at every
            # handheld yet in the first half-minute of a run - and the earliest episodes are exactly
            # then, as the ports open. Reading an empty table as "no module" mislabelled the two boards
            # that have one. D77: F4B is the only handheld profile with LoRa pins.
            fitted = (self.lora_last.get(h.name) or {}).get("fitted")
            has_radio = bool(fitted) if fitted is not None else h.board == "F4B"
            if ep is None:
                ep = {"handheld": h.name, "since": now, "t": self.now_s(), "last_node": h.last_node,
                      "has_lora": has_radio, "unjustified_s": 0.0, "reason": why, "reported": False}
                self.fallback[h.name] = ep
                self.event("fallback_began", **{k: v for k, v in ep.items() if k != "since"})
            ep["reason"] = why
            if ok:
                ep["unjustified_s"] += FALLBACK_EVERY_S
            if ok and not ep["reported"] and ep["unjustified_s"] >= FALLBACK_GRACE_S:
                ep["reported"] = True
                carried = "its traffic is crossing on LoRa, so nothing looked broken" if has_radio else \
                          "it has no second radio, so it is simply offline"
                self.finding(
                    f"{h.name} has had no Wi-Fi session for {int(ep['unjustified_s'])} s with no reason: "
                    f"{why}, and no fault was injected. {carried.capitalize()}. "
                    f"A handheld with no backup radio would be off the grid for this long.",
                    handheld=h.name, unjustified_s=ep["unjustified_s"], has_lora=has_radio,
                    last_node=h.last_node, kind="unjustified_fallback")

    def backup_section(self):
        """Every time a handheld leaned on the backup, and whether it had to.

        The owner's requirement, in his words: "if it is using a backup we need to make sure that there
        was a good reason to use the backup", because some handhelds will not have one.
        """
        eps = self.fallback_log + [dict(e, seconds=round(time.monotonic() - e["since"], 1))
                                   for e in self.fallback.values()]
        out = ["", "## Did anything lean on the backup, and did it need to?", "",
               "Every stretch a handheld spent with **no Wi-Fi session**. A handheld with a LoRa module "
               "keeps working through one of these and looks healthy from the outside, which is exactly "
               "how a Wi-Fi fault stays hidden - so each one is judged on whether Wi-Fi *could* have "
               "worked, not on whether messages arrived. **Unjustified** means an AP was up, had room, "
               "and no fault was injected: a bug, and an outage on any handheld without a second radio.",
               ""]
        carried = [p for p in self.probes if "backup" in (p.get("sender_path"), p.get("receiver_path"))]
        avoidable = [p for p in carried if p.get("wifi_possible")]
        if carried:
            out += [f"**{len(carried)} message(s) crossed with an end on the backup**, "
                    f"{sum(p['result'] == 'delivered' for p in carried)} of them delivered. "
                    f"**{len(avoidable)} of those should not have needed it** - Wi-Fi was available at "
                    f"the moment they were sent. Each one is a message that a handheld without a second "
                    f"radio would have had no way to send.", ""]
        if not eps:
            out += ["No handheld was ever without a Wi-Fi session.", ""]
            return out
        bad = [e for e in eps if e.get("unjustified_s", 0) >= FALLBACK_GRACE_S]
        brief = [e for e in eps if 0 < e.get("unjustified_s", 0) < FALLBACK_GRACE_S]
        out += [f"{len(eps)} episode(s); **{len(bad)} unjustified for {FALLBACK_GRACE_S} s or longer** (each is "
                f"a finding), and **{len(brief)} more with a shorter unjustified stretch**, listed below and "
                f"not raised as findings.", "",
                "| Handheld | Started (s) | Lasted (s) | Unjustified (s) | Second radio | Why it had none |",
                "|---|---|---|---|---|---|"]
        for e in sorted(eps, key=lambda x: -x.get("unjustified_s", 0))[:40]:
            out.append(f"| {e['handheld']} | {e['t']} | {e.get('seconds', 'still going')} | "
                       f"{int(e.get('unjustified_s', 0)) or ''} | {'yes' if e.get('has_lora') else 'NO'} | "
                       f"{e.get('reason') or ''} |")
        out += ["", "A justified episode is the grid working: an AP held down by this run, a radio it "
                "switched off (D78), or every AP genuinely full. Those are not faults, and they are "
                "listed so the unjustified ones can be told apart rather than lost among them.", ""]
        return out

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
        self.fallback_audit(now)
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
        """Everything faulty right now: boards this run is holding, and APs their own chaos is breaking.

        The second half is why a whole-system night can be read at all. An AP that is restarting or has
        lost a backbone link is in exactly the state this harness would have put it in with a reset line,
        and a message that failed while that was true is not evidence of a bug.
        """
        now = time.monotonic()
        mine = [b.name for b in self.boards if b.held or now - b.injected_at < FAULT_GRACE_S]
        # A radio switched off by a LoRa round is a fault too. It was left out, so on 2026-10-04 a group
        # message lost five seconds after ESP-NOW was turned off on two access points was reported as
        # lost "while both were registered" with no fault named - true, and no help to whoever reads it.
        for b in self.boards:
            off = [what for what, flag in (("backbone off", b.bb_off), ("LoRa off", b.lora_off),
                                           ("Wi-Fi off", b.wifi_off)) if flag]
            if off and b.name not in mine:
                mine.append(f"{b.name} ({', '.join(off)})")
        if self.eyes is None:
            return mine
        theirs = self.eyes.troubled()
        rows = self.eyes.aps()
        return mine + [f"{rows.get(i, {}).get('name') or ('AP ' + str(i))} (its own chaos)"
                       for i in sorted(theirs)]

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
        self.handheld_probe()

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

    def handheld_probe(self, sender=None, receiver=None, kind=None):
        """Send one token message between two handhelds and remember it until it lands.

        Drawn at random by the ordinary traffic, or aimed by a round that needs a particular pair - a
        LoRa round has to send *to and from* the handheld whose Wi-Fi is off, which is the whole claim
        under test. Returns the token, or None if nothing was sent.
        """
        if sender is None:
            senders = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
            if not senders:
                # Every handheld is on the backup, or none is registered. Sending anyway is the point:
                # the harness used to go quiet in exactly the state it most needed to measure, so a grid
                # carried entirely by LoRa produced no traffic and therefore no evidence either way.
                senders = [h for h in self.hhs if self.present(h)]
            if not senders:
                return None
            sender = self.rng.choice(senders)
        elif not self.present(sender):
            return None
        if receiver is None:
            receiver = self.rng.choice([h for h in self.hhs if h is not sender])
        # --no-alerts (a night run): announcements and urgent broadcasts take over every handheld's screen,
        # and urgent ones sound even with the volume off (D40), so only 1:1 and group messages are sent, and
        # none at all while grid time is unset (then urgent is all a handheld may send, D6).
        quiet = not self.audible_ok()
        if kind is None and quiet and not self.time_set:
            # Nothing but an urgent broadcast may go out with no grid time (D6), and that is the one
            # thing the hour forbids, so this pass sends nothing at all.
            return None
        if kind is None:
            kinds = (["direct"] if quiet
                     else ["broadcast", "direct"]) + (["group"] if self.group else [])
            kind = self.rng.choice(kinds) if self.time_set else "urgent"
        # "announce" is a plain broadcast, which is what an announcement is on the wire (D56); it is named
        # separately so the report can tell an announcement apart from the background broadcast traffic.
        # "safe" is D66's all clear, which only reached serial today - `send safe <text>`.
        target = {"broadcast": "all", "announce": "all", "direct": str(receiver.index),
                  "group": self.group, "urgent": "urgent", "safe": "safe"}[kind]
        self.probe_n += 1
        token = f"CX{self.run_id}-{self.probe_n:05d}"
        # The path each end was on, written down at send time so the log can be analysed afterwards
        # without re-deriving it: "wifi" means a session with an AP, "backup" means no session, so
        # anything that crosses goes over LoRa. wifi_possible says whether that was avoidable.
        paths = {}
        for role, b in (("sender", sender), ("receiver", receiver)):
            paths[role + "_path"] = "wifi" if b.registered_node is not None else "backup"
        if "backup" in paths.values():
            ok, why = self.wifi_should_have_worked(
                sender if sender.registered_node is None else receiver)
            paths["wifi_possible"] = ok
            paths["backup_reason"] = why
        p = {"token": token, "kind": kind, "sender": sender.name, "receiver": receiver.name,
             "mono": time.monotonic(),   # the clock probe_tick measures the deadline against, always
             "t": self.now_s(), "sender_node": sender.registered_node, "receiver_node": receiver.registered_node,
             "faults_at_send": self.active_faults(), "drops": (sender.drops, receiver.drops), **paths}
        if not self.write(sender, f"send {target} {token}"):
            return None
        self.pending[token] = p
        return token

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
        def shared_groups():
            for h in self.hhs:
                h.groups.clear()
                self.write(h, "groups")
            self.wait(5, until=lambda: all(h.groups for h in self.hhs))
            return [g for g in (self.hhs[0].groups if self.hhs else {}) if all(h.groups.get(g) for h in self.hhs)]

        shared = shared_groups()
        if not shared and len(self.hhs) >= 2:
            # No group holds every handheld under test, so group messages and group voice would go
            # untested for the whole run - which is what happened, unnoticed, in every run before this one.
            # Make one. It stays on the grid afterwards (groups are sticky, D48) and the next run reuses it.
            devices = ",".join(str(h.index) for h in self.hhs)
            maker = self.hhs[0]
            if CHAOS_GROUP in maker.group_ids:
                command = f"group set {maker.group_ids[CHAOS_GROUP]} {CHAOS_GROUP} {devices}"
            else:
                command = f"group new {CHAOS_GROUP} {devices}"
            self.say(f"No group has every handheld under test; asking {maker.name} for one: {command}")
            self.write(maker, command)
            self.wait(8)
            shared = shared_groups()
            if not shared:
                self.finding(f"The group every handheld should share could not be made ({command}); group "
                             f"messages and group voice are left out of this run")
        self.group = (CHAOS_GROUP if CHAOS_GROUP in shared else shared[0]) if shared else None
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
        if not self.aps and not self.hhs:
            raise SystemExit("No board answered; nothing was injected")
        self.firmware_gate()
        if not self.aps:
            # The APs are on battery (D81/D82): no serial cable, so no AP-side fault can be *typed* at
            # them. They still attack themselves when armed over BLE (D82, D85), so a whole-system night
            # is possible - what this side needs is to *see* them, or it would blame the grid for what an
            # AP outage was doing. That is what BleEyes is for.
            self.say("No AP is on USB: the APs are on battery. AP-side faults are asked for over BLE "
                     "(D90), one short session each, and every one restores itself on the AP's clock.")
            self.event("no_aps_on_usb")
            self.start_ble_eyes()
            self.start_ble_hands()
            if not self.args.lora:
                self.say("Nothing for this side to inject without --lora, because every other fault "
                         "needs an AP port. The APs' own faults still land if they are armed.")
        else:
            # Some on cables and some not (2026-10-04: Fuji's USB serial went dead mid-night). The cabled
            # ones are driven as always; the others are only watched, so a handheld registered with one
            # is known to be on a live access point and the run is not held up waiting for it.
            try:
                mapped = {d["node_index"] for d in load_map().get("devices", [])
                          if d.get("role") == "N" and isinstance(d.get("node_index"), int)}
            except Exception:
                mapped = set()
            if mapped - {a.index for a in self.aps}:
                self.say("Some access points are not on USB: they are watched over BLE and not faulted.")
                self.start_ble_eyes()

    def firmware_gate(self):
        """Refuse to start unless every board is running the image that is in the build folder.

        On 2026-10-03 a twelve-hour run to flat batteries was started with one access point still on the
        firmware from before three fixes, because it could not be reached over USB the night before and
        the run began on a one-line warning. Everything that access point touched that day was wasted.
        A warning is not enough; the harness now stops.

        The check is the build's own fingerprint: each image prints the start of its ELF's SHA-256 as it
        boots, the harness resets every board as it starts and so sees that line, and it is compared with
        the ELF in the build folder for that board's firmware and chip. A board that printed nothing, an
        image that was never built, and an access point with no cable are all **not verified**, and not
        verified is treated as wrong - an access point on battery is exactly the one that went unchecked.

        --allow-firmware-mismatch starts anyway and records every problem as the first finding of the run,
        so a report can never again be read without knowing what it was run on.
        """
        try:
            bench = load_map()
        except Exception:
            bench = {}
        kinds = bench.get("boards", {})
        problems = []
        for b in self.aps + self.hhs:
            target = (kinds.get(b.board) or {}).get("target")
            project, elf = ("node", "lg_node.elf") if b.is_ap else ("handheld", "lg_handheld.elf")
            path = ROOT / "firmware" / project / f"build-{target}" / elf
            if not target or not path.exists():
                problems.append(f"{b.name}: no built {project} image for {target or 'board ' + str(b.board)} to compare with")
                continue
            want = hashlib.sha256(path.read_bytes()).hexdigest()
            if not b.elf_sha:
                problems.append(f"{b.name}: did not print its firmware fingerprint as it started")
            elif not want.startswith(b.elf_sha):
                problems.append(f"{b.name}: running {b.elf_sha}, the current {project} build is {want[:len(b.elf_sha)]}")
        on_usb = {b.index for b in self.aps}
        for d in bench.get("devices", []):
            if d.get("role") == "N" and isinstance(d.get("node_index"), int) and d["node_index"] not in on_usb:
                problems.append(f"{d.get('node_name') or d.get('name')}: not on USB, so its firmware cannot be read")
        self.event("firmware_gate", problems=problems,
                   fingerprints={b.name: b.elf_sha for b in self.aps + self.hhs})
        if not problems:
            self.say(f"Firmware: all {len(self.aps) + len(self.hhs)} boards are running the current build")
            return
        for p in problems:
            self.say(f"Firmware: {p}")
        if getattr(self.args, "allow_firmware_mismatch", False):
            self.finding("THIS RUN STARTED ON FIRMWARE THAT WAS NOT VERIFIED (--allow-firmware-mismatch): "
                         + "; ".join(problems))
            return
        raise SystemExit(
            "\nNot started: " + str(len(problems)) + " board(s) are not verified as running the current build.\n"
            "Flash the boards named above (python tools/flash.py <names> --no-build), or pass\n"
            "--allow-firmware-mismatch to run anyway and have this recorded as the run's first finding.\n"
            "Nothing was injected.")

    def start_ble_hands(self):
        """The BLE fault path (D90). Needs the admin password; without it the APs are only watched."""
        self.hands = None
        self.ble_slots = {}
        password = self.args.admin_password or os.environ.get("LG_ADMIN_PASSWORD")
        if self.args.no_ble:
            self.say("--no-ble: the access points will not be faulted this run")
            return
        if not password:
            self.say("No --admin-password (or LG_ADMIN_PASSWORD): the access points will be watched but "
                     "not faulted. Give it to make this a whole-system night.")
            return
        hands = BleHands(self.args.secrets, password, self.names_by_index())
        if self.eyes is not None:
            self.eyes.close()          # one BLE job at a time: the scan for APs, then the eyes again
        ok = hands.start()
        if self.eyes is not None:
            self.eyes.start()
        if not ok:
            self.say(f"No access point answered over BLE ({hands.error or 'none found'}); they will be "
                     "watched but not faulted")
            self.event("ble_hands_failed", error=hands.error)
            return
        self.hands = hands
        for n, ap in enumerate(sorted(hands.aps)):
            self.ble_slots[f"AP{n + 1}"] = ap
        self.say("Access points reachable for faults over BLE: "
                 + ", ".join(f"{slot} = AP {ap} {self.names_by_index().get(ap, '')}".rstrip()
                             for slot, ap in self.ble_slots.items()))
        self.event("ble_hands", aps=sorted(hands.aps))

    def names_by_index(self):
        """AP index -> bench name, from the device map, for the eyes and the hands alike."""
        try:
            import grid_watch as gw
            return gw.bench_ap_names()
        except Exception:
            return {}

    def ble_fault(self, ap, kind, seconds):
        """One fault over BLE with the eyes paused around it. Returns the acknowledgement or None."""
        if self.eyes is not None:
            self.eyes.close()
        try:
            return self.hands.fault(ap, kind, seconds)
        except Exception as e:
            self.say(f"  AP {ap}: fault {kind} not acknowledged ({type(e).__name__}: {e})")
            return None
        finally:
            if self.eyes is not None:
                self.eyes.start()

    def ble_backbone_whole(self):
        """Every access point the beacons hear has a link to every other one heard."""
        if self.eyes is None:
            return True
        rows = self.eyes.aps()
        live = [i for i, r in rows.items()
                if r["heard"] and (r["silent_s"] or 0) <= BLE_SILENT_S and r["links"] is not None]
        return all(rows[i]["links"] >= len(live) - 1 for i in live)

    def run_ble_experiment(self, e):
        """An AP fault asked for over BLE (D90): the AP it names breaks itself and comes back on its own."""
        missing = [s for s in e["victims"] if s not in self.ble_slots]
        if missing or self.hands is None:
            self.say(f"#{e['n']} skipped: no BLE access point in slot {', '.join(missing) or '?'}")
            self.event("skipped", n=e["n"], kind=e["kind"], why="no BLE access point in slot")
            return
        aps = [self.ble_slots[s] for s in e["victims"]]
        names = self.names_by_index()
        # An access point whose battery is flat cannot be asked for anything. On 2026-10-03 the harness
        # went on asking Everest for faults for hours after it had died, and each one was a finding.
        rows = self.eyes.aps() if self.eyes else {}
        gone = [a for a in aps if (rows.get(a) or {}).get("silent_s") is not None
                and rows[a]["silent_s"] > DEAD_AP_S]
        if gone:
            self.say(f"#{e['n']}: {', '.join(names.get(a, f'AP {a}') for a in gone)} has gone silent "
                     f"(battery); not asked")
            aps = [a for a in aps if a not in gone]
        if not aps:
            self.event("skipped", n=e["n"], kind=e["kind"], why="every access point named has gone silent")
            return
        victims = [names.get(a, f"AP {a}") for a in aps]
        exp = {**e, "slots": e["victims"], "victims": victims, "ap_indices": aps, "started": self.now_s(),
               "failover": [], "result": None, "unconfirmed": []}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        kind = "backbone" if e["kind"] == "ble_bb_off" else "restart"
        seconds = e["outage_s"] if kind == "backbone" else 0
        # Only a restart takes handhelds off an access point. A backbone-off leaves them where they are,
        # so marking them as "failing over" was wrong - and the marker was never cleared, so the next time
        # such a handheld registered anywhere, hours later, the summary counted it as a failover that had
        # taken hours. That is where "23 moves, median 2,127 s" came from on 2026-10-03: no such move
        # is in the event log.
        if kind == "restart":
            for h in self.hhs:
                if self.present(h) and h.registered_node in set(aps):
                    h.failover = (time.monotonic(), exp)
        self.say(f"#{e['n']} {e['kind']} {', '.join(victims)}"
                 + (f" for {seconds} s" if seconds else "") + " (over BLE)")
        self.event("fault", **{k: v for k, v in exp.items() if k not in ("at_s", "failover")})
        before = {a: dict(self.eyes.aps().get(a) or {}) for a in aps} if self.eyes else {}
        acked = []
        for a in aps:
            ack = self.ble_fault(a, kind, seconds or 60)
            if ack is None:
                # No answer, or a refusal. The usual reason on this bench is an access point still on
                # firmware without OP_FAULT (Everest, 2026-10-03): the run says so, and it is not blamed on
                # the beacons, which cannot show a fault that was never performed.
                exp["unconfirmed"].append(f"{names.get(a, a)} did not acknowledge the fault")
                self.finding(f"#{e['n']} {names.get(a, a)} did not acknowledge a {kind} fault over BLE - "
                             f"firmware without OP_FAULT, or out of range", n=e["n"], board=names.get(a, str(a)))
            else:
                acked.append(a)
        # Confirmation. A restart shows in the beacons - the boot counter moves - and an AP that said yes
        # and did not restart is a finding. A backbone-off does *not* show for a short window: D74 calls a
        # link down only after three missed keepalives, about three minutes, so an 18 s outage is over
        # before any beacon could reflect it. For that one the acknowledgement is the confirmation; what
        # the outage did to the traffic is what the probes measure.
        if kind == "restart" and acked:
            def restarted(a):
                row = self.eyes.aps().get(a) if self.eyes else None
                return bool(row and before.get(a) and row["boot"] is not None and row["boot"] != before[a].get("boot"))
            if not self.wait(75, until=lambda: all(restarted(a) for a in acked)):
                for a in acked:
                    if not restarted(a):
                        exp["unconfirmed"].append(f"{names.get(a, a)} restart: the beacons never showed it")
                        self.finding(f"#{e['n']} {names.get(a, a)} acknowledged a restart and its boot counter "
                                     f"never moved", n=e["n"], board=names.get(a, str(a)))
        if seconds:
            self.wait(seconds)
        released = time.monotonic()
        self.event("released", n=e["n"], unconfirmed=exp["unconfirmed"])
        if not acked:
            exp["result"] = "done"   # nothing was taken out, so there is nothing to recover from
        elif kind == "restart":
            if self.wait(self.args.recovery_timeout, until=self.ble_backbone_whole):
                exp["recovery_s"] = round(time.monotonic() - released, 1)
                exp["result"] = "recovered"
                self.say(f"#{e['n']} backbone whole again {exp['recovery_s']} s after release")
            else:
                exp["result"] = "not recovered"
                self.finding(f"#{e['n']} backbone not whole {self.args.recovery_timeout} s after "
                             f"{', '.join(victims)} came back", n=e["n"])
        else:
            exp["result"] = "done"   # a backbone-off restores itself; the beacons cannot time it (D74)
        for h in self.hhs:
            if h.failover and h.failover[1] is exp:
                h.failover = None    # as the cabled round does: a move after this is not this round's
        self.phase = ""

    def start_ble_eyes(self):
        """Listen to the APs' status beacons, so a run with no cable to them is not blind."""
        if self.args.no_ble:
            self.say("--no-ble: this run will not watch the APs at all, so a message lost while an AP "
                     "was down will be reported as a finding. Only useful when no AP is armed.")
            return
        # The device map first, the cabled boards over it: an access point with no cable is named too.
        try:
            names = {d["node_index"]: d["node_name"] for d in load_map().get("devices", [])
                     if d.get("role") == "N" and isinstance(d.get("node_index"), int)}
        except Exception:
            names = {}
        names.update({b.index: b.ap_name for b in self.aps if b.ap_name})
        self.eyes = BleEyes(self.args.secrets, names)
        if not self.eyes.start():
            self.finding(f"The APs could not be watched over BLE ({self.eyes.error}), so an AP fault "
                         "would be invisible to this run and its losses blamed on the grid",
                         harness=True)
            self.eyes = None
            return
        self.say("Watching the APs over BLE (D68): their restarts, backbone links and brownouts are "
                 "recorded, and a probe in flight while an AP is in trouble is not counted against the "
                 "grid.")
        if self.wait(25, until=lambda: any(r["has_health"] for r in self.eyes.aps().values())):
            rows = self.eyes.aps()
            for i in sorted(rows):
                r = rows[i]
                links = "no status frame yet" if r["links"] is None else f"{r['links']} link(s)"
                self.say(f"  heard {r['name'] or ('AP ' + str(i))} (AP {i}): boot {r['boot']}, {links}"
                         + (f", {r['handhelds']} handheld(s), {r['brownouts']} brownout(s) so far"
                            if r["has_health"] else ""))
            self.event("ble_eyes", aps=rows)
        else:
            why = f" ({self.eyes.error})" if self.eyes.error else ""
            self.finding(f"No AP was heard over BLE within 20 s{why}, so this run cannot tell an AP "
                         "fault from a grid fault", harness=True)

    def close_eyes(self):
        if self.eyes is not None:
            self.eyes.close()

    def ap_section(self):
        """What the access points did to themselves, from their beacons (D68).

        A run that holds no cable to them still has to account for them, or a night reads as a clean bill
        of health for a grid whose access points were restarting all the way through it. Restarts and
        brownouts are counters kept in NVS, so only their movement during this run means anything.
        """
        if self.eyes is None:
            return ["", "## The access points", "",
                    "Not watched over BLE this run, so nothing here is an account of them.", ""]
        rows = self.eyes.aps()
        if not rows:
            return ["", "## The access points", "",
                    "None was heard over BLE, so this run has no account of them at all - which also "
                    "means any message it called lost may have been lost to an AP that was down.", ""]
        moved = self.eyes.changes()
        out = ["", "## The access points, over BLE (they hold no cable)", "",
               "| AP | Heard | Boot | Links | Handhelds | Restarts here | Brownouts here | Lowest heap |",
               "|---|---|---|---|---|---|---|---|"]
        for i in sorted(rows):
            r = rows[i]
            d = moved.get(i, {})
            out.append(f"| {r['name'] or i} | {'yes' if r['heard'] else 'NO'} | {r['boot']} | "
                       f"{r['links']} | {r['handhelds']} | {d.get('restarts', 0)} | "
                       f"{d.get('brownouts', 0)} | {r['heap_kb']} KB |")
        out.append("")
        if moved:
            out.append("What moved during the run, and what it means:")
            out.append("")
            for i in sorted(moved):
                d = moved[i]
                bits = []
                if d["restarts"]:
                    bits.append(f"**restarted {d['restarts']} time(s)** (last reason: {d['reset']})")
                if d["brownouts"]:
                    bits.append(f"**{d['brownouts']} brownout(s)** - a supply problem, not a fault "
                                "anything here injected")
                out.append(f"- {d['name'] or ('AP ' + str(i))}: " + "; ".join(bits)
                           + f"; lowest heap {d['heap_kb']} KB")
            out.append("")
            out.append("A restart here is **not** necessarily a bug: an armed chaos build restarts itself "
                       "on purpose (D82). Its own journal says which, and this side cannot: read it with "
                       "`python tools/chaos_ble.py --ap <name> journal`.")
        else:
            out.append("No access point restarted or browned out while this run was going.")
        out.append("")
        return out

    def release_port(self, b):
        """Both lines low, then close: a plain close holds CP210x and CH34x boards in reset.

        Its reader is stopped and joined first. Closing the port under a thread blocked in read()
        is a segmentation fault, not an exception, so no amount of try/except around the close
        catches it - the thread has to be gone before the handle is."""
        b.stop_reader.set()
        if b.reader_thread is not None:
            b.reader_thread.join(timeout=2)
            b.reader_thread = None
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
            if any(a.status and a.status["time"] != "UNSET" for a in self.aps if self.present(a)):
                return True
            # With the APs on battery there is no AP status line to read. A handheld's clock is evidence
            # of the same fact: a handheld only has grid time because an AP gave it one (D53, D60).
            return not self.aps and any(h.grid_time for h in self.hhs if self.present(h))

        # A GPS on MAIN (D63) or a handheld's clock usually gives the grid time within seconds; the PC
        # typing `time set` then only fights it (MAIN refuses while its GPS has a fix, another AP
        # would accept and hand-set time until MAIN's next fix took it back).
        if self.wait(STATUS_EVERY_S + 30, until=time_seen):
            self.time_set = True
            self.event("time_from_grid", source=self.time_source or "unknown")
            self.say(f"grid time came from {self.time_source or 'the grid'}; the PC did not set it")
            return
        if not self.aps:
            # Nothing here can set the time: `time set` is an AP command and the PC is not a grid member
            # (D25). Either the grid has a clock of its own or this run records that it did not.
            self.finding("No AP is on USB and no handheld reported grid time, so the time could not be "
                         "confirmed; rounds that need it will be skipped")
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
        if e["kind"] == "no_net":
            return self.run_no_net_experiment(e)
        if e["kind"] == "alerts":
            return self.run_alerts_experiment(e)
        if e["kind"] == "voice":
            return self.run_voice_experiment(e)
        if e["kind"] in LORA_KINDS:
            return self.run_lora_experiment(e)
        if e["kind"] in BLE_KINDS:
            return self.run_ble_experiment(e)
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

    def alert_burst(self, sender):
        """Announcement, SOS, all clear - in that order, from one handheld, as a person would use them.

        Returns the tokens, which the ordinary probe machinery resolves and counts like any other message.
        The order matters: the all clear is only meaningful after the SOS it stands down, and D86's
        re-stamping applies to both of them, so sending them apart would not exercise the same path.
        """
        tokens = []
        # D6: with no grid time an announcement is refused by design, and counting that as a failed
        # message would manufacture a finding out of correct behaviour. Urgent and the all clear are the
        # two a handheld may always send, so those are what an alert round does until time is set.
        sequence = ("announce", "urgent", "safe") if self.time_set else ("urgent", "safe")
        for kind in sequence:
            tok = self.handheld_probe(sender=sender, kind=kind)
            if tok:
                tokens.append(tok)
                self.wait(6)   # let each land before the next takes over every screen
        return tokens

    def run_alerts_experiment(self, e):
        """The emergency path end to end: announcement, SOS, and standing it down.

        Driven as its own round rather than left to the random traffic because the three are a sequence,
        and because the all clear is the one alert that had never been tested on this bench at all.
        """
        if not self.audible_ok():
            self.say(f"#{e['n']} alerts skipped: {self.quiet_note()}")
            self.event("skipped", n=e["n"], kind="alerts", why=self.quiet_note())
            return
        senders = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        if not senders:
            senders = [h for h in self.hhs if self.present(h)]
        if not senders:
            self.say(f"#{e['n']} skipped: no handheld present to raise an alert")
            return
        v = self.rng.choice(senders)
        exp = {**e, "slots": e.get("victims", []), "victims": [v.name], "started": self.now_s(),
               "failover": [], "result": None, "what": "announcement, SOS, and the all clear"}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        self.say(f"#{e['n']} alerts from {v.name}: announcement, SOS, all clear")
        self.event("fault", **{k: val for k, val in exp.items() if k not in ("at_s", "failover")})
        tokens = self.alert_burst(v)
        # Wait for the last of them to be resolved by probe_tick, rather than guessing.
        self.wait(PROBE_DEADLINE_S + 10, until=lambda: not any(tk in self.pending for tk in tokens))
        got = [p for p in self.probes if p["token"] in tokens]
        exp["sent"] = len(tokens)
        exp["delivered"] = sum(p["result"] == "delivered" for p in got)
        exp["result"] = "done"   # it took nothing out, so there was nothing to recover from
        exp["alerts"] = [{"token": p["token"], "kind": p["kind"], "result": p["result"]} for p in got]
        if not tokens:
            self.finding(f"#{e['n']} {v.name}: not one alert could even be queued", n=e["n"], board=v.name)
        self.phase = ""

    # ---- voice ----

    def voice_hearers(self, sender, scope, target):
        """Who must play this talk, and who must not.

        The second half is the point. A 1:1 talk reaching a third handheld, or a group talk reaching
        somebody who is not in the group, is the voice equivalent of reading someone else's message, and
        nothing else in this harness would notice it. Speaker capability comes from each board's own boot
        line rather than a table of board codes, so a board that has been rewired reports itself.
        """
        heard_by = [h for h in self.hhs if h is not sender and self.present(h) and h.can_hear is not False]
        if scope == "all":
            return heard_by, []
        if scope == "group":
            members = [h for h in heard_by if h.groups.get(self.group)]
            return members, [h for h in heard_by if h not in members]
        one = [h for h in heard_by if h.index == target]
        return one, [h for h in heard_by if h not in one]

    def run_voice_experiment(self, e):
        """Push to talk, end to end, over the three scopes a person has.

        Voice was the one path this harness could not reach: it drives handhelds over their serial console
        and nothing there could hold a talk bar down, so every run until now reported a grid that carried
        text and said nothing about whether anybody could be heard on it. `demo` gives it a generated voice
        of a known length from any board, including the four with no microphone, and both ends count frames
        under a talk id - so the result is a loss figure per listener rather than a yes or no.

        The owner's requirement, 2026-10-02: the two boards with microphones must manage 1:1, a group and
        an announcement to everyone, "and that needs to be tested. The group and the announcement."
        """
        if not self.audible_ok():
            self.say(f"#{e['n']} voice skipped: {self.quiet_note()}")
            self.event("skipped", n=e["n"], kind="voice", why=self.quiet_note())
            return
        senders = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        if not senders:
            self.say(f"#{e['n']} skipped: no handheld on the grid to talk")
            self.event("skipped", n=e["n"], kind="voice", why="no handheld registered")
            return
        # A board with a microphone is preferred, because that is the one a person would be holding and
        # the one the owner asked about; any board can do it, which is how the other four get covered.
        mic = [h for h in senders if h.can_talk]
        v = self.rng.choice(mic or senders)
        scope = e.get("scope") or self.rng.choice(("direct", "group", "all"))
        target = 0
        if scope == "group" and not any(h.groups.get(self.group) for h in self.hhs if h is not v):
            scope = "all"          # nobody else is in the shared group: an announcement tests more
        if scope == "direct":
            others = [h for h in self.hhs if h is not v and self.present(h) and h.can_hear is not False]
            if not others:
                self.say(f"#{e['n']} skipped: nobody to talk to")
                self.event("skipped", n=e["n"], kind="voice", why="no listener present")
                return
            target = self.rng.choice(others).index
        who = {"direct": str(target), "group": self.group, "all": "all"}[scope]
        secs = e.get("talk_s") or self.rng.choice((3, 5, 10))

        expect, forbid = self.voice_hearers(v, scope, target)
        exp = {**e, "slots": e.get("victims", []), "victims": [v.name], "started": self.now_s(),
               "failover": [], "result": None, "scope": scope, "talk_s": secs,
               "what": f"a {secs} s talk to {('everyone' if scope == 'all' else who)}"}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        self.say(f"#{e['n']} voice from {v.name}: {secs} s to "
                 f"{'everyone' if scope == 'all' else 'group ' + who if scope == 'group' else 'device ' + who}"
                 f"; expecting {len(expect)} listener(s)")
        self.event("fault", **{k: val for k, val in exp.items() if k not in ("at_s", "failover")})

        before_sent = dict(v.voice_sent)
        for h in self.hhs:
            h.voice_refused = None
        on_lora = v.registered_node is not None and getattr(v, "on_lora", False)
        if not self.write(v, f"demo {who} {secs}"):
            self.finding(f"#{e['n']} {v.name}: the console would not take a talk", n=e["n"], board=v.name)
            exp["result"] = "done"
            self.phase = ""
            return
        # The talk stops itself, then the listeners' last frames have to drain: PREBUFFER_MS plus the
        # 1.5 s a listener waits before it decides the END was lost.
        self.wait(secs + 12, until=lambda: len(v.voice_sent) > len(before_sent))
        new = [k for k in v.voice_sent if k not in before_sent]
        if not new:
            why = v.voice_refused or "no result line from the sender"
            # D71: voice never goes over LoRa, so a handheld with no Wi-Fi refusing is correct behaviour.
            if on_lora or (v.voice_refused and "short" in v.voice_refused.lower()):
                self.say(f"#{e['n']} voice refused, correctly: {why}")
                exp.update(result="done", refused=why, correct_refusal=True)
            else:
                self.finding(f"#{e['n']} {v.name}: talked for {secs} s and reported nothing ({why})",
                             n=e["n"], board=v.name)
                exp.update(result="done", refused=why)
            self.phase = ""
            return
        talk_id = new[-1]
        frames, dropped, ms = v.voice_sent[talk_id]
        self.wait(4)   # the listeners' END lines trail the sender's by the prebuffer
        heard = {}
        for h in self.hhs:
            if talk_id in h.voice_heard:
                author, played, missing = h.voice_heard[talk_id]
                if author == v.index:
                    heard[h.name] = {"played": played, "missing": missing,
                                     "share": round(played / frames, 3) if frames else None}
        silent = [h.name for h in expect if h.name not in heard]
        extra = [h.name for h in forbid if h.name in heard]
        exp.update(result="done", frames=frames, dropped=dropped, ms=ms, talk_id=talk_id,
                   expected=[h.name for h in expect], heard=heard, silent=silent, uninvited=extra)
        self.say(f"#{e['n']} voice: {frames} frame(s) sent, {dropped} dropped at the sender; "
                 f"heard by {len(heard)} of {len(expect)}")
        if frames == 0:
            self.finding(f"#{e['n']} {v.name}: a {secs} s talk sent no frames at all", n=e["n"], board=v.name)
        if dropped and frames and dropped >= frames:
            self.finding(f"#{e['n']} {v.name}: every one of {frames} voice frame(s) was refused before the "
                         f"AP - the scope is not accepted on the send path", n=e["n"], board=v.name)
        if extra:
            self.finding(f"#{e['n']} a {scope} talk from {v.name} was played by {', '.join(extra)}, who was "
                         f"not addressed", n=e["n"], board=v.name)
        for name in silent:
            self.finding(f"#{e['n']} {name} heard nothing of a {scope} talk from {v.name} "
                         f"({frames} frame(s) sent)", n=e["n"], board=name)
        for name, got in heard.items():
            if got["share"] is not None and got["share"] < 0.5:
                self.finding(f"#{e['n']} {name} played only {got['played']} of {frames} voice frame(s) "
                             f"({int(got['share'] * 100)}%)", n=e["n"], board=name)
        self.phase = ""

    def run_no_net_experiment(self, e):
        """Switch the second radio off everywhere, so Wi-Fi has to stand on its own.

        The owner's requirement, and the lesson of 2026-10-01: "in some of these handhelds, we won't have
        those radios. So we need to catch them." Every other round leaves LoRa there to carry the traffic,
        which is how a Wi-Fi bug can pass a whole night unnoticed - messages arrived, so nothing looked
        wrong. With the module off, a handheld that cannot hold a Wi-Fi session is simply off the grid,
        exactly as a module-less handheld would be, and the loss is reported.

        `lora off <seconds>` restores itself from the handheld's own clock (HH_LORA_OFF_MAX_S), so a
        harness that dies in here cannot leave a handheld without its second radio, and a restart ends it.
        """
        have = [h for h in self.hhs
                if self.present(h) and (self.lora_last.get(h.name) or {}).get("fitted")]
        if not have:
            self.say(f"#{e['n']} skipped: no handheld with a LoRa module to take away")
            return
        if len([h for h in self.hhs if self.present(h)]) < 2:
            self.say(f"#{e['n']} skipped: needs two handhelds so messages have somewhere to go")
            return
        window = e["outage_s"]
        restore_s = min(HH_LORA_OFF_MAX_S, int(window) + 60)
        exp = {**e, "slots": e.get("victims", []), "victims": [h.name for h in have],
               "started": self.now_s(), "failover": [], "result": None, "window_s": window,
               "what": "every second radio off: Wi-Fi alone, as on a handheld with no module"}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        self.say(f"#{e['n']} no_net for {window} s: LoRa off on {', '.join(h.name for h in have)}")
        self.event("fault", **{k: val for k, val in exp.items() if k not in ("at_s", "failover")})
        first = len(self.probes)
        try:
            for h in have:
                self.write(h, f"lora off {restore_s}")
            self.wait(8)
            # Ordinary traffic keeps flowing through wait(); add one full alert sequence, because an SOS
            # with no second radio is the case that matters most.
            if self.audible_ok():
                self.alert_burst(self.rng.choice([h for h in self.hhs if self.present(h)]))
            else:
                self.say(f"#{e['n']} no alert in this window: {self.quiet_note()}")
            self.wait(max(0.0, window - 30))
        finally:
            for h in have:
                self.write(h, "lora on")
        during = [p for p in self.probes[first:] if p["clean"]]
        lost = [p for p in during if p["result"] != "delivered"]
        exp["messages"] = len(during)
        exp["lost"] = len(lost)
        exp["result"] = "done"   # it took nothing out, so there was nothing to recover from
        if during and lost:
            self.finding(f"#{e['n']} with every second radio off, {len(lost)} of {len(during)} messages "
                         f"between registered handhelds did not arrive. Nothing could fall back, which is "
                         f"the point: this is what a handheld without a module would have suffered.",
                         n=e["n"], lost=len(lost), of=len(during), kind="no_backup_loss")
        elif not during:
            self.say(f"#{e['n']} no message was exchanged while the radios were off; nothing proved")
        self.phase = ""

    def run_lora_handheld_experiment(self, e):
        """The handheld leg: take one handheld's Wi-Fi away and make it carry its own traffic over LoRa.

        This is the round the AP-side ones cannot cover. They take ESP-NOW away between APs, which proves
        LoRa carries *between APs*; nothing they do makes a handheld use its own module, because a handheld
        only reaches for LoRa when its own Wi-Fi session is gone (D74, D78). On 2026-09-30 that gap hid a
        real fault for as long as it existed: a LoRa-only handheld could receive a message and an SOS and
        could originate nothing but its announcement, and a chaos night would have been reported clean
        either way.

        Traffic goes *both* ways on purpose. Receiving was never broken; sending was.

        `wifi off <seconds>` restores itself from the handheld's own clock, so a harness that dies here
        cannot leave a handheld off the grid, and a restart ends the outage too.
        """
        slots = {b.slot: b for b in self.hhs}
        victims = [slots[s] for s in e["victims"] if s in slots]
        if len(victims) != 1 or not self.present(victims[0]):
            self.say(f"#{e['n']} skipped: {', '.join(e['victims'])} is not a present handheld")
            return
        v = victims[0]
        if (self.lora_last.get(v.name) or {}).get("fitted") is False:
            self.say(f"#{e['n']} skipped: {v.name} has no LoRa module")
            return
        others = [h for h in self.hhs if h is not v and self.present(h) and h.registered_node is not None]
        if not others:
            self.say(f"#{e['n']} skipped: no other registered handheld to exchange messages with {v.name}")
            return

        window = e["outage_s"]
        restore_s = min(HH_WIFI_OFF_MAX_S, int(window) + 60)   # the handheld restores it even if this dies
        e = {**e, "slots": e["victims"], "victims": [v.name]}
        exp = {**e, "started": self.now_s(), "failover": [], "result": None, "unconfirmed": [],
               "what": LORA_WHAT[e["kind"]], "window_s": window,
               "peer_handhelds": [h.name for h in others]}
        self.experiments.append(exp)
        self.phase = f"#{e['n']}"
        self.say(f"#{e['n']} {e['kind']} on {v.name} for {window} s: {exp['what']}")
        self.event("fault", **{k: val for k, val in e.items() if k != "at_s"})
        watched = [v] + [h for h in others if (self.lora_last.get(h.name) or {}).get("fitted")]
        before = self.lora_query(watched)
        exp["lora_before"] = {n: {k: val for k, val in s.items() if k != "peers"}
                              for n, s in before.items()}
        tokens = []
        try:
            # Let anything already in flight to or from this handheld finish first. A probe sent by the
            # ordinary traffic a moment before the window opens would otherwise be resolved as lost in
            # the first second of it, and counted clean, because the handheld's own "lost its AP" event
            # has not arrived yet - the first live run blamed exactly one message that way, 0.3 s after
            # the window opened. Waiting removes the race instead of excusing the finding.
            self.wait(LORA_HH_SETTLE_S, until=lambda: not any(
                v.name in (pr["sender"], pr["receiver"]) for pr in self.pending.values()))
            self.write(v, f"wifi off {restore_s}")
            if not self.wait(12, until=lambda: v.wifi_off):
                exp["unconfirmed"].append(f"{v.name} wifi off")
                self.finding(f"#{e['n']} {v.name}: no [NET] line confirmed Wi-Fi went off, so nothing "
                             "that follows can be attributed to the radio", n=e["n"], board=v.name)
            end = time.monotonic() + window
            while time.monotonic() < end and time.monotonic() < self.deadline:
                peer = self.rng.choice(others)
                # Both directions, the sending one first: it is the one that was broken.
                for sender, receiver in ((v, peer), (peer, v)):
                    tok = self.handheld_probe(sender=sender, receiver=receiver, kind="direct")
                    if tok:
                        tokens.append(tok)
                    self.wait(min(LORA_HH_PROBE_GAP_S, max(0.0, end - time.monotonic())))
            # A frame is seconds of airtime: wait for the last ones instead of calling them lost.
            self.wait(LORA_HH_SETTLE_S, until=lambda: not any(k in self.pending for k in tokens))
        finally:
            released = time.monotonic()
            self.write(v, "wifi on")             # explicit, although the hook restores itself
            self.phase = None
        self.event("released", n=e["n"], unconfirmed=exp["unconfirmed"])

        exp["rejoined"] = self.wait(HH_REJOIN_S, until=lambda: v.registered_node is not None)
        if not exp["rejoined"]:
            self.finding(f"#{e['n']} {v.name}: did not register with any AP within {HH_REJOIN_S} s of its "
                         "Wi-Fi coming back", n=e["n"], board=v.name)
        self.settle(exp, released)

        mine = [pr for pr in self.probes if pr["token"] in tokens]
        from_v = [pr for pr in mine if pr["sender"] == v.name]
        to_v = [pr for pr in mine if pr["receiver"] == v.name]
        exp["probes"] = {
            "from_victim": {"sent": len(from_v),
                            "delivered": sum(pr["result"] == "delivered" for pr in from_v)},
            "to_victim": {"sent": len(to_v),
                          "delivered": sum(pr["result"] == "delivered" for pr in to_v)},
            "unresolved": len([k for k in tokens if k in self.pending]),
        }
        after = self.lora_query(watched)
        exp["lora"] = self.lora_delta(before, after)
        out = (exp["lora"].get(v.name) or {}).get("frames_out")
        exp["frames_out"] = out

        # The findings this round exists to produce. Deliberately not excused by the victim "losing its
        # AP": it lost its AP because this round took it away, and carrying its traffic anyway is the claim.
        if from_v and exp["probes"]["from_victim"]["delivered"] == 0:
            self.finding(f"#{e['n']} {v.name}: not one of {len(from_v)} messages it sent while on LoRa "
                         "alone was delivered; the radio received but did not carry", n=e["n"],
                         board=v.name)
        if to_v and exp["probes"]["to_victim"]["delivered"] == 0:
            self.finding(f"#{e['n']} {v.name}: not one of {len(to_v)} messages sent to it while on LoRa "
                         "alone arrived", n=e["n"], board=v.name)
        if out == 0 and from_v:
            self.finding(f"#{e['n']} {v.name}: its LoRa frame count did not move while it sent "
                         f"{len(from_v)} message(s) with no Wi-Fi, so nothing reached the air",
                         n=e["n"], board=v.name)
        if v.wifi_off:
            self.finding(f"#{e['n']} {v.name}: Wi-Fi is still switched off after the round", n=e["n"],
                         board=v.name)
        self.event("lora_round", n=e["n"], probes=exp["probes"], frames_out=out, lora=exp["lora"])

    def run_lora_experiment(self, e):
        """A LoRa round. No board is taken out: the AP console's own hooks are used, every one of which
        restores itself from the AP's clock, so a dead harness cannot leave a radio off."""
        if e["kind"] in LORA_HH_KINDS:
            return self.run_lora_handheld_experiment(e)
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
        # Which handhelds have a module is read off the boards, not assumed from a board code: `lora`
        # says so, and a handheld with no module would make the round meaningless.
        if self.args.lora and self.hhs:
            self.lora_query(self.hhs)
        self.args._lora_hhs = [h.slot for h in self.hhs
                               if (self.lora_last.get(h.name) or {}).get("fitted")]
        if self.args.lora:
            self.say("handhelds with a LoRa module: "
                     + (", ".join(h.name for h in self.hhs if h.slot in self.args._lora_hhs) or "none"))
        ble_aps = len(getattr(self, 'ble_slots', {}) or {}) if not self.aps else 0
        self.args._ble_aps = bool(ble_aps)
        plan = make_schedule(random.Random(self.args.seed), len(self.aps) or ble_aps, len(self.hhs), self.args)
        print_schedule(plan, self.args)
        self.event("schedule", plan=plan)
        run_start = time.monotonic()
        run_end = run_start + self.args.hours * 3600
        for e in plan:
            if self.aps_dead():
                self.say(f"#{e['n']} and later not started: the access points are out of battery")
                break
            wait_s = e["at_s"] - (time.monotonic() - run_start)
            if wait_s > 0:
                self.wait(wait_s)
            if self.aps_dead():
                self.say(f"#{e['n']} and later not started: the access points are out of battery")
                break
            budget = e["outage_s"] + self.args.recovery_timeout + (
                LORA_RECOVERY_S if e["kind"] in LORA_KINDS else 0)
            if time.monotonic() + budget > run_end:
                self.say(f"#{e['n']} and later not started: they could not finish before the planned end")
                break
            self.rounds_reached += 1
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
        # "recovery_s" belongs to a round that took something out and waited for the grid to come back.
        # The backup and alert rounds take nothing out, so they have no such number - and reading it
        # unconditionally killed a live run 40 minutes in (KeyError, 2026-10-01), taking the night with
        # it. A missing measurement is never a reason to lose the report that holds the others.
        rec_s = sorted(e["recovery_s"] for e in exps
                       if e.get("result") == "recovered" and e.get("recovery_s") is not None)
        echo_ok = [x for x in self.echoes if x["result"] == "delivered"]
        echo_lat = sorted(x["latency_s"] for x in echo_ok if x["latency_s"] is not None)
        relink = sorted(e["relink_s"] for e in exps if e.get("relink_s") is not None and any(
            (n in self.by_name and self.by_name[n].is_ap) for n in e["victims"]))

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
               # "run of reached": a round that was skipped or could not start used to vanish from this
               # row, so a run that was skipping one round after another looked as if it had stalled.
               f"| Experiments | {len(exps)} run of {self.rounds_reached} reached "
               f"({max(0, self.rounds_reached - len(exps))} skipped or not started), "
               f"recovered {sum(e.get('result') == 'recovered' for e in exps)}, "
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
                     else f"Alerts and announcements were part of the traffic, in the audible hours "
                          f"{self.audible_hours[0]:02d}:00-{self.audible_hours[1]:02d}:00 only (D87); "
                          f"rounds that fell outside them were skipped and say so."), ""]
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
        out += self.backup_section()
        out += self.voice_section()
        out += self.lora_report()
        out += ["", "## Memory", "",
                "Free heap now and the lowest seen during the run: bytes on APs (status line), KB on handhelds "
                "('Online' line). A lowest value that keeps falling across long uptimes points at a leak; compare "
                "raw logs hour by hour. MAIN is the one to watch: it carries a GPS and a LoRa module.", ""]
        out += [f"- {b.name}: free {b.free_heap if b.free_heap is not None else 'no sample'}, "
                f"lowest {b.lowest_heap if b.lowest_heap is not None else 'no sample'}" for b in self.boards]
        out += ["", "Raw serial logs: raw-NN.log per hour. Every event: events.jsonl.", ""]
        (self.out / "summary.md").write_text("\n".join(out), encoding="utf-8")

    def voice_section(self):
        """Push to talk: what was sent, and what each listener actually played.

        Frames rather than words. The harness cannot hear, so it counts: the sender reports the frames it
        sent under a talk id and every listener reports how many of them it played under the same id. A
        talk that reached nobody, or reached somebody who was not addressed, is a finding above; this is
        the arithmetic behind it.
        """
        rounds = [e for e in self.experiments if e["kind"] == "voice"]
        if not rounds:
            return []
        out = ["", "## Voice (D61, D88)", ""]
        done = [e for e in rounds if e.get("frames") is not None]
        refused = [e for e in rounds if e.get("correct_refusal")]
        sent = sum(e["frames"] for e in done)
        if not done:
            out += [f"{len(rounds)} round(s), none of which produced a frame count."]
            if refused:
                out += [f"{len(refused)} were refused correctly, with no Wi-Fi to carry voice (D71)."]
            return out + [""]
        plays = [(name, got) for e in done for name, got in e.get("heard", {}).items()]
        shares = sorted(got["share"] for _, got in plays if got.get("share") is not None)
        silent = sum(len(e.get("silent", [])) for e in done)
        uninvited = sum(len(e.get("uninvited", [])) for e in done)
        expected = sum(len(e.get("expected", [])) for e in done)
        out += [f"{len(done)} talk(s), **{sent} frames sent**, heard by {len(plays)} of {expected} listener(s) "
                f"that should have heard them."]
        if shares:
            mid = statistics.median(shares)
            out += [f"Frames played, as a share of frames sent: median {mid * 100:.0f}%, "
                    f"worst {min(shares) * 100:.0f}%, best {max(shares) * 100:.0f}%."]
        if silent:
            out += [f"**{silent} listener(s) heard nothing at all** of a talk addressed to them."]
        out += [f"**{uninvited} listener(s) played a talk they were not addressed in.**" if uninvited
                else "No handheld played a talk it was not addressed in."]
        if refused:
            out += [f"{len(refused)} round(s) were refused with no Wi-Fi to carry them, which is D71 working."]
        out += ["", "| # | from | scope | asked | frames | dropped | heard by | silent |",
                "|---|---|---|---|---|---|---|---|"]
        for e in rounds:
            heard = e.get("heard", {})
            detail = ", ".join(f"{n} {g['played']}/{e.get('frames', 0)}" for n, g in heard.items()) or "nobody"
            out += [f"| {e['n']} | {e['victims'][0]} | {e.get('scope', '?')} | {e.get('talk_s', '?')} s | "
                    f"{e.get('frames', '-')} | {e.get('dropped', '-')} | {detail} | "
                    f"{', '.join(e.get('silent', [])) or '-'} |"]
        return out + [""]

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
            for e in [x for x in rounds if x["kind"] not in LORA_HH_KINDS]:
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
            hh_rounds = [x for x in rounds if x["kind"] in LORA_HH_KINDS]
            if hh_rounds:
                out += ["", "### Handheld rounds: a handheld on LoRa alone, sending and receiving", "",
                        "Its Wi-Fi was switched off for the window, so every message below had to cross on "
                        "its module. **From it** is the direction that was broken until 2026-09-30 and the "
                        "one worth reading first: receiving over LoRa worked long before sending did.", "",
                        "| # | Handheld | Window (s) | Sent from it | Delivered | Sent to it | Delivered | "
                        "Its LoRa frames out | Rejoined | Result |",
                        "|---|---|---|---|---|---|---|---|---|---|"]
                for e in hh_rounds:
                    pr = e.get("probes") or {}
                    fv, tv = pr.get("from_victim") or {}, pr.get("to_victim") or {}
                    out.append(
                        f"| {e['n']} | {', '.join(e['victims'])} | {e.get('window_s', 0)} | "
                        f"{fv.get('sent', '')} | {fv.get('delivered', '')} | {tv.get('sent', '')} | "
                        f"{tv.get('delivered', '')} | "
                        f"{e.get('frames_out') if e.get('frames_out') is not None else ''} | "
                        f"{'yes' if e.get('rejoined', True) else 'NO'} | {e.get('result') or 'running'} |")
                out.append("")
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
        out += self.ap_section()
        out += self.battery_section()
        out += ["", "### Each AP's LoRa module at the end", ""]
        for name, s in self.lora_last.items():
            def peer_text(i, pr):
                head = f"AP {i} {'up' if pr['up'] else 'DOWN'}"
                if pr.get("rssi") is None:
                    return head + " (heard recently; a handheld reports no per-peer figures)"
                return (head + f" RSSI {pr['rssi']} dBm SNR {pr['snr']} ({pr['age_ms']} ms ago)")

            peers = ", ".join(peer_text(i, pr) for i, pr in sorted(s.get("peers", {}).items()))
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
    # Voice, both ends of it. The sender's talk id is what ties the two together, and the listener's
    # count is what makes the pair a loss figure instead of a yes or no.
    ("voice_caps", "I (900) VOICE: [MSG] Push-to-talk: talk and listen", ("talk and listen",)),
    ("voice_caps", "I (900) VOICE: [MSG] Push-to-talk: listen only", ("listen only",)),
    ("voice_begin", "I (5000) VOICE: [MSG] Talking to everyone 0 (generated voice)", ("everyone", "0")),
    ("voice_begin", "I (5000) VOICE: [MSG] Talking to group 1", ("group", "1")),
    ("voice_begin", "I (5000) VOICE: [MSG] Talking to device 4", ("device", "4")),
    ("voice_sent", "I (9000) VOICE: [MSG] Talk ended: talk 3, 50 frame(s), 0 dropped before the AP, 5021 ms",
     ("3", "50", "0", "5021")),
    ("voice_heard", "I (9400) VOICE: [MSG] Heard talk 3 from 9: 48 frame(s), 2 missing", ("3", "9", "48", "2")),
    ("voice_heard", "I (9400) VOICE: [MSG] Heard talk 3 from 9: 11 frame(s), 0 missing, no last frame",
     ("3", "9", "11", "0")),
    ("voice_refused", "W (5000) MSG: [MSG] Talk not sent: short", ("short",)),
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
# A whole `lora` block as a handheld prints it, taken verbatim from Channi (COM9) on 2026-09-30, the run
# that proved a LoRa-only handheld can send as well as receive.
LORA_HH_BLOCK = """grid> lora
LoRa (D77): module fitted, address 101, network 14
  module:   RYLR998_REYAX_V1.2.4
  APs heard: 1 2 
  last in:  52 s ago, RSSI -21 dBm, SNR 10
  frames:   4 out, 3 in
  parts:    4 out, 9 in, 0 dropped, 0 retries
  refused:  2 by type (not carried over LoRa), 0 could not be opened
  queue:    0 dropped; reassembly timeouts 0
  airtime:  2282 ms; module resets 0"""

LORA_HH_BLOCK_WANT = {"fitted": True, "off": False, "connector": True, "configured": True,
                      "address": 101, "network": 14, "version": "RYLR998_REYAX_V1.2.4",
                      "heard_age_ms": 52000, "rssi": -21, "snr": 10, "frames_out": 4, "frames_in": 3,
                      "frames_first": None, "parts_out": 4, "parts_in": 9, "parts_dropped": 0,
                      "retries": 0, "refused_type": 2, "seal_fail": 0, "queue_dropped": 0,
                      "reasm_timeouts": 0, "airtime_ms": 2282, "restarts": 0, "done": True}
LORA_HH_PEERS_WANT = {1: {"up": True, "rssi": None, "snr": None, "age_ms": None},
                      2: {"up": True, "rssi": None, "snr": None, "age_ms": None}}
LORA_HH_HEADERS = [
    ("LoRa (D77): module fitted but switched OFF, address 101, network 14",
     {"fitted": True, "off": True, "done": False}),
    ("LoRa (D77): module fitted, not configured yet, address 0, network 14",
     {"fitted": True, "off": False, "configured": False}),
    ("LoRa (D77): no module answering, address 101, network 14",
     {"fitted": False, "done": True}),
    ("LoRa (D77): this board has no connector for a module",
     {"fitted": False, "connector": False, "done": True}),
]

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
    # The same command on a handheld (D77): a different block, the same keys out of it.
    st, peers = {}, {}
    for line in LORA_HH_BLOCK.splitlines():
        parse_lora_line(line, st, peers)
    for k, want in LORA_HH_BLOCK_WANT.items():
        if st.get(k) != want:
            failures += 1
            print(f"FAIL handheld lora status {k}: {st.get(k)!r}, want {want!r}")
    if peers != LORA_HH_PEERS_WANT:
        failures += 1
        print(f"FAIL handheld lora peers: {peers}, want {LORA_HH_PEERS_WANT}")
    for line, want in LORA_HH_HEADERS:
        st = {}
        if not parse_lora_line(line, st):
            failures += 1
            print(f"FAIL handheld lora header not parsed: {line!r}")
            continue
        for k, v in want.items():
            if st.get(k) != v:
                failures += 1
                print(f"FAIL handheld lora header {k} in {line!r}: {st.get(k)!r}, want {v!r}")

    # Lines that are not part of a status block must fall through to the ordinary parsers.
    for line in ("I (30000) node: [GRID] links 2 handhelds 1 heap 81234 min 74400 time GPS",
                 "I (5000) MSG: [MSG] From 2 (Handheld 2): CX0130-00014",
                 "  AP    STATE  RSSI  SNR  AGE_MS"):
        if parse_lora_line(line, {}, {}):
            failures += 1
            print(f"FAIL lora parser swallowed {line!r}")
    return failures


def check_lora_schedule():
    """LoRa rounds: only when asked for, on the right kind of board, and the right number of them.

    The AP-side rounds are typed at an AP console, so their victims must be APs. `lora_handheld` is typed
    at a handheld and needs a second handheld to carry traffic to, so it must only ever appear when there
    are at least two - and it must never name an AP.
    """
    failures = 0
    for n_aps, n_hhs in ((3, 0), (3, 2), (2, 0), (1, 2), (0, 2), (0, 1)):
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
                    on_hh = e["kind"] in LORA_HH_KINDS
                    want_prefix = "HH" if on_hh else "AP"
                    if any(not v.startswith(want_prefix) for v in e["victims"]):
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: victims {e['victims']} are not all "
                              f"{want_prefix}")
                    if on_hh and n_hhs < 2:
                        failures += 1
                        print(f"FAIL lora_handheld seed {seed}: scheduled with {n_hhs} handheld(s); it "
                              "needs a second one to message")
                    want = 2 if e["kind"] in ("lora_only", "lora_blind") else 1
                    if len(set(e["victims"])) != want:
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: {e['victims']}, want {want} distinct board(s)")
                    if not on_hh and n_aps < 2 and e["kind"] in ("lora_only", "lora_blind"):
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: needs two APs, the grid has {n_aps}")
                    if e["kind"] == "lora_wedge" and e["outage_s"] != 0:
                        failures += 1
                        print(f"FAIL lora_wedge seed {seed}: a window of {e['outage_s']} s")
                    if e["kind"] != "lora_wedge" and not 90 <= e["outage_s"] <= 300:
                        failures += 1
                        print(f"FAIL {e['kind']} seed {seed}: window {e['outage_s']} s outside 90-300")
            if want_any and n_aps == 0 and n_hhs >= 2 and "lora_handheld" not in seen:
                failures += 1
                print(f"FAIL no AP on USB, {n_hhs} handhelds, share {share}: no handheld LoRa round was "
                      "scheduled, so a battery-powered grid would get no LoRa coverage at all")
            if want_any and n_aps == 0 and n_hhs < 2 and seen:
                failures += 1
                print(f"FAIL no AP on USB and {n_hhs} handheld: scheduled {sorted(seen)} with nothing "
                      "that can run")
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
            ("  3   LEADERS           not a member", ("3", "LEADERS", "not a member")),
            # as a handheld actually prints them, with the members listed after
            ("  2   all               member         1 2 3", ("2", "all", "member")),
            ("  3   testers           not a member   2 6 8", ("3", "testers", "not a member")),
            ("  4   chaos             member         ", ("4", "chaos", "member"))]
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
    ap.add_argument("--secrets", default=str(ROOT / "firmware" / "common" / "lg_secrets.h"),
                    help="this grid's lg_secrets.h, to open the APs' sealed status beacons (D68)")
    ap.add_argument("--admin-password", metavar="PASSWORD",
                    help="the grid's admin password, for faulting access points over BLE when they hold "
                         "no cable (D90); or set LG_ADMIN_PASSWORD. Without it they are watched only")
    ap.add_argument("--no-ble", action="store_true",
                    help="do not watch the APs over BLE. Only for a run where no AP is armed: without "
                         "it a message lost while an AP was restarting is reported as a finding")
    ap.add_argument("--hours", type=float, default=10)
    ap.add_argument("--until-aps-die", action="store_true",
                    help="run until every access point has gone silent long enough to call it dead, "
                         "rather than for --hours. --hours becomes an upper bound")
    ap.add_argument("--ap-dead-after", type=float, default=10.0, metavar="MIN",
                    help="minutes of silence from every access point before they count as dead "
                         "(default 10). A restart takes seconds, so this is about a flat battery")
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
    ap.add_argument("--extra-share", type=float, default=0.22,
                    help="fraction of experiments that take the backup radio away (no_net) or walk the "
                         "alert sequence announcement -> SOS -> all clear (alerts). no_net is what proves "
                         "Wi-Fi stands on its own, as it must on a handheld with no module")
    ap.add_argument("--time-recovery", type=float, default=120,
                    help="seconds after a blackout to wait for grid time to come back from a handheld (D53) "
                         "before the PC sets it")
    ap.add_argument("--recovery-timeout", type=float, default=180, help="seconds after release to reach steady state")
    ap.add_argument("--probe-interval", type=float, default=45, help="seconds between handheld messages")
    ap.add_argument("--allow-firmware-mismatch", action="store_true",
                    help="start even when a board is not verified as running the current build; every "
                         "problem is recorded as the run's first finding")
    ap.add_argument("--no-set-time", dest="set_time", action="store_false",
                    help="do not set grid time from the PC clock at the start (1:1 and group need it, D6)")
    ap.add_argument("--audible-hours", metavar="HH-HH",
                    help="when an alert that makes a noise may be raised (D87); default 14-21, and "
                         "'0-0' allows them at any hour. --no-alerts silences them whatever this says")
    ap.add_argument("--no-voice", action="store_true",
                    help="no push-to-talk rounds (the listeners play the voice out loud)")
    ap.add_argument("--no-alerts", action="store_true",
                    help="never send announcements or urgent broadcasts (they wake every handheld); for night runs")
    ap.add_argument("--quiet-handhelds", action="store_true",
                    help="set handheld volume off for the run and restore it at the end")
    args = ap.parse_args()
    if args.seed is None:
        # Random per run, because a night that replays the same schedule stops being a sample of
        # anything - and recorded, because a finding nobody can reproduce is half a finding.
        args.seed = secrets.randbelow(100000)
    if args.until_aps_die:
        # The schedule still has to be generated against *some* horizon. Make it longer than any battery
        # could plausibly last, so the run ends because the access points died and not because the plan
        # ran out of entries.
        args.hours = max(args.hours, 48.0)
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
