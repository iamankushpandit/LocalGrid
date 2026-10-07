#!/usr/bin/env python3
"""One button per fact: a self-running demo of the bench grid, made to be filmed.

Each button plays one claim about the grid from start to finish on the real boards, through their
USB cables: it checks the grid is ready, says what is about to happen, does it, shows it happening,
and ends with the numbers the boards themselves printed. Then it puts the grid back.

The page reads every caption and result aloud with the browser's own voice (on by default, the voice
chosen on the page), so a recording needs nobody speaking, and the demo waits for each spoken line to
finish before it acts.

  python tools/demo.py                 # serve the page at http://127.0.0.1:8770/, the grid heard over Bluetooth
  python tools/demo.py --bench         # start with every board on its cable instead
  python tools/demo.py --page-only     # the page with no boards, to rehearse the words

The presentation leads with the product as people would use it, with no cables: the laptop only listens
to the access points' Bluetooth beacons, and the presenter pulls a plug, raises an SOS or switches
everything off by hand. Halfway, every board is plugged into the laptop for the story of how it was
built and tested: each claim broken on cue through the cables and measured.

The page is local to this laptop and talks to nothing else.

How it works: the engine is the chaos harness (tools/chaos.py) with its fault injection, log parsing
and steady-state checks, driven by a fixed script instead of a random schedule. Faults go through the
cable and no other way (D103), and every radio switched off restores itself from the board's own clock
even if this tool dies. The handhelds send and receive every message themselves; the PC types console
commands and reads logs, and never joins the grid (D25).

Output (gitignored): chaos-runs/demo/<start time>/ with the raw log and events.jsonl of the session.
"""
import argparse
import hashlib
import http.server
import io
import json
import pathlib
import queue
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import chaos  # noqa: E402
from flash import acquire_lock, load_map  # noqa: E402

PAGE = ROOT / "tools" / "demo_page.html"
STORY = ROOT / "tools" / "demo_story.json"
FLASHER = ROOT / "site" / "flash" / "index.html"
WATCH_PORT = 8768        # tools/grid_watch.py's own default
VOICES = ROOT / "build" / "voices"   # Piper voice models (.onnx with its .onnx.json); build/ is not in git
NARRATOR = "en_US-amy-medium"         # the owner's choice of voice, 2026-10-07
MEDIA = ROOT / "build" / "demo-media"  # the owner's photos for the chapters; not in git (they show the bench)
MEDIA_TYPES = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png", ".webp": "image/webp",
               ".mp4": "video/mp4", ".webm": "video/webm"}
chaos.RUNS = ROOT / "chaos-runs" / "demo"

# Broadcast, group and alert text is logged in full by the receiving handheld; 1:1 text never is
# (AGENTS.md), so 1:1 messages are matched by (author, boot, seq) through chaos.RX instead.
FROM_TEXT = re.compile(r"\[MSG\] From (\d+) \(([^)]*)\)[^:]*: (.*)$")
NOT_SENT_TEXT = re.compile(r"\[MSG\] Not sent \(([^)]*)\): (.*)$")
PEOPLE_ROW = re.compile(r"^\s*(\d+)\s{2,}(\S.*?)\s{2,}(offline|online.*)$")
# A handheld's `groups` rows, members included (hh_console.c, cmd_groups).
GROUP_LINE = re.compile(r"^\s*(\d+)\s+(\S+)\s+(member|not a member)\s*([\d ]*?)\s*$")
AP_GROUPS = re.compile(r"\[GRID\] Groups version (\d+)")
# Updates (D101, D107, D111): what an AP carries, and what a handheld takes.
AP_CARRYING = re.compile(r"\[GRID\] Handheld update serial (\d+) taken from AP (\d+) in (\d+) s; carrying it")
AP_HANDING = re.compile(r"\[GRID\] Handing the handheld update, serial (\d+), to device (\d+)")
HH_OFFERED = re.compile(r"\[GRID\] The AP offers update serial (\d+)")
HH_TAKEN = re.compile(r"\[GRID\] Update serial (\d+) taken from the AP in (\d+) s")
HH_RESTARTING = re.compile(r"\[GRID\] Restarting into update serial (\d+)")
HH_UPDATE_NO = re.compile(r"\[GRID\] Update from the AP not (?:taken|installed): (.*)$")

LOG_PREFIX = re.compile(r"^\x1b\[[0-9;]*m|\x1b\[0m$")
DEVICE_ID = re.compile(r"(LG-[NH]-[A-Z0-9]{3}-)[A-Z2-7]{10}")   # IDs are shortened on screen
IDF_LINE = re.compile(r"^([IWED]) \((\d+)\) [^:]+: (.*)$")   # "I (1234) TAG: text" -> level and text
LOG_KEEP = 200            # lines kept per board, for a page opened mid-session
LOG_BATCH_S = 0.3         # lines go to the page in batches, not one message each

DEMO_GROUPS = ("Trail-Crew", "Ridge-Crew")   # the split demo renames one into the other

DELIVERY_S = 30          # a message over Wi-Fi that has not landed by then has not landed
LORA_DELIVERY_S = 75     # LoRa is seconds of airtime per frame, plus acknowledgement turns (D104)
REJOIN_S = 120           # handhelds of a dead AP find another one well inside this
RELINK_S = 150           # an AP back from a power cut is linked to the others well inside this
STEADY_S = 180
SPLIT_S = 300            # every radio the split demo turns off comes back by itself after this
BLACKOUT_S = 20          # how long every access point is without power
UPDATE_S = 1500          # a handheld update crossing the grid and being installed (minutes, not seconds)
# A beacon goes out every 500 ms (D68), but this laptop's scanner misses several seconds at a time: at 5 s
# a busy moment read as "Denali went silent" with Denali running (2026-10-07). This much is an AP that is off.
GONE_S = 15
HANDS_S = 180            # how long a hands-on demo waits for the presenter to do something


class Stopped(Exception):
    """The Stop button: the scenario ends where it is and the grid is put back."""


class Bus:
    """Every open page gets every update. Published from the engine thread, read by HTTP threads."""

    def __init__(self):
        self.subs = []
        self.lock = threading.Lock()
        self.last_state = None
        self.recent = []   # captions and results of the scenario running now, for a page opened mid-way

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def publish(self, msg):
        if msg["type"] == "state":
            self.last_state = msg
        elif msg["type"] == "scenario" and msg.get("phase") == "start":
            self.recent = [msg]
        elif msg["type"] in ("caption", "fact", "timer", "scenario", "flow"):
            self.recent = (self.recent + [msg])[-60:]
        with self.lock:
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass   # a page that stopped reading misses updates; it gets the state again on reconnect


class Grid(chaos.Chaos):
    """The chaos harness, scripted. One thread (the engine) owns it, as one task owns core state."""

    def __init__(self, args, names, bus):
        super().__init__(args, names)
        self.bus = bus
        self.cancel = threading.Event()
        self.requests = queue.Queue()
        self.busy = None              # the scenario running now
        self.ready = False
        self.people = {}              # device index -> the name the grid knows it by
        self.tracked = {}             # token -> a message this demo sent, until every receiver has it
        self.msg_n = 0
        self.state_key = None
        self.rejoins = {}             # handheld name -> seconds to its next registration, while watched
        self.watch_since = None
        self.narrate = False          # a page is reading captions aloud (its own switch)
        self.line_n = 0               # every caption and fact, numbered for the page to say when it has spoken it
        self.spoken = 0
        self.gtab = {}                # handheld name -> {group id: (name, members)} from its last `groups`
        self.updates = []             # (time, board name, what, detail) seen during the update demo
        self.installing = None        # the board the installer demo has handed to the browser
        self.untethered = False       # every cable let go: the grid is seen over Bluetooth only (D68)
        self.seen_online = set()      # handheld device numbers seen online over Bluetooth this session
        self.logs = {}                # board label -> {role, the last LOG_KEEP lines as (level, text)}
        self.log_out = []             # lines not yet sent to the page
        self.log_sent_at = 0.0
        self.ble_events_seen = 0.0    # the newest Bluetooth event already shown
        self.kept = {}                # port -> held open, lines released, while the cables are not in use
        self.kept_at = 0.0

    # ---- what the page sees ----

    def publish(self, type_, **fields):
        self.bus.publish({"type": type_, "t": round(time.monotonic() - self.t0, 1), **fields})

    def spoken_aloud(self, n, text):
        """With the page narrating, the demo waits for the voice to finish before it moves on, so what
        is said and what happens stay together on camera. The page says when each line ends; the
        estimate is only the longest wait, for a page that closed mid-sentence."""
        if self.narrate:
            self.wait(len(text.split()) / 2.2 + 4, until=lambda: self.spoken >= n)

    def caption(self, text, hold=0.0):
        self.say(text)
        self.line_n += 1
        self.publish("caption", text=text, n=self.line_n)
        self.spoken_aloud(self.line_n, text)
        if hold:
            self.wait(hold)

    def fact(self, text, ok=True):
        self.say(("FACT: " if ok else "NOT SHOWN: ") + text)
        self.line_n += 1
        self.publish("fact", text=text, ok=ok, n=self.line_n)
        self.spoken_aloud(self.line_n, text)

    def timer(self, label, state, seconds=None):
        self.publish("timer", label=label, state=state, seconds=seconds)

    def label(self, b):
        if b.is_ap:
            return b.ap_name or b.name
        return self.people.get(b.index) or b.name

    def beacon(self):
        """What the access points' Bluetooth beacons say right now (tools/grid_watch.py's own decoder)."""
        if self.eyes is None or self.eyes.state is None:
            return {"aps": [], "handhelds": [], "alert": None}
        try:
            self.eyes.state.tick()
            return self.eyes.state.snapshot()
        except Exception:
            return {"aps": [], "handhelds": [], "alert": None}

    def heard(self, a):
        return bool(a.get("heard")) and (a.get("silent_s") or 0) <= GONE_S

    def snapshot_untethered(self):
        snap = self.beacon()
        aps = []
        for a in snap["aps"]:
            h = a.get("health") or {}
            links = [n["index"] for n in h.get("links") or []]
            aps.append({"index": a["index"], "name": a["name"], "present": self.heard(a), "held": False,
                        "links": links if self.heard(a) else [], "lora_links": [], "bb_off": False,
                        "lora_off": False, "time": h.get("time_quality")})
        live = {a["index"] for a in aps if a["present"]}
        # Only handhelds that have been on the grid this session: the grid also remembers names of ones
        # that are not here at all, and showing those would put boards on the map that are not on the bench.
        self.seen_online |= {h["device"] for h in snap["handhelds"] if h["online"]}
        hhs = [{"index": h["device"], "name": h["name"], "present": True,
                "node": h["ap"] if h["online"] and h["ap"] in live else None, "last_node": h["ap"],
                "wifi_off": False, "battery": h["battery"]} for h in snap["handhelds"] if h["device"] in self.seen_online]
        return {"aps": aps, "handhelds": hhs, "busy": self.busy, "ready": self.ready, "untethered": True,
                "alert": snap.get("alert")}

    def snapshot(self):
        if self.untethered:
            return self.snapshot_untethered()
        aps = [{"index": a.index, "name": self.label(a), "present": self.present(a), "held": a.held,
                "links": sorted(a.links), "lora_links": sorted(a.lora_links), "bb_off": bool(a.bb_off),
                "lora_off": bool(a.lora_off), "time": (a.status or {}).get("time")} for a in self.aps]
        hhs = [{"index": h.index, "name": self.label(h), "present": self.present(h),
                "node": h.registered_node, "last_node": h.last_node, "wifi_off": bool(h.wifi_off),
                "lora": (self.lora_last.get(h.name) or {}).get("fitted")} for h in self.hhs]
        return {"aps": aps, "handhelds": hhs, "busy": self.busy, "ready": self.ready, "untethered": False}

    def background(self):
        super().background()
        self.flush_logs()
        if self.untethered:
            self.keep_ports()
        snap = self.snapshot()
        key = json.dumps(snap, sort_keys=True)
        if key != self.state_key:
            self.state_key = key
            self.publish("state", **snap)

    def event(self, what, /, **fields):
        rec = super().event(what, **fields)
        if what == "registered" and self.watch_since is not None:
            name = fields.get("board")
            if name in self.rejoins and self.rejoins[name] is None:
                self.rejoins[name] = round(time.monotonic() - self.watch_since, 1)
        if what in ("registered", "handheld_down", "link_up", "link_down", "boot", "bb_off", "bb_on",
                    "wifi_off", "wifi_on", "lora_link_up", "lora_link_down", "finding"):
            self.publish("log", event=what, **{k: v for k, v in fields.items() if isinstance(v, (str, int, float))})
        return rec

    # ---- the Stop button ----

    def wait(self, seconds, until=None):
        if self.busy and self.cancel.is_set():
            raise Stopped()
        got = super().wait(seconds, until=lambda: self.cancel.is_set() or bool(until and until()))
        if self.busy and self.cancel.is_set():
            raise Stopped()
        return got and not self.cancel.is_set()

    # ---- messages this demo sends, followed to every screen they should reach ----

    def log_line(self, key, role, line, who=None):
        """One console line for the page's log view. Keys, passphrases and 1:1 text are never logged by
        the firmware (AGENTS.md), hardware addresses are masked by the reader (D21), and positions are
        never logged (D65), so what the boards print can be shown as it is."""
        who = who or key
        line = DEVICE_ID.sub(r"…", LOG_PREFIX.sub("", line)).rstrip()
        if not line.strip():
            return
        m = IDF_LINE.match(line)
        level, text = (m.group(1), m.group(3)) if m else ("", line)
        entry = self.logs.setdefault(key, {"role": role, "name": who, "lines": []})
        entry.update(role=role, name=who)
        entry["lines"].append((level, text))
        del entry["lines"][:-LOG_KEEP]
        self.log_out.append([key, role, level, text, who])

    def flush_logs(self):
        now = time.monotonic()
        if self.untethered and self.eyes is not None and self.eyes.state is not None:
            try:
                events = list(self.eyes.state.events)
            except Exception:
                events = []
            for e in events:
                if e.get("t", 0) > self.ble_events_seen:
                    self.ble_events_seen = e["t"]
                    self.log_line("Bluetooth", "ble", f"{time.strftime('%H:%M:%S', time.localtime(e['t']))}  {e['text']}")
        if self.log_out and now - self.log_sent_at >= LOG_BATCH_S:
            self.publish("logs", lines=self.log_out[-400:])
            self.log_out = []
            self.log_sent_at = now

    def handle(self, b, line):
        super().handle(b, line)
        # Keyed by port: a board's name is learned as it talks (port, then its ID, then the name the grid
        # knows it by), and keying by name split one board's output across three panels.
        self.log_line(b.port, "ap" if b.is_ap else "hh" if b.role else "", line, self.label(b))
        now = time.monotonic()
        if not b.is_ap and (m := PEOPLE_ROW.search(line)):
            self.people[int(m.group(1))] = m.group(2).strip()
        if not b.is_ap and (m := GROUP_LINE.search(line)):
            members = ",".join(m.group(4).split())
            self.gtab.setdefault(b.name, {})[int(m.group(1))] = (m.group(2), members)
        if b.is_ap and (m := AP_GROUPS.search(line)):
            b.groups_version = int(m.group(1))
        for rx, what in ((AP_CARRYING, "carrying"), (AP_HANDING, "handing"), (HH_OFFERED, "offered"),
                         (HH_TAKEN, "taken"), (HH_RESTARTING, "restarting"), (HH_UPDATE_NO, "refused")):
            if m := rx.search(line):
                self.updates.append((now, b.name, what, m.groups()))
                self.publish("log", event="update_" + what, board=self.label(b), detail=" ".join(m.groups()))
        if m := chaos.RX["sent_direct"].search(line):
            for p in self.tracked.values():
                if p["direct"] and p["sender"] is b and p["target"] == int(m.group(1)) and p["msg_id"] is None:
                    p["msg_id"] = (b.index, int(m.group(2)), int(m.group(3)))
                    early = p.pop("early", None)
                    if early:
                        self.landed(p, early[0], early[1])
                    break
        elif m := chaos.RX["received_direct"].search(line):
            key = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            p = next((p for p in self.tracked.values() if p["msg_id"] == key), None)
            if p:
                self.landed(p, b, now)
            else:
                # The receiver's line can reach the PC before the sender's "Sent" line: the two ports
                # deliver independently. Kept against the oldest unmatched 1:1 to this receiver.
                for p in self.tracked.values():
                    if p["direct"] and p["msg_id"] is None and b.name in p["got"] and p["got"][b.name] is None:
                        p["early"] = (b, now)
                        break
        elif m := (chaos.RX["not_sent_direct"].search(line) or chaos.RX["ap_refused_direct"].search(line)):
            for p in self.tracked.values():
                if p["direct"] and p["sender"] is b and p["refused"] is None:
                    p["refused"] = m.group(1)
                    break
        elif m := FROM_TEXT.search(line):
            self.people.setdefault(int(m.group(1)), m.group(2))
            for p in self.tracked.values():
                if not p["direct"] and p["token"] in m.group(3) and b.name in p["got"]:
                    self.landed(p, b, now)
        elif m := NOT_SENT_TEXT.search(line):
            for p in self.tracked.values():
                if not p["direct"] and p["token"] in m.group(2) and p["sender"] is b:
                    p["refused"] = m.group(1)

    def landed(self, p, b, now):
        if p["got"].get(b.name, 0) is None:
            p["got"][b.name] = round(now - p["mono"], 2)
            self.publish("flow", phase="landed", token=p["token"], to=self.label(b),
                         seconds=p["got"][b.name], path=p["path"])

    def send(self, sender, target, text, receivers, path="wifi"):
        """target: a handheld (1:1), or "urgent", "safe", "all". Returns the tracker."""
        self.msg_n += 1
        token = f"[{self.msg_n}]"
        direct = not isinstance(target, str)
        p = {"token": token, "direct": direct, "sender": sender, "path": path,
             "target": target.index if direct else None, "msg_id": None, "refused": None,
             "got": {r.name: None for r in receivers}, "mono": time.monotonic()}
        self.tracked[token] = p
        self.publish("flow", phase="sent", token=token, sender=self.label(sender), path=path,
                     to=[self.label(r) for r in receivers])
        self.write(sender, f"send {target.index if direct else target} {text} {token}")
        return p

    def arrived(self, p):
        return p["refused"] is not None or all(v is not None for v in p["got"].values())

    def settle_message(self, p, seconds):
        self.wait(seconds, until=lambda: self.arrived(p))
        self.tracked.pop(p["token"], None)
        return {name: secs for name, secs in p["got"].items()}

    # ---- getting ready, and putting the grid back ----

    def start_eyes(self):
        if self.eyes is not None:
            return True
        try:
            names = {d["node_index"]: d["node_name"] for d in load_map().get("devices", [])
                     if d.get("role") == "N" and isinstance(d.get("node_index"), int)}
        except (OSError, ValueError):
            names = {}
        names.update({b.index: b.ap_name for b in self.aps if b.ap_name})
        self.eyes = chaos.BleEyes(self.args.secrets, names)
        if not self.eyes.start():
            self.caption(f"This laptop cannot listen over Bluetooth: {self.eyes.error}.")
            self.eyes = None
            return False
        return True

    def keep_ports(self):
        """Hold every bench bridge port open with both lines released, reading nothing.

        Found in the demo of 2026-10-07: with the access points' USB cables still in this laptop for power,
        unplugging one access point left the others on the same hub held in reset - Everest and Fuji both
        reported 1 s of uptime the moment a port was opened, minutes after they had gone quiet, and every
        handheld said "No LocalGrid AP in range". A CP210x or CH34x that the system has just set up holds
        the board's enable line until a program opens it with the lines released. Holding the ports open
        released keeps a re-plugged board running. Nothing is read or written: the laptop still only
        listens over Bluetooth. Native USB boards (Espressif's own) have no reset line to hold and are left
        alone."""
        now = time.monotonic()
        if now - self.kept_at < 2.0:
            return
        self.kept_at = now
        from serial.tools import list_ports
        present = {pt.device: pt.vid for pt in list_ports.comports() if pt.vid in chaos.SEPARATE_BRIDGES}
        for port in list(self.kept):
            if port not in present:
                try:
                    self.kept.pop(port).close()
                except (self.serial.SerialException, OSError):
                    pass
        for port in present:
            if port in self.kept:
                continue
            s = self.serial.Serial()
            s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0, False, False
            try:
                s.open()
            except (self.serial.SerialException, OSError):
                continue
            self.kept[port] = s
            self.publish("log", event="port_kept", board=port)

    def let_go_of_ports(self):
        for port, s in list(self.kept.items()):
            try:
                s.dtr = s.rts = False
                s.close()
            except (self.serial.SerialException, OSError):
                pass
        self.kept.clear()

    def untether(self):
        """Let go of every cable. The boards keep running; the laptop only listens from here on."""
        self.caption("This laptop lets go of every cable. From here on it only listens to the grid over "
                     "Bluetooth, the way anyone watching a real deployment would. It cannot send, change or "
                     "break anything.")
        if not self.start_eyes():
            return
        for b in self.boards:
            self.release_port(b)
        self.untethered = True
        self.background()
        self.wait(25, until=lambda: any(self.heard(a) for a in self.beacon()["aps"]))
        snap = self.beacon()
        heard = [a["name"] for a in snap["aps"] if self.heard(a)]
        self.fact(f"Heard over Bluetooth: {', '.join(heard) or 'no access point yet'}")
        self.caption("Unplug every cable now. Give each access point a battery or a charger. "
                     "The handhelds run on their own batteries.")

    def warm_up_untethered(self):
        self.caption("No cables: this laptop listens to the grid over Bluetooth only.")
        self.untethered = True
        self.args.bench = False
        if not self.start_eyes():
            return
        # Every access point the bench map knows, or whatever has been heard after 30 s.
        try:
            want = sum(1 for d in load_map().get("devices", []) if d.get("role") == "N" and d.get("port"))
        except (OSError, ValueError):
            want = 1
        self.wait(30, until=lambda: sum(self.heard(a) for a in self.beacon()["aps"]) >= want)
        self.wait(8)                  # and a moment for the handhelds' frames to say who is where
        self.ready = True
        snap = self.beacon()
        self.caption(f"Ready: {sum(self.heard(a) for a in snap['aps'])} access points heard over Bluetooth.")

    def warm_up(self):
        if not self.args.bench:
            return self.warm_up_untethered()
        self.warm_up_tethered()

    def tether(self):
        """Every board plugged into this laptop: the bench, where each claim is broken on cue and measured."""
        self.caption("Every board is now plugged into this laptop. Each one is restarted, so each test "
                     "starts from a known point.")
        try:
            acquire_lock()    # the same lock as flash.py and chaos.py: one tool on the boards at a time
        except SystemExit as e:
            self.caption(f"The boards are in use by another tool. {e}")
            return
        self.ready = False
        try:
            self.warm_up_tethered()
        except SystemExit as e:   # no board answered, or one runs a build that is not the current one
            self.ready = True
            self.caption(f"Not ready: {str(e).strip()}")

    def warm_up_tethered(self):
        self.caption("Finding the boards on this laptop's cables.")
        self.let_go_of_ports()
        self.boards, self.aps, self.hhs, self.by_name = [], [], [], {}
        self.start_boards()
        self.untethered = False
        self.args.bench = True
        self.background()
        self.caption("The access points are finding each other by themselves.")
        t = time.monotonic()
        if self.wait(90, until=self.backbone_whole):
            self.fact(f"The access points formed the grid by themselves in {time.monotonic() - t:.0f} s")
        if self.args.set_time:
            self.set_time()
        self.wait(STEADY_S, until=lambda: not self.unsteady())
        for h in self.hhs:
            self.write(h, "people")
        self.wait(5)
        if self.args.lora_check:
            self.lora_query(self.aps + self.hhs)
        self.ready = True
        why = self.unsteady()
        self.caption("Ready." if not why else "Ready, though not everything is settled: " + "; ".join(why))

    def start_boards(self):
        self.discover()
        self.event("start", boards=[{"name": b.name, "id": b.id, "role": b.role, "index": b.index}
                                    for b in self.boards])

    def restore(self):
        """Every fault this demo can leave behind, undone. Each hook also restores itself on the board."""
        for b in self.boards:
            if b.held:
                self.release_hold(b)
            if b.bb_off:
                self.write(b, "bb on")
            if b.lora_off:
                self.write(b, "lora on")
            if b.wifi_off:
                self.write(b, "wifi on")

    def reset_grid(self):
        self.caption("Putting the grid back: every radio on, every board powered.")
        self.restore()
        t = time.monotonic()
        steady = self.wait(STEADY_S, until=lambda: not self.unsteady())
        why = self.unsteady()
        self.caption(f"The grid is steady ({time.monotonic() - t:.0f} s)." if steady else
                     "Not steady yet: " + "; ".join(why))

    def precheck(self, need_handhelds=2):
        if not self.wait(60, until=lambda: not self.unsteady()):
            self.caption("The grid is not ready for this: " + "; ".join(self.unsteady())
                         + ". Press Reset grid, then try again.")
            return False
        on = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        if len(on) < need_handhelds:
            self.caption(f"This needs {need_handhelds} handhelds on the grid; {len(on)} are.")
            return False
        return True

    # ---- the scenarios ----

    def sc_ap_out(self):
        """An access point loses power. Its handhelds find another one, and messages keep arriving."""
        if not self.precheck():
            return
        live = [a for a in self.aps if self.present(a)]
        if len(live) < 2:
            self.caption("This needs two access points: one to lose and one to move to.")
            return
        loads = {a.index: [h for h in self.hhs if self.present(h) and h.registered_node == a.index] for a in live}
        candidates = [a for a in live if a.holdable and loads[a.index]]
        if not candidates:
            self.caption("No access point with handhelds on it can have its power cut from here.")
            return
        v = max(candidates, key=lambda a: len(loads[a.index]))
        riders = loads[v.index]
        others = [h for h in self.hhs if self.present(h) and h not in riders and h.registered_node is not None]
        names = ", ".join(self.label(h) for h in riders)
        self.caption(f"{self.label(v)} is carrying {names}. Watch what happens when it loses power.", hold=5)

        self.rejoins = {h.name: None for h in riders}
        self.watch_since = time.monotonic()
        self.hold(v)
        self.publish("log", event="power_cut", board=self.label(v))
        self.confirm(v, "power cut (board still printing)", lambda: time.monotonic() - v.last_line_at > 2.0,
                     lambda: self.hold(v), 4)
        self.caption(f"{self.label(v)} has no power. Its handhelds are looking for another access point.")
        self.timer("Finding another access point", "start")
        self.wait(REJOIN_S, until=lambda: all(h.registered_node not in (None, v.index) for h in riders))
        done = {n: s for n, s in self.rejoins.items() if s is not None}
        self.watch_since = None
        self.timer("Finding another access point", "stop", max(done.values()) if done else None)
        for h in riders:
            secs = self.rejoins.get(h.name)
            if secs is not None and h.registered_node is not None:
                ap = next((a for a in self.aps if a.index == h.registered_node), None)
                self.fact(f"{self.label(h)} moved to {self.label(ap) if ap else 'another access point'} "
                          f"in {secs:.1f} s, by itself")
            else:
                self.fact(f"{self.label(h)} did not find another access point within {REJOIN_S} s", ok=False)

        sender = next((h for h in riders if h.registered_node is not None), None)
        receiver = (others or [h for h in riders if h is not sender and h.registered_node is not None] or [None])[0]
        if sender and receiver:
            self.caption(f"{self.label(sender)} writes to {self.label(receiver)}.", hold=2)
            self.timer("Message on its way", "start")
            p = self.send(sender, receiver, f"{self.label(v)} lost power. This found another way.", [receiver])
            got = self.settle_message(p, DELIVERY_S)
            secs = got.get(receiver.name)
            self.timer("Message on its way", "stop", secs)
            if secs is not None:
                self.fact(f"A message from {self.label(sender)} reached {self.label(receiver)} in {secs:.1f} s "
                          f"with {self.label(v)} still off")
            else:
                self.fact(f"The message to {self.label(receiver)} did not arrive"
                          + (f" ({p['refused']})" if p["refused"] else ""), ok=False)

        self.caption(f"Power comes back to {self.label(v)}.", hold=2)
        released = time.monotonic()
        self.release_hold(v)
        self.confirm(v, "power back (no boot banner)", lambda: v.boot_at > released, lambda: self.pulse(v), 12)
        self.timer(f"{self.label(v)} rejoining", "start")
        linked = self.wait(RELINK_S, until=lambda: v.links >= {a.index for a in self.aps if self.present(a)} - {v.index})
        secs = round(time.monotonic() - released, 1)
        self.timer(f"{self.label(v)} rejoining", "stop", secs if linked else None)
        if linked:
            self.fact(f"{self.label(v)} started, found the others and rejoined the grid in {secs:.0f} s. "
                      f"Nobody set anything up again.")
        else:
            self.fact(f"{self.label(v)} had not linked to every access point {RELINK_S} s after power came back",
                      ok=False)

    def sc_lora_bridge(self):
        """The Wi-Fi link between access points is cut everywhere. The next message crosses by LoRa."""
        if not self.precheck():
            return
        live = [a for a in self.aps if self.present(a)]
        if len(live) < 2:
            self.caption("This needs two access points.")
            return
        on = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        pair = next(((s, r) for s in on for r in on if s.registered_node != r.registered_node), None)
        before = self.lora_query(live)
        if not any(s.get("fitted") for s in before.values()):
            self.caption("No access point has a LoRa module answering, so there is no second path to show.")
            return
        self.caption("Access points normally pass messages to each other over Wi-Fi. "
                     "We switch that link off on every one of them.", hold=4)
        for a in live:
            self.write(a, "bb off 300")
        self.wait(8, until=lambda: all(a.bb_off for a in live))
        self.publish("log", event="backbone_cut", board="every access point")
        self.caption("The Wi-Fi link between the access points is gone. The only way between them is LoRa.",
                     hold=3)
        if pair:
            s, r = pair
            sa = next(a for a in self.aps if a.index == s.registered_node)
            ra = next(a for a in self.aps if a.index == r.registered_node)
            self.caption(f"{self.label(s)}, on {self.label(sa)}, writes to {self.label(r)}, on {self.label(ra)}.",
                         hold=2)
            self.timer("Crossing by LoRa", "start")
            p = self.send(s, r, "Sent with the Wi-Fi between access points switched off.", [r], path="lora")
            got = self.settle_message(p, LORA_DELIVERY_S)
            secs = got.get(r.name)
            self.timer("Crossing by LoRa", "stop", secs)
            if secs is not None:
                self.fact(f"{self.label(r)} got it {secs:.1f} s later, carried from {self.label(sa)} to "
                          f"{self.label(ra)} over LoRa")
            else:
                self.fact(f"The message to {self.label(r)} did not arrive"
                          + (f" ({p['refused']})" if p["refused"] else ""), ok=False)
        else:
            self.caption("Every handheld is on the same access point, so a message between them would not "
                         "need to cross. The access points send each other a test message instead.", hold=3)
            self.timer("Crossing by LoRa", "start")
            e = self.echo_probe()
            if e:
                self.publish("flow", phase="sent", token=e["token"], sender=self.label(self.by_name[e["sender"]]),
                             path="lora", to=[self.label(self.by_name[n]) for n in e["receivers"]])
                self.wait(chaos.ECHO_DEADLINE_S + 5, until=lambda: e["token"] not in self.echo_pending)
            got = (e or {}).get("got") or {}
            self.timer("Crossing by LoRa", "stop", max(got.values()) if got else None)
            for name, secs in sorted(got.items()):
                b = self.by_name.get(name)
                self.fact(f"{self.label(b) if b else name} heard it {secs:.1f} s later, over LoRa")
            if not got:
                self.fact("The test message did not cross", ok=False)
        after = self.lora_query(live)
        delta = self.lora_delta(before, after)
        sent = {n: r.get("frames_out") for n, r in delta.items() if r.get("frames_out")}
        if sent:
            self.fact("LoRa frames put on the air: " + ", ".join(
                f"{self.label(self.by_name[n])} {k}" for n, k in sent.items()))
        self.caption("Now the Wi-Fi link comes back.", hold=1)
        t = time.monotonic()
        for a in live:
            self.write(a, "bb on")
        self.timer("Wi-Fi link back", "start")
        back = self.wait(RELINK_S, until=self.backbone_whole)
        self.timer("Wi-Fi link back", "stop", round(time.monotonic() - t, 1) if back else None)
        if back:
            self.fact(f"The access points were linked by Wi-Fi again {time.monotonic() - t:.0f} s later, "
                      f"by themselves")

    def sc_emergency(self):
        """An SOS from one handheld takes over every other screen, over Wi-Fi and LoRa at once."""
        if not self.precheck():
            return
        on = [h for h in self.hhs if self.present(h) and h.registered_node is not None]
        sender = on[0]
        receivers = [h for h in self.hhs if self.present(h) and h is not sender]
        live = [a for a in self.aps if self.present(a)]
        before = self.lora_query(live)
        self.caption(f"{self.label(sender)} raises an SOS. It goes out over Wi-Fi and LoRa at the same time, "
                     f"and it takes over every screen, even a locked one.", hold=5)
        self.timer("SOS reaching every screen", "start")
        p = self.send(sender, "urgent", f"SOS from {self.label(sender)}: need help at the north gate.", receivers)
        got = self.settle_message(p, DELIVERY_S)
        reached = {n: s for n, s in got.items() if s is not None}
        self.timer("SOS reaching every screen", "stop", max(reached.values()) if reached else None)
        if p["refused"]:
            self.fact(f"The SOS was not sent: {p['refused']}", ok=False)
        for name, secs in sorted(got.items(), key=lambda kv: (kv[1] is None, kv[1] or 0)):
            b = self.by_name[name]
            self.fact(f"{self.label(b)}: on screen in {secs:.1f} s" if secs is not None
                      else f"{self.label(b)}: did not show it within {DELIVERY_S} s", ok=secs is not None)
        after = self.lora_query(live)
        delta = self.lora_delta(before, after)
        lora = sum(r.get("frames_out") or 0 for r in delta.values())
        if lora:
            self.fact(f"The access points also put it on LoRa ({lora} frame{'s' if lora != 1 else ''}), "
                      f"so it does not depend on one radio")
        self.caption("Every screen shows it, and who has seen it.", hold=8)
        self.caption(f"{self.label(sender)} is safe. One message stands the alert down everywhere.", hold=2)
        self.timer("All clear reaching every screen", "start")
        p = self.send(sender, "safe", f"{self.label(sender)} is safe. Stand down.", receivers)
        got = self.settle_message(p, DELIVERY_S)
        reached = {n: s for n, s in got.items() if s is not None}
        self.timer("All clear reaching every screen", "stop", max(reached.values()) if reached else None)
        if len(reached) == len(receivers):
            self.fact(f"The all clear reached all {len(receivers)} handhelds in {max(reached.values()):.1f} s")
        else:
            missed = [self.label(self.by_name[n]) for n, s in got.items() if s is None]
            self.fact("The all clear did not reach " + ", ".join(missed), ok=False)


    # ---- helpers for the scenarios below ----

    def ap_at(self, index):
        return next((a for a in self.aps if a.index == index), None)

    def on_grid(self):
        return [h for h in self.hhs if self.present(h) and h.registered_node is not None]

    def chip_of(self, b):
        try:
            return (load_map().get("boards", {}).get(b.board) or {}).get("target")
        except (OSError, ValueError):
            return None

    def read_groups(self, boards, seconds=6):
        for b in boards:
            self.gtab[b.name] = {}
            self.write(b, "groups")
        self.wait(seconds, until=lambda: all(self.gtab.get(b.name) for b in boards))
        return {b.name: dict(self.gtab.get(b.name) or {}) for b in boards}

    def marks_until(self, seconds, checks):
        """Times, from now, at which each named condition first held. Returns the dict when all have,
        or after `seconds` with whatever had."""
        t0 = time.monotonic()
        marks = {}

        def all_done():
            for name, fn in checks.items():
                if name not in marks and fn():
                    marks[name] = round(time.monotonic() - t0, 1)
                    self.publish("log", event="mark", board=name, detail=f"{marks[name]} s")
            return len(marks) == len(checks)
        self.wait(seconds, until=all_done)
        return marks

    def reopen_quiet(self, b):
        """Open a port again without restarting the board: both lines low before the port opens, as
        tools/flash.py's open_quiet does. chaos.Chaos.open asserts them and so resets CP210x boards."""
        s = self.serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout, s.dtr, s.rts = b.port, 115200, 0.1, 0.5, False, False
        s.open()
        b.ser, b.lost_since = s, None
        b.stop_reader.clear()
        th = threading.Thread(target=self.reader, args=(b, s), daemon=True)
        th.start()
        b.reader_thread = th
        self.readers.append(th)

    def run_tool(self, argv, timeout, show=None):
        """Another bench tool, its output shown on the page line by line as it runs. Returns its exit code."""
        proc = subprocess.Popen([sys.executable, *argv], cwd=str(ROOT), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(x) for x in proc.stdout] and None, daemon=True).start()
        end = time.monotonic() + timeout
        try:
            while proc.poll() is None or not lines.empty():
                if time.monotonic() > end:
                    proc.kill()
                    return None
                try:
                    line = lines.get(timeout=0.2).rstrip()
                except queue.Empty:
                    self.wait(0.05)
                    continue
                self.write_raw(f"[{argv[0]}] {line}")
                if line and (show is None or show(line)):
                    self.publish("log", event="tool", board=pathlib.Path(argv[0]).stem, detail=line[:160])
            return proc.returncode
        except Stopped:
            proc.kill()
            raise

    # ---- more scenarios ----

    def sc_hh_lora(self):
        """A handheld loses Wi-Fi. Messages still reach it, and leave it, over LoRa (D77, D80, D109)."""
        if not self.precheck():
            return
        fitted = [h for h in self.on_grid() if (self.lora_last.get(h.name) or {}).get("fitted")]
        if not fitted:
            self.lora_query(self.hhs)
            fitted = [h for h in self.on_grid() if (self.lora_last.get(h.name) or {}).get("fitted")]
        if not fitted:
            self.caption("No handheld on the grid has a LoRa module answering, so there is nothing to show.")
            return
        v = fitted[0]
        peer = next((h for h in self.on_grid() if h is not v), None)
        if peer is None:
            self.caption("This needs a second handheld on the grid to talk to.")
            return
        self.caption(f"{self.label(v)} has a LoRa radio as well as Wi-Fi. We switch its Wi-Fi off, "
                     f"as if it had walked out of range of every access point.", hold=2)
        self.write(v, "wifi off 300")
        self.wait(12, until=lambda: v.wifi_off)
        self.caption(f"{self.label(v)} has no Wi-Fi now. An access point that hears it over LoRa takes it on, "
                     "and the grid knows it is on LoRa alone.", hold=chaos.LORA_HH_SETTLE_S / 3)
        self.wait(chaos.LORA_HH_SETTLE_S * 2 / 3)
        self.caption(f"{self.label(peer)} writes to {self.label(v)}.")
        self.timer(f"To {self.label(v)} over LoRa", "start")
        p = self.send(peer, v, "This reached you over LoRa, with your Wi-Fi off.", [v], path="lora")
        got = self.settle_message(p, LORA_DELIVERY_S)
        secs = got.get(v.name)
        self.timer(f"To {self.label(v)} over LoRa", "stop", secs)
        self.fact(f"{self.label(v)} got it over LoRa in {secs:.1f} s, and its access point heard it say so"
                  if secs is not None else f"The message to {self.label(v)} did not arrive"
                  + (f" ({p['refused']})" if p["refused"] else ""), ok=secs is not None)
        self.caption(f"And {self.label(v)} answers.")
        self.timer(f"From {self.label(v)} over LoRa", "start")
        p = self.send(v, peer, "Answering over LoRa. Still no Wi-Fi here.", [peer], path="lora")
        got = self.settle_message(p, LORA_DELIVERY_S)
        secs = got.get(peer.name)
        self.timer(f"From {self.label(v)} over LoRa", "stop", secs)
        self.fact(f"Its answer reached {self.label(peer)} in {secs:.1f} s" if secs is not None
                  else "Its answer did not arrive" + (f" ({p['refused']})" if p["refused"] else ""),
                  ok=secs is not None)
        self.caption(f"Wi-Fi comes back on {self.label(v)}.")
        t = time.monotonic()
        self.write(v, "wifi on")
        back = self.wait(chaos.HH_REJOIN_S, until=lambda: v.registered_node is not None)
        if back:
            self.fact(f"{self.label(v)} was back on Wi-Fi with an access point {time.monotonic() - t:.0f} s later")

    def sc_voice(self):
        """Push to talk: a voice from one handheld out of every other one's speaker (D61, D88)."""
        if not self.precheck():
            return
        on = self.on_grid()
        sender = next((h for h in on if h.can_talk), on[0])
        listeners = [h for h in self.hhs if self.present(h) and h is not sender and h.can_hear is not False]
        if not listeners:
            self.caption("No other handheld can play sound.")
            return
        sent_before = set(sender.voice_sent)
        heard_before = {h.name: set(h.voice_heard) for h in listeners}
        self.caption(f"Push to talk. {self.label(sender)} talks to everyone. A generated voice stands in for "
                     "a person, so it says the same thing every time.", hold=1)
        self.timer("Talking", "start")
        self.write(sender, "demo all 6")

        def new_talk():
            ids = set(sender.voice_sent) - sent_before
            return next(iter(ids), None)

        self.wait(30, until=lambda: new_talk() is not None and all(
            new_talk() in h.voice_heard for h in listeners))
        tid = new_talk()
        self.timer("Talking", "stop", (sender.voice_sent[tid][2] / 1000) if tid in sender.voice_sent else None)
        if tid is None:
            why = sender.voice_refused
            self.fact(f"{self.label(sender)} did not send the talk" + (f": {why}" if why else ""), ok=False)
            return
        frames = sender.voice_sent[tid][0]
        for h in listeners:
            got = h.voice_heard.get(tid) if tid not in heard_before[h.name] else None
            if got:
                played = got[1]
                self.fact(f"{self.label(h)} played {played} of {frames} voice frames "
                          f"({100 * played // max(frames, 1)} %)")
            else:
                self.fact(f"{self.label(h)} did not play it", ok=False)

    def sc_ap_restart(self):
        """An access point crashes and restarts; what it knew comes back by itself (D48, D53, D60)."""
        if not self.precheck(need_handhelds=1):
            return
        live = [a for a in self.aps if self.present(a)]
        if len(live) < 2:
            self.caption("This needs two access points: one to restart, one that remembers.")
            return
        v = max(live, key=lambda a: sum(h.registered_node == a.index for h in self.hhs))
        others = [a for a in live if a is not v]
        settings = max((a.settings_version or 0) for a in others) or None
        groups = max((getattr(a, "groups_version", None) or 0) for a in others) or None
        self.caption(f"{self.label(v)} crashes and restarts. There is no master copy anywhere: whatever it "
                     f"knew has to come back from its own flash and from the others.", hold=1)
        t = time.monotonic()
        self.pulse(v)
        self.on_down(v)
        v.settings_version = None
        v.groups_version = None
        self.publish("log", event="restart", board=self.label(v))
        self.confirm(v, "restart (no boot banner)", lambda: v.boot_at > t, lambda: (self.pulse(v), self.on_down(v)), 12)
        self.timer(f"{self.label(v)} coming back", "start")
        want = {a.index for a in others}
        checks = {
            "linked": lambda: v.links >= want,
            "time": lambda: bool(v.status) and v.status["time"] != "UNSET",
            "handhelds": lambda: all(h.registered_node is not None for h in self.hhs if self.present(h)),
        }
        if settings:
            checks["settings"] = lambda: (v.settings_version or 0) >= settings
        if groups:
            checks["groups"] = lambda: (getattr(v, "groups_version", None) or 0) >= groups
        marks = self.marks_until(RELINK_S, checks)
        self.timer(f"{self.label(v)} coming back", "stop", max(marks.values()) if len(marks) == len(checks) else None)
        said = {
            "linked": ("Linked to every other access point again in {s} s", "Not linked to every access point"),
            "time": ("Had the grid's time again {s} s after restarting, with nobody setting it",
                     "Did not have the grid's time"),
            "settings": (f"Had settings version {settings}, the same as the others, {{s}} s after restarting",
                         f"Did not reach settings version {settings}"),
            "groups": (f"Had groups version {groups}, the same as the others, {{s}} s after restarting",
                       f"Did not reach groups version {groups}"),
            "handhelds": ("Every handheld was on an access point again {s} s after the crash",
                          "Not every handheld was back on an access point"),
        }
        for name in checks:
            if name in marks:
                self.fact(said[name][0].format(s=f"{marks[name]:.0f}"))
            else:
                self.fact(f"{said[name][1]} within {RELINK_S} s", ok=False)

    def sc_split(self):
        """The grid splits in two. Each half carries on; when they meet, the newest change wins (D45, D48)."""
        if not self.precheck():
            return
        live = [a for a in self.aps if self.present(a)]
        if len(live) < 2:
            self.caption("This needs two access points to split.")
            return
        on = self.on_grid()
        pick = next(((a, h, f) for a in live for h in on for f in on
                     if h.registered_node == a.index and f.registered_node != a.index), None)
        if pick is None:
            self.caption("This needs handhelds on two different access points.")
            return
        v, near, far = pick
        tables = self.read_groups([near, far])
        gid, gname, members = None, None, None
        for i, (name, mem) in tables[near.name].items():
            if name in DEMO_GROUPS:
                gid, gname, members = i, name, mem
        if gid is None:
            members = ",".join(str(h.index) for h in self.hhs if self.present(h))
            self.caption(f"First, a group everyone is in, made from {self.label(near)}: {DEMO_GROUPS[0]}.")
            self.write(near, f"group new {DEMO_GROUPS[0]} {members}")
            self.wait(20, until=lambda: any(n == DEMO_GROUPS[0] for n, _ in
                                            self.read_groups([far], 3)[far.name].values()))
            tables = self.read_groups([near, far])
            for i, (name, mem) in tables[near.name].items():
                if name == DEMO_GROUPS[0]:
                    gid, gname, members = i, name, mem
            if gid is None:
                self.fact("The group could not be made", ok=False)
                return
        new = DEMO_GROUPS[1] if gname == DEMO_GROUPS[0] else DEMO_GROUPS[0]
        self.caption(f"Now we cut {self.label(v)} off from the others: no Wi-Fi link and no LoRa. "
                     f"{self.label(near)} is on its side, {self.label(far)} on the other.", hold=1)
        self.write(v, f"bb off {SPLIT_S}")
        self.write(v, f"lora off {SPLIT_S}")
        others = {a.index for a in live if a is not v}
        self.wait(45, until=lambda: v.bb_off and v.lora_off and not (v.links & others)
                  and all(v.index not in a.links for a in live if a is not v))
        self.publish("log", event="split", board=self.label(v))
        self.fact(f"The grid is in two halves: {self.label(v)} alone, and "
                  + ", ".join(self.label(a) for a in live if a is not v))
        self.caption(f"On its side, {self.label(near)} renames the group {gname} to {new}.")
        self.write(near, f"group set {gid} {new} {members}")
        self.wait(20, until=lambda: any(n == new for n, _ in self.read_groups([near], 3)[near.name].values()))
        far_now = self.read_groups([far])[far.name]
        if any(n == gname for n, _ in far_now.values()):
            self.fact(f"{self.label(far)}, on the other half, still calls it {gname}: the halves really are apart")
        self.caption("Now the two halves meet again.", hold=1)
        t = time.monotonic()
        self.write(v, "bb on")
        self.write(v, "lora on")
        self.timer("Halves agreeing", "start")
        agreed = self.wait(120, until=lambda: any(n == new for n, _ in self.read_groups([far], 3)[far.name].values()))
        secs = round(time.monotonic() - t, 1)
        self.timer("Halves agreeing", "stop", secs if agreed else None)
        if agreed:
            self.fact(f"{self.label(far)} calls it {new} {secs:.0f} s after the halves met. The newer change won, "
                      "and nobody merged anything by hand")
        else:
            self.fact(f"{self.label(far)} still did not have the new name 120 s after the halves met", ok=False)
        self.wait(RELINK_S, until=self.backbone_whole)

    def sc_blackout(self):
        """Every access point loses power at once. The grid comes back on its own, time included (D53)."""
        if not self.precheck(need_handhelds=1):
            return
        live = [a for a in self.aps if self.present(a)]
        if not live or not all(a.holdable for a in live):
            self.caption("Every access point needs a cable that can cut its power for this one.")
            return
        self.caption("The worst case: every access point loses power at the same moment. "
                     "The handhelds stay on, on their own batteries.", hold=1)
        self.time_source = None
        for a in live:
            self.hold(a)
        self.publish("log", event="power_cut", board="every access point")
        self.timer("No access point running", "start")
        self.wait(BLACKOUT_S)
        self.timer("No access point running", "stop", BLACKOUT_S)
        self.caption("Power comes back to all of them.")
        released = time.monotonic()
        for a in live:
            self.release_hold(a)
        for a in live:
            self.confirm(a, "power back (no boot banner)", lambda a=a: a.boot_at > released, lambda a=a: self.pulse(a), 12)
        self.timer("Grid coming back", "start")
        marks = self.marks_until(STEADY_S, {
            "linked": self.backbone_whole,
            "time": self.grid_has_time,
            "handhelds": lambda: all(h.registered_node is not None for h in self.hhs if self.present(h)),
        })
        self.timer("Grid coming back", "stop", max(marks.values()) if len(marks) == 3 else None)
        if "linked" in marks:
            self.fact(f"The access points found each other and formed the grid again in {marks['linked']:.0f} s")
        else:
            self.fact("The access points had not all linked again", ok=False)
        if "time" in marks:
            src = self.time_source or "another access point"
            self.fact(f"The grid had its time back {marks['time']:.0f} s after power returned, from {src}. "
                      "Nobody typed it in")
        else:
            self.fact("The grid did not get its time back by itself", ok=False)
            self.caption("This laptop sets the time so the other demos can run.")
            self.set_time()
        if "handhelds" in marks:
            self.fact(f"Every handheld was back on an access point {marks['handhelds']:.0f} s after power returned")

    def sc_tour(self):
        """The handhelds' own screens, opened from their consoles, for the camera."""
        hs = [h for h in self.hhs if self.present(h)]
        if not hs:
            self.caption("No handheld is on a cable.")
            return
        steps = [
            ("status", "Every handheld has a status screen: which access point it is on, the signal, its battery, "
                       "the grid's time, and which firmware it runs."),
            ("messages", "Messages: everyone, groups and one person, in one list. A mark on each message says how "
                         "far it got: sent, delivered, read."),
            ("groups", "Groups are made on the handhelds while the grid runs. Nothing is built in."),
            ("settings", "Settings: the handheld's own name, sound, screen lock and screen saver, kept through "
                         "restarts."),
            ("home", "And home."),
        ]
        for screen, words in steps:
            for h in hs:
                self.write(h, f"screen {screen}")
            self.caption(words, hold=4)

    def sc_update(self):
        """A signed update given to one access point reaches the others and the handhelds by itself
        (D97, D101, D107, D111)."""
        path = pathlib.Path(self.args.update)
        blob = path.read_bytes()
        if blob[:4] != b"LGFW" or blob[5:6] != b"H":
            self.caption("The update file given to this demo is not a handheld update.")
            return
        chip = {1: "esp32", 2: "esp32s3", 3: "esp32c6"}.get(blob[6])
        serial = int.from_bytes(blob[8:12], "little")
        live = [a for a in self.aps if self.present(a)]
        if not live:
            self.caption("No access point on a cable to give it to.")
            return
        v = live[0]
        mine = [h for h in self.hhs if self.present(h) and self.chip_of(h) == chip]
        self.caption(f"A signed update for the {chip.upper().replace('ESP32S3', 'ESP32-S3')} handhelds is given "
                     f"to {self.label(v)} alone, over its cable. Nothing else is told about it.", hold=1)
        self.updates = []
        self.timer(f"Giving it to {self.label(v)}", "start")
        t_give = time.monotonic()
        name = next((d["name"] for d in load_map().get("devices", []) if d.get("id") == v.id), v.name)
        self.release_port(v)
        code = self.run_tool(["tools/lgfw.py", "send", str(path), name], 900,
                             show=lambda line: "%" in line or "LGFWU" in line or "lgfw" in line)
        self.reopen_quiet(v)
        given = round(time.monotonic() - t_give, 1)
        self.timer(f"Giving it to {self.label(v)}", "stop", given if code == 0 else None)
        if code != 0:
            self.fact(f"{self.label(v)} did not take the update (exit {code})", ok=False)
            return
        self.fact(f"{self.label(v)} checked the signature and carries the update, serial {serial}")
        self.caption("From here nobody touches anything. The access points offer it to each other, and to "
                     "every handheld it fits. This takes minutes; the handhelds wait until they are idle.")
        self.timer("Spreading", "start")
        t = time.monotonic()
        seen = set()

        def progress():
            for at, board, what, detail in self.updates:
                key = (board, what)
                if key in seen:
                    continue
                seen.add(key)
                b = self.by_name.get(board)
                who = self.label(b) if b else board
                secs = at - t
                if what == "carrying":
                    self.fact(f"{who} took it from another access point and carries it ({secs:.0f} s)")
                elif what == "taken":
                    self.fact(f"{who} downloaded it over Wi-Fi and checked it ({secs:.0f} s)")
                elif what == "restarting":
                    self.fact(f"{who} restarted into it ({secs:.0f} s)")
                elif what == "refused":
                    self.fact(f"{who} refused it: {detail[0]}", ok=False)
            done = {board for _, board, what, _ in self.updates if what in ("restarting", "refused")}
            return mine and all(h.name in done for h in mine)
        self.wait(UPDATE_S, until=progress)
        progress()
        self.timer("Spreading", "stop", round(time.monotonic() - t, 1))
        self.caption(f"Finally, one command on {self.label(v)} withdraws the update from the whole grid.")
        self.write(v, "spread drop")
        self.wait(10)
        self.fact("Withdrawn: no access point offers it any more")

    def sc_install(self):
        """A board gets LocalGrid from the installer web page and joins the grid (the web flasher)."""
        name = self.args.install_board
        b = next((x for x in self.boards if x.name == name), None)
        if b is None:
            self.caption(f"{name} is not one of the boards on this laptop's cables.")
            return
        self.caption(f"{self.label(b)} is installed from the installer web page, the way anybody with a new "
                     "board would do it: press Install, choose the port, wait, then give it this grid's file.",
                     hold=1)
        self.installing = b
        self.release_port(b)
        b.registered_node = None
        t = time.monotonic()
        self.timer("Installing from the browser", "start")
        argv = ["tools/test_installer_page.py", name] + (["--release", self.args.release] if self.args.release else [])
        try:
            code = self.run_tool(argv, 1200)
        finally:
            self.installing = None
            self.reopen_quiet(b)
        self.timer("Installing from the browser", "stop", round(time.monotonic() - t, 1) if code == 0 else None)
        if code != 0:
            self.fact(f"The installer did not finish (exit {code})", ok=False)
            return
        self.fact(f"Installed from the browser and set up for this grid in {time.monotonic() - t:.0f} s")
        self.write(b, "people")
        back = self.wait(120, until=lambda: b.registered_node is not None)
        if back:
            ap = self.ap_at(b.registered_node)
            self.fact(f"{self.label(b)} joined {self.label(ap) if ap else 'an access point'} by itself, "
                      f"{time.monotonic() - t:.0f} s after the install started")


    # ---- hands-on, with no cables: the presenter acts, the beacon shows what the grid did ----

    def where_now(self):
        snap = self.beacon()
        live = {a["index"]: a for a in snap["aps"] if self.heard(a)}
        return snap, live, {h["device"]: h["ap"] for h in snap["handhelds"] if h["online"] and h["ap"] in live}

    def sc_hands_ap(self):
        """The presenter pulls the power on an access point; the beacon shows the grid recover."""
        snap, live, where = self.where_now()
        if len(live) < 2:
            self.caption("This needs two access points heard over Bluetooth.")
            return
        names = {h["device"]: h["name"] for h in snap["handhelds"]}
        before = set(live)
        self.caption("Pull the power on any access point you like. Take your time.")
        self.timer("Waiting for you", "start")
        gone = []

        def one_gone():
            _, now_live, _ = self.where_now()
            gone[:] = sorted(before - set(now_live))
            return bool(gone)
        if not self.wait(HANDS_S, until=one_gone):
            self.timer("Waiting for you", "stop", None)
            self.caption("No access point went quiet. Try again whenever you are ready.")
            return
        dead = gone[0]
        dead_name = live[dead]["name"]
        riders = [d for d, ap in where.items() if ap == dead]
        self.timer("Waiting for you", "stop", None)
        self.fact(f"{dead_name} went silent; the laptop saw it stop")
        if riders:
            self.caption(f"{dead_name} was carrying {', '.join(names.get(d, str(d)) for d in riders)}. "
                         "Watch them move.")
        self.timer("Handhelds finding another access point", "start")
        t = time.monotonic()
        moved = {}

        def all_moved():
            _, _, w = self.where_now()
            for d in riders:
                if d not in moved and w.get(d) not in (None, dead):
                    moved[d] = (round(time.monotonic() - t, 1), w[d])
            return len(moved) == len(riders)
        self.wait(REJOIN_S, until=all_moved)
        self.timer("Handhelds finding another access point", "stop", max((s for s, _ in moved.values()), default=None))
        _, live2, _ = self.where_now()
        for d in riders:
            if d in moved:
                secs, ap = moved[d]
                self.fact(f"{names.get(d, d)} is on {live2.get(ap, {}).get('name', ap)}, "
                          f"{secs:.0f} s after {dead_name} went quiet, with no cable anywhere")
            else:
                self.fact(f"{names.get(d, d)} was not seen on another access point", ok=False)
        self.caption(f"Now plug {dead_name} back in.")
        self.timer(f"{dead_name} rejoining", "start")
        t = time.monotonic()

        def back():
            _, l3, _ = self.where_now()
            a = l3.get(dead)
            others = set(l3) - {dead}
            links = {n["index"] for n in ((a or {}).get("health") or {}).get("links") or []}
            return a is not None and links >= others
        ok = self.wait(HANDS_S + RELINK_S, until=back)
        secs = round(time.monotonic() - t, 1)
        self.timer(f"{dead_name} rejoining", "stop", secs if ok else None)
        if ok:
            self.fact(f"{dead_name} is back and linked to every other access point")

    def sc_hands_sos(self):
        """The presenter raises an SOS on a handheld's own screen; the grid carries it, and Read and Safe."""
        snap = self.beacon()
        last = (snap.get("alert") or {}).get("id")
        self.caption("Pick up any handheld and raise an SOS from its screen.")
        self.timer("Waiting for an SOS", "start")
        t = time.monotonic()
        got = {}

        def new_alert():
            a = self.beacon().get("alert")
            if a and a.get("id") != last and a.get("active") and not a.get("all_clear"):
                got.update(a)
                return True
            return False
        if not self.wait(HANDS_S, until=new_alert):
            self.timer("Waiting for an SOS", "stop", None)
            self.caption("No SOS was seen. Try again whenever you are ready.")
            return
        self.timer("Waiting for an SOS", "stop", round(time.monotonic() - t, 1))
        self.fact(f"SOS from {got['name']}, near {got['near']}, carried across the grid and seen from here")
        self.caption("Every other handheld is showing it now. Press Read on a few of them.")
        reads = got.get("reads") or 0
        # Wait for Reads to come in, then for the count to stop moving, so the number said is the last one.
        seen = {"n": reads, "at": time.monotonic()}

        def settled():
            n = (self.beacon().get("alert") or {}).get("reads") or 0
            if n != seen["n"]:
                seen.update(n=n, at=time.monotonic())
            return n > reads and time.monotonic() - seen["at"] > 10
        self.wait(90, until=settled)
        a = self.beacon().get("alert") or {}
        if (a.get("reads") or 0) > reads:
            self.fact(f"Read by {a['reads']} handheld{'s' if a['reads'] != 1 else ''}, and the sender can see who")
        self.caption(f"Now, on {got['name']}, mark yourself safe.")
        self.wait(HANDS_S, until=lambda: bool((self.beacon().get("alert") or {}).get("all_clear")))
        if (self.beacon().get("alert") or {}).get("all_clear"):
            self.fact(f"{got['name']} is safe: the alert is stood down across the grid")

    def sc_hands_blackout(self):
        """The presenter switches every access point off, then on; the grid re-forms with no cable."""
        _, live, _ = self.where_now()
        if not live:
            self.caption("No access point is heard over Bluetooth.")
            return
        names = {i: a["name"] for i, a in live.items()}
        self.caption("Switch every access point off.")
        self.timer("Waiting for you", "start")
        if not self.wait(HANDS_S, until=lambda: not self.where_now()[1]):
            self.timer("Waiting for you", "stop", None)
            self.caption("Some access points are still on. Try again whenever you are ready.")
            return
        self.timer("Waiting for you", "stop", None)
        self.fact("Every access point is off. The handhelds are on their own batteries, and keep the grid's time")
        self.caption("Now switch them all back on.")
        self.timer("Grid coming back", "start")
        t = None
        marks = {}

        def check():
            nonlocal t
            snap, l2, w = self.where_now()
            if t is None and l2:
                t = time.monotonic()
            if t is None:
                return False
            now = round(time.monotonic() - t, 1)
            if "all" not in marks and set(names) <= set(l2):
                marks["all"] = now
            if "linked" not in marks and set(names) <= set(l2) and all(
                    {n["index"] for n in ((l2[i].get("health") or {}).get("links") or [])} >= set(names) - {i}
                    for i in names):
                marks["linked"] = now
            if "time" not in marks and any(((a.get("health") or {}).get("time_quality") or "unset") not in
                                           ("unset", "?") for a in l2.values()):
                marks["time"] = now
            return len(marks) == 3
        self.wait(HANDS_S + STEADY_S, until=check)
        self.timer("Grid coming back", "stop", max(marks.values()) if len(marks) == 3 else None)
        if "linked" in marks:
            self.fact(f"The access points found each other and formed the grid again in {marks['linked']:.0f} s")
        if "time" in marks:
            self.fact(f"The grid had its time back {marks['time']:.0f} s after the first one started, "
                      "carried back by the handhelds. Nobody typed it in")
        elif t is not None:
            self.fact("The grid did not get its time back by itself", ok=False)

    SCENARIOS = {
        "tour": ("The handheld screens", "Status, messages, groups and settings on every handheld.", "sc_tour"),
        "emergency": ("SOS on every screen", "Over Wi-Fi and LoRa at once; one message stands it down.",
                      "sc_emergency"),
        "voice": ("Push to talk", "A voice from one handheld out of every other speaker.", "sc_voice"),
        "ap_out": ("Unplug an access point", "Its handhelds move by themselves; messages keep arriving.",
                   "sc_ap_out"),
        "ap_restart": ("Crash an access point", "It comes back with everything it knew.", "sc_ap_restart"),
        "lora_bridge": ("Cut the Wi-Fi between access points", "The next message crosses by LoRa.",
                        "sc_lora_bridge"),
        "hh_lora": ("A handheld out of Wi-Fi range", "Messages still reach it, and leave it, by LoRa.",
                    "sc_hh_lora"),
        "split": ("Split the grid in two", "Each half carries on; when they meet, the newest change wins.",
                  "sc_split"),
        "blackout": ("Every access point off at once", "The grid comes back on its own, time included.",
                     "sc_blackout"),
        "update": ("An update that spreads itself", "Give it to one access point; the grid does the rest.",
                   "sc_update"),
        "install": ("Install from the browser", "A board gets LocalGrid from a web page and joins.", "sc_install"),
        "hands_ap": ("Pull a plug, no cables", "You unplug an access point; the grid recovers on its own.",
                     "sc_hands_ap"),
        "hands_sos": ("Raise an SOS by hand", "From a handheld's own screen; Read, then Safe.", "sc_hands_sos"),
        "hands_blackout": ("Everything off, by hand", "Switch every access point off and on again.",
                           "sc_hands_blackout"),
    }
    HANDS_ON = ("hands_ap", "hands_sos", "hands_blackout")

    def unavailable(self, key):
        """Why a scenario cannot run on this session, or None."""
        if key in self.HANDS_ON and not self.untethered:
            return "runs with the cables unplugged"
        if key not in self.HANDS_ON and self.untethered:
            return "runs once the boards are plugged in"
        if key == "update" and not self.args.update:
            return "start the demo with --update <a signed handheld .lgfw file>"
        if key == "install" and not self.args.install_board:
            return "start the demo with --install-board <a bench handheld>"
        return None

    # ---- the engine loop ----

    def run_scenario(self, key):
        title, _, method = self.SCENARIOS[key]
        self.busy = key
        self.cancel.clear()
        self.publish("scenario", phase="start", key=key, title=title)
        self.background()
        outcome = "done"
        try:
            getattr(self, method)()
        except Stopped:
            outcome = "stopped"
            self.cancel.clear()      # or the caption's own wait is stopped too, and the server with it
            self.caption("Stopped.")
        finally:
            self.cancel.clear()
            self.busy = None
            self.watch_since = None
            self.tracked.clear()
            self.restore()
        self.publish("scenario", phase="end", key=key, title=title, outcome=outcome)
        self.event("demo_scenario", key=key, outcome=outcome)

    def serve(self):
        self.warm_up()
        while True:
            try:
                kind, arg = self.requests.get_nowait()
            except queue.Empty:
                self.cancel.clear()     # Stop pressed with nothing running means nothing
                self.wait(0.3)
                continue
            if kind == "run":
                self.run_scenario(arg)
            elif kind == "tether":
                self.busy = "tether"
                try:
                    self.tether()
                finally:
                    self.busy = None
                    self.background()
            elif kind == "untether":
                self.busy = "untether"
                try:
                    self.untether()
                finally:
                    self.busy = None
                    self.background()
            elif kind == "reset":
                self.busy = "reset"
                try:
                    self.reset_grid()
                finally:
                    self.busy = None
                    self.background()


class Narrator:
    """A natural voice made on this laptop with Piper (an open-source neural speech synthesiser), if it
    is installed and a voice model is in build/voices/. Nothing leaves the laptop. Without it the page
    falls back to the browser's own voices."""

    def __init__(self, model=None):
        self.paths = {}               # voice name -> model file; each is loaded the first time it speaks
        self.loaded = {}
        self.error = None
        self.lock = threading.Lock()
        self.cache = {}
        models = [pathlib.Path(model)] if model else sorted(VOICES.glob("*.onnx"))
        if not models:
            self.error = f"no voice model in {VOICES}"
            return
        import importlib.util
        if importlib.util.find_spec("piper") is None:   # not installed: the browser's voices still work
            self.error = "Piper is not installed (pip install piper-tts)"
            return
        self.paths = {m.stem: m for m in models}
        # Loaded now, before anything starts Bluetooth: once the Bluetooth listener is running in this
        # process, onnxruntime's DLL fails to initialise ("A dynamic link library (DLL) initialization
        # routine failed", 2026-10-07), and every line was silently skipped.
        try:
            from piper import PiperVoice
            self.loaded[self.default] = PiperVoice.load(str(self.paths[self.default]))
        except Exception as e:
            self.error = f"{type(e).__name__}: {e}"
            self.paths = {}

    @property
    def names(self):
        return list(self.paths)

    @property
    def default(self):
        return NARRATOR if NARRATOR in self.paths else next(iter(self.paths), None)

    def wav(self, text, name=None):
        import wave
        from piper import PiperVoice
        name = name if name in self.paths else self.default
        key = hashlib.sha256((name + "\0" + text).encode("utf-8")).hexdigest()
        with self.lock:
            if key in self.cache:
                return self.cache[key]
            if name not in self.loaded:
                self.loaded[name] = PiperVoice.load(str(self.paths[name]))
            voice = self.loaded[name]
            buf = io.BytesIO()
            with wave.open(buf, "wb") as w:
                if hasattr(voice, "synthesize_wav"):
                    voice.synthesize_wav(text, w)
                else:
                    voice.synthesize(text, w)
            data = buf.getvalue()
            if len(self.cache) > 400:          # bounded: the story and a session's captions fit easily
                self.cache.clear()
            self.cache[key] = data
            return data


def scenario_list(grid):
    return [{"key": k, "title": t, "subtitle": s,
             "unavailable": (grid.unavailable(k) if grid is not None else "page only: no boards are open")}
            for k, (t, s, _) in Grid.SCENARIOS.items()]


def hardware(grid):
    """What is on this bench, as the boards themselves said (D-numbers in the story say why)."""
    try:
        bench = load_map()
    except (OSError, ValueError):
        bench = {}
    kinds = bench.get("boards", {})
    rows = []
    for b in (grid.aps + grid.hhs) if grid is not None else []:
        desc = (kinds.get(b.board) or {}).get("description", b.board or "")
        lora = (grid.lora_last.get(b.name) or {}).get("fitted")
        rows.append({"role": "access point" if b.is_ap else "handheld", "name": grid.label(b),
                     "board": desc.split(" (")[0], "detail": desc[len(desc.split(" (")[0]):].strip(" ()"),
                     "chip": (kinds.get(b.board) or {}).get("target", "").upper().replace("ESP32S3", "ESP32-S3")
                     .replace("ESP32C6", "ESP32-C6"),
                     "lora": lora, "mic": b.can_talk if not b.is_ap else None,
                     "speaker": b.can_hear if not b.is_ap else None})
    return {"boards": rows}


def make_handler(bus, grid, watch_url=None, narrator=None):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            if path == "/":
                return self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            if path == "/api/scenarios":
                return self.reply(200, {"scenarios": scenario_list(grid), "boards": grid is not None,
                                        "watch": watch_url,
                                        "tts": narrator.names if narrator else [],
                                        "tts_default": narrator.default if narrator else None})
            if path == "/api/tts":
                if not narrator or not narrator.paths:
                    return self.reply(404, {"error": "no local voice"})
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                text = (q.get("text") or [""])[0][:600]
                return self.reply(200, narrator.wav(text, (q.get("voice") or [None])[0]), "audio/wav")
            if path == "/api/logs":
                return self.reply(200, {"logs": grid.logs if grid is not None else {}})
            if path == "/api/story":
                return self.reply(200, STORY.read_bytes())
            if path == "/api/hardware":
                return self.reply(200, hardware(grid))
            if path == "/api/media":
                have = sorted(f.name for f in MEDIA.glob("*") if f.suffix.lower() in MEDIA_TYPES) if MEDIA.is_dir() else []
                return self.reply(200, {"files": have})
            if path.startswith("/media/"):
                name = urllib.parse.unquote(path[len("/media/"):])
                f = MEDIA / name
                if "/" in name or "\\" in name or f.suffix.lower() not in MEDIA_TYPES or not f.is_file():
                    return self.reply(404, {"error": "not found"})
                return self.reply(200, f.read_bytes(), MEDIA_TYPES[f.suffix.lower()])
            if path in ("/assets/network-diagram.svg", "/assets/brand/localgrid-icon.svg"):
                # The site's own diagram of the network and the product's icon: the presentation uses
                # these rather than drawing its own (owner, 2026-10-07).
                return self.reply(200, (ROOT / path.lstrip("/")).read_bytes(), "image/svg+xml")
            if path in ("/flash", "/flash/"):
                return self.reply(200, FLASHER.read_bytes(), "text/html; charset=utf-8")
            if path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                q = bus.subscribe()
                try:
                    for msg in ([bus.last_state] if bus.last_state else []) + list(bus.recent):
                        self.wfile.write(f"data: {json.dumps(msg)}\n\n".encode())
                    self.wfile.flush()
                    while True:
                        try:
                            msg = q.get(timeout=15)
                            self.wfile.write(f"data: {json.dumps(msg)}\n\n".encode())
                        except queue.Empty:
                            self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                except (ConnectionError, OSError):
                    pass
                finally:
                    bus.unsubscribe(q)
                return None
            return self.reply(404, {"error": "not found"})

        def do_POST(self):
            path = urllib.parse.urlparse(self.path).path
            qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if grid is None:
                if path in ("/api/narrate", "/api/spoken"):
                    return self.reply(200, {"ok": True})     # nothing waits for the voice with no boards
                return self.reply(409, {"error": "page only: no boards are open"})
            if path == "/api/run":
                key = (qs.get("s") or [""])[0]
                if key not in Grid.SCENARIOS:
                    return self.reply(400, {"error": "no such scenario"})
                if grid.unavailable(key):
                    return self.reply(409, {"error": grid.unavailable(key)})
                if grid.busy or not grid.ready or not grid.requests.empty():
                    return self.reply(409, {"error": "busy" if grid.ready else "warming up"})
                grid.requests.put(("run", key))
                return self.reply(202, {"ok": True})
            if path == "/api/tether":
                if grid.busy or not grid.untethered or not grid.requests.empty():
                    return self.reply(409, {"error": "busy" if grid.busy else "already on the cables"})
                grid.requests.put(("tether", None))
                return self.reply(202, {"ok": True})
            if path == "/api/untether":
                if grid.busy or grid.untethered or not grid.requests.empty():
                    return self.reply(409, {"error": "busy" if grid.busy else "already untethered"})
                grid.requests.put(("untether", None))
                return self.reply(202, {"ok": True})
            if path == "/api/reset":
                if grid.busy or not grid.requests.empty():
                    return self.reply(409, {"error": "busy"})
                grid.requests.put(("reset", None))
                return self.reply(202, {"ok": True})
            if path == "/api/narrate":
                grid.narrate = (qs.get("on") or ["0"])[0] == "1"
                return self.reply(200, {"narrate": grid.narrate})
            if path == "/api/spoken":
                try:
                    grid.spoken = max(grid.spoken, int((qs.get("n") or ["0"])[0]))
                except ValueError:
                    return self.reply(400, {"error": "n"})
                return self.reply(200, {"ok": True})
            if path == "/api/stop":
                grid.cancel.set()
                return self.reply(202, {"ok": True})
            return self.reply(404, {"error": "not found"})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8770, help="the page's port on 127.0.0.1")
    ap.add_argument("--page-only", action="store_true", help="serve the page with no boards opened")
    ap.add_argument("--devices", nargs="+", help="bench names or device IDs to use (default: every board that answers)")
    ap.add_argument("--allow-firmware-mismatch", action="store_true",
                    help="start even if a board is not running the current build (it is logged)")
    ap.add_argument("--no-set-time", dest="set_time", action="store_false",
                    help="do not set grid time from this laptop if the grid has none of its own")
    ap.add_argument("--no-lora-check", dest="lora_check", action="store_false",
                    help="skip reading which boards have a LoRa module at the start")
    ap.add_argument("--update", metavar="FILE",
                    help="a signed handheld update (.lgfw) for the 'update that spreads itself' demo")
    ap.add_argument("--install-board", metavar="NAME",
                    help="a bench handheld the 'install from the browser' demo may reinstall (its settings are "
                         "cleared, its identity kept; it then runs the published image, not the bench build)")
    ap.add_argument("--release", metavar="FILE", help="a release public key (hex) the installer gives that board")
    ap.add_argument("--bench", action="store_true",
                    help="start with every board on its cable (the default starts with none: the product "
                         "first, watched over Bluetooth, and the cables come in halfway)")
    ap.add_argument("--voice-model", metavar="FILE",
                    help=f"a Piper voice model (.onnx); default: the first in {VOICES.relative_to(ROOT)}")
    ap.add_argument("--no-watch", dest="watch", action="store_false",
                    help="do not start tools/grid_watch.py (the laptop's Bluetooth view of the grid)")
    args = ap.parse_args()

    bus = Bus()
    grid = None
    if not args.page_only:
        try:
            names = {d["id"]: d["name"] for d in load_map().get("devices", []) if d.get("id")}
        except (OSError, ValueError):
            names = {}
        if args.bench:
            acquire_lock()    # the same lock as flash.py and chaos.py: one tool on the boards at a time
        chaos.keep_awake()
        # The harness's own settings, fixed: no schedule, no random traffic, no BLE, no alert quiet hours.
        hargs = argparse.Namespace(hours=24, seed=0, devices=args.devices, recovery_timeout=STEADY_S,
                                   time_recovery=120, probe_interval=45, set_time=args.set_time, lora=True,
                                   lora_share=0.0, no_alerts=False, quiet_handhelds=False, no_ble=True,
                                   until_aps_die=False, ap_dead_after=10.0, audible_hours="0-23",
                                   allow_firmware_mismatch=args.allow_firmware_mismatch, scenario="mixed",
                                   blackout=False, max_aps_down=2, min_gap=3, max_gap=15, aps=3, handhelds=2,
                                   lora_check=args.lora_check, update=args.update,
                                   install_board=args.install_board, release=args.release,
                                   bench=args.bench, secrets=str(ROOT / "firmware" / "common" / "lg_secrets.h"))
        grid = Grid(hargs, names, bus)

    watch, watch_url = None, None
    if args.watch and not args.page_only:
        # The beacon layer only: no admin password is given to it, so positions never reach this
        # screen (they cross the admin link only after a login, D70), which matters on a recording.
        watch = subprocess.Popen([sys.executable, str(ROOT / "tools" / "grid_watch.py"), "--no-browser",
                                  "--no-map-tiles", "--port", str(WATCH_PORT)], cwd=str(ROOT),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        watch_url = f"http://127.0.0.1:{WATCH_PORT}/"
    narrator = Narrator(args.voice_model)
    print(f"Narrator: Piper on this laptop, voices {', '.join(narrator.names)}" if narrator.paths else
          f"Narrator: the browser's voices ({narrator.error})", flush=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(bus, grid, watch_url, narrator))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Demo page: http://127.0.0.1:{args.port}/", flush=True)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, signal.default_int_handler)
    try:
        if grid is None:
            bus.publish({"type": "caption", "t": 0, "text": "Page only: no boards are open."})
            while True:
                time.sleep(1)
        grid.serve()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        if watch is not None:
            watch.terminate()
        if grid is not None:
            try:
                grid.let_go_of_ports()
                grid.restore()
            finally:
                grid.close_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())
