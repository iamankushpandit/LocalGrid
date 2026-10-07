#!/usr/bin/env python3
"""One button per fact: a self-running demo of the bench grid, made to be filmed.

Each button plays one claim about the grid from start to finish on the real boards, through their
USB cables: it checks the grid is ready, says what is about to happen, does it, shows it happening,
and ends with the numbers the boards themselves printed. Then it puts the grid back.

The page reads every caption and result aloud with the browser's own voice (on by default, the voice
chosen on the page), so a recording needs nobody speaking, and the demo waits for each spoken line to
finish before it acts.

  python tools/demo.py                 # open the boards and serve the page at http://127.0.0.1:8770/
  python tools/demo.py --page-only     # the page with no boards, to look at the layout

The page is local to this laptop and talks to nothing else.

How it works: the engine is the chaos harness (tools/chaos.py) with its fault injection, log parsing
and steady-state checks, driven by a fixed script instead of a random schedule. Faults go through the
cable and no other way (D103), and every radio switched off restores itself from the board's own clock
even if this tool dies. The handhelds send and receive every message themselves; the PC types console
commands and reads logs, and never joins the grid (D25).

Output (gitignored): chaos-runs/demo/<start time>/ with the raw log and events.jsonl of the session.
"""
import argparse
import http.server
import json
import pathlib
import queue
import re
import signal
import sys
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import chaos  # noqa: E402
from flash import acquire_lock, load_map  # noqa: E402

PAGE = ROOT / "tools" / "demo_page.html"
chaos.RUNS = ROOT / "chaos-runs" / "demo"

# Broadcast, group and alert text is logged in full by the receiving handheld; 1:1 text never is
# (AGENTS.md), so 1:1 messages are matched by (author, boot, seq) through chaos.RX instead.
FROM_TEXT = re.compile(r"\[MSG\] From (\d+) \(([^)]*)\)[^:]*: (.*)$")
NOT_SENT_TEXT = re.compile(r"\[MSG\] Not sent \(([^)]*)\): (.*)$")
PEOPLE_ROW = re.compile(r"^\s*(\d+)\s{2,}(\S.*?)\s{2,}(offline|online.*)$")

DELIVERY_S = 30          # a message over Wi-Fi that has not landed by then has not landed
LORA_DELIVERY_S = 75     # LoRa is seconds of airtime per frame, plus acknowledgement turns (D104)
REJOIN_S = 120           # handhelds of a dead AP find another one well inside this
RELINK_S = 150           # an AP back from a power cut is linked to the others well inside this
STEADY_S = 180


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

    def snapshot(self):
        aps = [{"index": a.index, "name": self.label(a), "present": self.present(a), "held": a.held,
                "links": sorted(a.links), "lora_links": sorted(a.lora_links), "bb_off": bool(a.bb_off),
                "lora_off": bool(a.lora_off), "time": (a.status or {}).get("time")} for a in self.aps]
        hhs = [{"index": h.index, "name": self.label(h), "present": self.present(h),
                "node": h.registered_node, "last_node": h.last_node, "wifi_off": bool(h.wifi_off),
                "lora": (self.lora_last.get(h.name) or {}).get("fitted")} for h in self.hhs]
        return {"aps": aps, "handhelds": hhs, "busy": self.busy, "ready": self.ready}

    def background(self):
        super().background()
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

    def handle(self, b, line):
        super().handle(b, line)
        now = time.monotonic()
        if not b.is_ap and (m := PEOPLE_ROW.search(line)):
            self.people[int(m.group(1))] = m.group(2).strip()
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

    def warm_up(self):
        self.caption("Finding the boards on this laptop's cables. Each one is restarted so the demo "
                     "starts from a known point.")
        self.start_boards()
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

    SCENARIOS = {
        "ap_out": ("Unplug an access point", "Its handhelds move by themselves; messages keep arriving.",
                   "sc_ap_out"),
        "lora_bridge": ("Cut the Wi-Fi between access points", "The next message crosses by LoRa.",
                        "sc_lora_bridge"),
        "emergency": ("SOS on every screen", "Over Wi-Fi and LoRa at once; one message stands it down.",
                      "sc_emergency"),
    }

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
            elif kind == "reset":
                self.busy = "reset"
                try:
                    self.reset_grid()
                finally:
                    self.busy = None
                    self.background()


def scenario_list():
    return [{"key": k, "title": t, "subtitle": s} for k, (t, s, _) in Grid.SCENARIOS.items()]


def make_handler(bus, grid):
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
                return self.reply(200, {"scenarios": scenario_list(), "boards": grid is not None})
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
                return self.reply(409, {"error": "page only: no boards are open"})
            if path == "/api/run":
                key = (qs.get("s") or [""])[0]
                if key not in Grid.SCENARIOS:
                    return self.reply(400, {"error": "no such scenario"})
                if grid.busy or not grid.ready or not grid.requests.empty():
                    return self.reply(409, {"error": "busy" if grid.ready else "warming up"})
                grid.requests.put(("run", key))
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
    args = ap.parse_args()

    bus = Bus()
    grid = None
    if not args.page_only:
        try:
            names = {d["id"]: d["name"] for d in load_map().get("devices", []) if d.get("id")}
        except (OSError, ValueError):
            names = {}
        acquire_lock()    # the same lock as flash.py and chaos.py: one tool on the boards at a time
        chaos.keep_awake()
        # The harness's own settings, fixed: no schedule, no random traffic, no BLE, no alert quiet hours.
        hargs = argparse.Namespace(hours=24, seed=0, devices=args.devices, recovery_timeout=STEADY_S,
                                   time_recovery=120, probe_interval=45, set_time=args.set_time, lora=True,
                                   lora_share=0.0, no_alerts=False, quiet_handhelds=False, no_ble=True,
                                   until_aps_die=False, ap_dead_after=10.0, audible_hours="0-23",
                                   allow_firmware_mismatch=args.allow_firmware_mismatch, scenario="mixed",
                                   blackout=False, max_aps_down=2, min_gap=3, max_gap=15, aps=3, handhelds=2,
                                   lora_check=args.lora_check, secrets=str(ROOT / "firmware" / "common" / "lg_secrets.h"))
        grid = Grid(hargs, names, bus)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(bus, grid))
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
        if grid is not None:
            try:
                grid.restore()
            finally:
                grid.close_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())
