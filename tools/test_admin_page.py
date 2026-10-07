#!/usr/bin/env python3
"""Tests the access point's admin page in a real browser, against a stand-in access point.

The admin page (firmware/node/main/web/admin.html) is served by an AP over the grid's own Wi-Fi, which
the development PC cannot join without losing its Internet connection. But the page is a web page and
the AP is, to it, a handful of HTTP answers. This tool is those answers: it serves the page exactly as
it is in the tree, answers /api/* with the same JSON and the same packed bytes an AP sends (the sample
replies tools/grid_watch.py checks its own decoders against), keeps the state an AP would keep - the
password, the groups, who may announce, the time zone, the LoRa power, the session - and drives the
page in a headless Edge or Chrome the way a person would. What the page showed, and every request it
sent back, are checked.

Three visits, each in a fresh browser:

  first run    an AP nobody has set up: the setup form, then every tab of the dashboard - the clock and
               setting the time, the map and a trail, the AP table, LoRa and its power, availability,
               the outage log and its filter, handhelds, making, changing and removing a group, who may
               announce, changing the password - then logging out, logging in with the old password and
               the new, and a session that ends under the page.
  returning    an AP already set up, opened at a tab's own address: the login form, then that tab.
  unreachable  an AP that does not answer: the page says so.

  python tools/test_admin_page.py            run the checks; exit 0 when all pass
  python tools/test_admin_page.py --serve    keep the stand-in AP up, to look at the page by hand
                                             (password: test-password)

No board is touched and nothing leaves this machine: the browser is started with every host but
127.0.0.1 unresolvable, so the map's tiles fail exactly as they do with no Internet, and the page's
"no map tiles" note is one of the things checked. One handheld in the stand-in's data has a name that is
markup, to check the page shows it as text and runs none of it.

What this does not prove: that a real AP's web server delivers the page, or that a real AP answers as
this one does. It proves the page.
"""
import argparse
import http.server
import importlib.util
import json
import os
import pathlib
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "firmware" / "node" / "main" / "web" / "admin.html"
PASSWORD = "test-password"            # the stand-in's password when it starts out already set up
SETUP_PASSWORD = "first-password-1"   # what the first-run visit sets
NEW_PASSWORD = "a-new-password-23"    # ... and changes it to
HOSTILE = 'T<img src=x onerror="window.__xss=1">m'   # a handheld's name, as text; never as markup
BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "/usr/bin/chromium", "/usr/bin/chromium-browser", "/usr/bin/google-chrome",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]


def load_samples():
    """The sample replies grid_watch.py keeps for its own self-check: one description of what an AP sends."""
    spec = importlib.util.spec_from_file_location("grid_watch", ROOT / "tools" / "grid_watch.py")
    gw = importlib.util.module_from_spec(spec)
    sys.modules["grid_watch"] = gw
    spec.loader.exec_module(gw)
    return gw


def ap_zone_rules():
    """The AP's own table of zone names and rules (tz_names.c, D100), to hold the page's rule against."""
    text = (ROOT / "firmware" / "node" / "main" / "tz_names.c").read_text(encoding="utf-8")
    macros = dict(re.findall(r'#define\s+(\w+)\s+"([^"]+)"', text))
    rules = {}
    for name, value in re.findall(r'\{\s*"([^"]+)",\s*("[^"]+"|\w+)\s*\}', text):
        rules[name] = value.strip('"') if value.startswith('"') else macros.get(value, "")
    return rules


# Runs first in the page: anything the page's own script throws is kept, and confirm() is answered by
# the test instead of by a dialog nobody is there to press.
CAPTURE = """<script>
window.__errors = [];
window.addEventListener("error", (e) => window.__errors.push(String(e.message || e)));
window.addEventListener("unhandledrejection", (e) => window.__errors.push("rejected: " + String(e.reason)));
window.__confirm = { answer: false, asked: [] };
window.confirm = (m) => { window.__confirm.asked.push(String(m)); return window.__confirm.answer; };
</script>"""

# What every visit's script starts with.
HELPERS = """
  const $ = (id) => document.getElementById(id);
  const shown = (el) => !!el && el.getClientRects().length > 0;
  const until = async (test, ms = 8000) => {
    const end = Date.now() + ms;
    while (Date.now() < end) { try { if (test()) return true; } catch (e) {} await new Promise((r) => setTimeout(r, 40)); }
    return false;
  };
  const pause = (ms) => new Promise((r) => setTimeout(r, ms));
  const txt = (id) => ($(id) ? $(id).textContent.replace(/\\s+/g, " ").trim() : null);
  const rows = (id) => [...$(id).querySelectorAll("tr")].map((tr) => [...tr.children].map((td) => td.textContent.replace(/\\s+/g, " ").trim()));
  const tab = (name) => document.querySelector('[role="tab"][data-tab="' + name + '"]').click();
  const tick = (box, on) => { box.checked = on; box.dispatchEvent(new Event("change", { bubbles: true })); };
  const out = { steps: [] };
  const step = (name, value) => out.steps.push([name, value]);
  const finish = async () => {
    out.errors = window.__errors;
    out.xss = window.__xss === undefined ? "none ran" : "RAN";
    await fetch("/test-result", { method: "POST", body: JSON.stringify(out) });
  };
"""

FIRST_RUN = """<script>
(async () => {""" + HELPERS + """
  try {
    /* ---- an AP nobody has set up ---- */
    step("an AP that is not set up shows the setup form, not the login", await until(() => shown($("setup-form"))) && !shown($("login-form")));
    out.setup_zone_shown = txt("setup-tz");
    out.setup_clock_running = /\\d\\d:\\d\\d:\\d\\d/.test(txt("setup-clock"));
    $("setup-name").value = "Test Grid";
    $("setup-password").value = "too short";
    $("setup-form").requestSubmit();
    step("a password the AP refuses is answered in the AP's words, and setup stays open",
         await until(() => /12 to 64/.test(txt("setup-msg"))) && shown($("setup-form")) && txt("setup-msg"));
    $("setup-password").value = "__SETUP_PASSWORD__";
    $("setup-form").requestSubmit();
    step("setting it up opens the dashboard", await until(() => shown($("tabs")) && !shown($("setup-form"))));
    out.title_after_setup = txt("title");
    out.setup_password_field = $("setup-password").value;

    /* ---- Overview ---- */
    step("the Overview tab is the one open", await until(() => shown($("time-card")) && /\\d\\d:\\d\\d:\\d\\d/.test(txt("grid-clock"))));
    await until(() => txt("s-node") !== "--");
    out.overview = { clock: txt("grid-clock"), date: txt("grid-date"), pill: txt("time-pill"), source: txt("time-source"),
                     zone_hint: txt("zone-hint"), subtitle: txt("subtitle"), node: txt("s-node"), uptime: txt("s-uptime"),
                     handhelds: txt("s-handhelds"), heap: txt("s-heap"), logout_shown: shown($("logout")) };
    out.only_overview_cards = shown($("grid-card")) && !shown($("lora-card")) && !shown($("groups-card")) && !shown($("security-card"));
    $("set-manual").click();
    out.time_empty_msg = txt("time-msg");
    $("use-device-time").click();
    step("with a GPS keeping the time, the AP's refusal to change it is shown in its words",
         await until(() => /GPS/.test(txt("time-msg"))) && txt("time-msg"));
    step("when the GPS has no fix the page stops saying the time is from it", await until(() => txt("time-pill") === "SET ON THIS AP", 9000) && txt("time-pill"));
    $("manual-time").value = "2031-03-04T05:06:07";
    out.manual_expected_ms = new Date("2031-03-04T05:06:07").getTime();
    $("set-manual").click();
    step("a time set by hand is taken", await until(() => /manual entry/.test(txt("time-msg"))) && txt("time-msg"));

    /* ---- Map ---- */
    tab("map");
    step("the Map tab draws the trail", await until(() => document.querySelectorAll(".loc-path").length > 0));
    out.hash_on_map = location.hash;
    out.map = { paths: document.querySelectorAll(".loc-path").length, dots: document.querySelectorAll(".loc-step").length,
                pins: document.querySelectorAll(".loc-pin").length, labels: [...document.querySelectorAll(".loc-label")].map((l) => l.textContent),
                widths: [...document.querySelectorAll(".loc-path")].map((p) => parseFloat(p.style.width)),
                dot_titles: [...document.querySelectorAll(".loc-step")].map((d) => d.title),
                offline: shown($("loc-offline")), ref: txt("loc-ref"), coords: txt("loc-coords"), detail: txt("loc-detail"),
                main_links: ["loc-geo", "loc-google", "loc-apple", "loc-osm"].map((id) => $(id).getAttribute("href")),
                positions: rows("loc-body"), heading: ($("loc-trails").querySelector("h2") || {}).textContent || "",
                trail: [...$("loc-trails").querySelectorAll("tbody tr")].map((tr) => ({
                  cells: [...tr.children].map((td) => td.textContent.trim()),
                  links: [...tr.querySelectorAll("a")].map((a) => [a.textContent, a.href, a.target, a.rel]) })) };

    /* ---- Network ---- */
    tab("network");
    step("the Network tab lists the APs", await until(() => shown($("nodes-card")) && rows("nodes-body").length === 3));
    out.network = { aps: rows("nodes-body"), lora_state: txt("lora-state"), lora_peers: rows("lora-body"), lora_counts: txt("lora-counts"),
                    strips: $("avail").querySelectorAll("svg").length,
                    minutes: [...$("avail").querySelectorAll("svg")].map((s) => s.querySelectorAll("rect").length),
                    sums: [...$("avail").querySelectorAll(".sum")].map((s) => s.textContent), avail_hint: txt("avail-hint"),
                    chips: [...$("incidents-filter").querySelectorAll("button")].map((b) => b.textContent), incidents: rows("incidents-body") };
    out.only_network_cards = shown($("avail-card")) && shown($("incidents-card")) && !shown($("time-card")) && !shown($("devices-card"));
    $("incidents-filter").querySelector('button[data-ap="2"]').click();
    out.network.incidents_south = rows("incidents-body");
    $("incidents-filter").querySelector('button[data-ap="0"]').click();
    out.network.incidents_main = { rows: rows("incidents-body").length, empty: txt("incidents-empty"), empty_shown: shown($("incidents-empty")) };
    $("incidents-filter").querySelector('button[data-ap="-1"]').click();
    out.network.incidents_all_again = rows("incidents-body").length;
    step("the Network tab shows the LoRa power", await until(() => shown($("lora-power")) && /dBm/.test(txt("lora-power-now"))));
    out.power = { before: txt("lora-power-now"), explained: txt("lora-power"),
                  marked_before: [...document.querySelectorAll("#lora-power button")].filter((b) => !b.classList.contains("ghost")).map((b) => b.dataset.power) };
    document.querySelector('#lora-power button[data-power="low"]').click();
    step("choosing Low is taken by the AP and shown back", await until(() => /low, 5 dBm/.test(txt("lora-power-now"))));
    out.power.after = txt("lora-power-now");
    out.power.msg = txt("lora-power-msg");
    out.power.marked_after = [...document.querySelectorAll("#lora-power button")].filter((b) => !b.classList.contains("ghost")).map((b) => b.dataset.power);

    /* ---- Handhelds: who is here, groups, who may announce ---- */
    tab("handhelds");
    step("the Handhelds tab lists the handhelds and the groups", await until(() => shown($("devices-card")) && rows("groups-body").length === 2));
    const groupRows = () => rows("groups-body").map((r) => r.slice(0, 2));
    const members = () => [...$("group-members").querySelectorAll("input")];
    const choose = (ids) => members().forEach((b) => { b.checked = ids.includes(b.value); });
    out.handhelds = { devices: rows("devices-body"), groups: groupRows(), choices: members().map((b) => [b.value, b.parentElement.textContent]),
                      markup_in_tables: document.querySelectorAll("#devices-body img, #groups-body img, #group-members img, #announce-members img, #loc-body img").length };
    out.groups = {};
    $("group-name").value = "Hikers"; choose([]);
    $("group-form").requestSubmit(); await pause(150);
    out.groups.no_members = txt("group-msg");
    $("group-name").value = "ABCDEFGHIJKLMNOP"; choose(["1"]);
    $("group-form").requestSubmit(); await pause(150);
    out.groups.long_ascii = txt("group-msg");
    $("group-name").value = "\\u00d1and\\u00faes del sur"; choose(["1"]);   /* 15 letters, 17 bytes */
    $("group-form").requestSubmit(); await pause(150);
    out.groups.long_bytes = txt("group-msg");
    $("group-name").value = "Hikers"; choose(["1", "4"]);
    $("group-form").requestSubmit();
    step("a new group appears in the list", await until(() => groupRows().length === 3) && groupRows());
    out.groups.added_msg = txt("group-msg");
    out.groups.form_after_add = [$("group-name").value, txt("group-form-title"), members().filter((b) => b.checked).length];
    $("groups-body").querySelector('[data-edit="1"]').click();
    out.groups.edit_form = [txt("group-form-title"), $("group-name").value, members().filter((b) => b.checked).map((b) => b.value), txt("group-save"), shown($("group-cancel"))];
    $("group-cancel").click();
    out.groups.after_cancel = [txt("group-form-title"), $("group-name").value, members().filter((b) => b.checked).length, shown($("group-cancel"))];
    $("groups-body").querySelector('[data-edit="1"]').click();
    $("group-name").value = "Kitchen"; choose(["1"]);
    $("group-form").requestSubmit();
    step("a changed group shows its new name and members", await until(() => groupRows().some((r) => r[0] === "Kitchen" && r[1] === "Ana")) && groupRows());
    window.__confirm.answer = false;
    $("groups-body").querySelector('[data-remove="2"]').click(); await pause(300);
    out.groups.after_refused_remove = groupRows().length;
    window.__confirm.answer = true;
    $("groups-body").querySelector('[data-remove="2"]').click();
    step("a removed group leaves the list", await until(() => groupRows().length === 2 && !groupRows().some((r) => r[0] === "Lake walk")) && groupRows());
    out.groups.confirm_asked = window.__confirm.asked;

    const boxes = () => [...$("announce-members").querySelectorAll("input")];
    out.announce = { all_before: $("announce-all").checked, before: boxes().map((b) => [b.value, b.checked, b.disabled]) };
    tick(boxes().find((b) => b.value === "3"), true);
    $("announce-form").requestSubmit();
    step("who may announce is saved", await until(() => txt("announce-msg") === "Saved.") && txt("announce-msg"));
    await until(() => boxes().filter((b) => b.checked).length === 2, 3000);
    boxes().forEach((b) => tick(b, false));
    $("announce-form").requestSubmit(); await pause(150);
    out.announce.nobody = txt("announce-msg");
    tick($("announce-all"), true);
    out.announce.all_ticked = boxes().map((b) => [b.checked, b.disabled]);
    $("announce-form").requestSubmit();
    step("letting everyone announce is saved", await until(() => txt("announce-msg") === "Everyone can announce.") && txt("announce-msg"));

    /* ---- Settings: firmware (D110) ---- */
    tab("settings");
    step("the Settings tab says what this AP runs", await until(() => shown($("fw-card")) && /cable/.test(txt("fw-running"))) && txt("fw-running"));
    out.fw = { running: txt("fw-running"), key: txt("fw-key"), carried_before: txt("fw-carried") };
    const lgfw = (magic, role, chip, serial, version, imageLen) => {
      const b = new Uint8Array(192 + imageLen), v = new DataView(b.buffer);
      b.set(magic ? [76, 71, 70, 87, 1] : [77, 90, 0, 0, 0], 0);
      b[5] = role.charCodeAt(0); b[6] = chip; v.setUint32(8, serial, true); v.setUint32(12, imageLen, true);
      for (let i = 0; i < version.length; i++) b[56 + i] = version.charCodeAt(i);
      for (let i = 192; i < b.length; i++) b[i] = i & 0xff;
      return new File([b], "update.lgfw");
    };
    const pick = async (f) => {
      const dt = new DataTransfer(); dt.items.add(f); $("fw-file").files = dt.files;
      $("fw-what").textContent = ""; $("fw-file").dispatchEvent(new Event("change"));
      await until(() => txt("fw-what") !== "");
    };
    await pick(lgfw(false, "H", 1, 7000, "hh-test", 4096));
    out.fw.bad = txt("fw-what"); out.fw.bad_send_disabled = $("fw-send").disabled;
    await pick(lgfw(true, "H", 1, 7000, "hh-test", 4096));
    out.fw.what = txt("fw-what"); out.fw.send_enabled = !$("fw-send").disabled;
    $("fw-form").requestSubmit();
    step("a handheld update is sent and the AP's answer is shown", await until(() => /carrying it/.test(txt("fw-msg"))) && txt("fw-msg"));
    out.fw.msg = txt("fw-msg");
    step("the card then says what this AP carries", await until(() => /hh-test/.test(txt("fw-carried"))) && txt("fw-carried"));
    out.fw.carried_after = txt("fw-carried");
    out.fw.others = txt("fw-others");
    out.fw.withdraw_shown = shown($("fw-withdraw"));
    /* D111: another chip's file, while this AP holds the only copy of the first: the AP refuses, in its words */
    await pick(lgfw(true, "H", 3, 7100, "hh-c6", 2048));
    $("fw-form").requestSubmit();
    await until(() => /only one carrying/.test(txt("fw-msg")));
    out.fw.only_copy = txt("fw-msg");
    /* withdrawing: asked first, then gone here */
    window.__confirm.answer = false; window.__confirm.asked = [];
    $("fw-withdraw").click(); await pause(300);
    out.fw.withdraw_declined_kept = /hh-test/.test(txt("fw-carried"));
    window.__confirm.answer = true;
    $("fw-withdraw").click();
    step("withdrawing it is asked first, then done", await until(() => txt("fw-carried") === "none") && txt("fw-msg"));
    out.fw.withdraw_msg = txt("fw-msg");
    out.fw.withdraw_asked = window.__confirm.asked;
    out.fw.withdraw_hidden_after = !shown($("fw-withdraw"));
    window.__confirm.answer = false;

    /* ---- Settings: the admin password ---- */
    step("the Settings tab has the password form", await until(() => shown($("password-form"))));
    out.only_settings_cards = !shown($("groups-card")) && !shown($("time-card")) && !shown($("nodes-card"));
    const pw = async (cur, a, b) => { $("pw-current").value = cur; $("pw-new").value = a; $("pw-again").value = b; $("password-form").requestSubmit(); await pause(200); };
    out.password = {};
    await pw("__SETUP_PASSWORD__", "__NEW_PASSWORD__", "__NEW_PASSWORD__x"); out.password.mismatch = txt("pw-msg");
    await pw("__SETUP_PASSWORD__", "elevenchars", "elevenchars"); out.password.short = txt("pw-msg");
    await pw("not the password", "__NEW_PASSWORD__", "__NEW_PASSWORD__");
    await until(() => !/Changing/.test(txt("pw-msg"))); out.password.wrong_current = txt("pw-msg");
    await pw("__SETUP_PASSWORD__", "__NEW_PASSWORD__", "__NEW_PASSWORD__");
    step("the password is changed", await until(() => /Password changed/.test(txt("pw-msg"))) && txt("pw-msg"));
    out.password.fields_after = ["pw-current", "pw-new", "pw-again"].map((id) => $(id).value);

    /* ---- out, and back in ---- */
    $("logout").click();
    step("logging out shows the login form and hides everything else", await until(() => shown($("login-form")) && !shown($("tabs")) && !shown($("security-card")) && !shown($("logout"))));
    $("login-password").value = "__SETUP_PASSWORD__";
    $("login-form").requestSubmit();
    step("the old password no longer logs in", await until(() => /wrong/i.test(txt("login-msg"))) && shown($("login-form")) && txt("login-msg"));
    $("login-password").value = "__NEW_PASSWORD__";
    $("login-form").requestSubmit();
    step("the new password does", await until(() => shown($("tabs")) && !shown($("login-form"))));
    out.tab_after_login = location.hash;
    await until(() => shown($("password-form")));

    /* ---- a session that ends under the page ---- */
    await fetch("/test-control", { method: "POST", body: JSON.stringify({ expire: true }) });
    step("when the AP ends the session the page goes back to the login form by itself", await until(() => shown($("login-form")) && !shown($("tabs")), 9000));
  } catch (e) {
    out.driver_error = String(e && e.stack || e);
  }
  await finish();
})();
</script>"""

RETURNING = """<script>
(async () => {""" + HELPERS + """
  try {
    step("an AP that is set up shows the login form, not the setup", await until(() => shown($("login-form"))) && !shown($("setup-form")));
    out.before_login = { tabs: shown($("tabs")), logout: shown($("logout")), cards: document.querySelectorAll("main > section[data-tab]:not([hidden])").length,
                         title: txt("title"), subtitle: txt("subtitle") };
    $("login-password").value = "__PASSWORD__";
    $("login-form").requestSubmit();
    step("logging in opens the tab the address named", await until(() => shown($("nodes-card")) && rows("nodes-body").length === 3));
    out.hash = location.hash;
    out.selected = [...document.querySelectorAll('[role="tab"]')].filter((b) => b.getAttribute("aria-selected") === "true").map((b) => b.dataset.tab);
    out.overview_hidden = !shown($("time-card")) && !shown($("grid-card"));
    out.zone_hint_default = (tab("overview"), await until(() => shown($("time-card"))), txt("zone-hint"));
    out.password_field = $("login-password").value;
    await fetch("/test-control", { method: "POST", body: JSON.stringify({ derive: true }) });
    await until(() => /worked out/.test(txt("zone-hint")), 15000);
    out.zone_hint_derived = txt("zone-hint");
    await fetch("/test-control", { method: "POST", body: JSON.stringify({ no_rule: true }) });
    await until(() => /does not know/.test(txt("zone-hint")), 15000);
    out.zone_hint_no_rule = txt("zone-hint");
  } catch (e) {
    out.driver_error = String(e && e.stack || e);
  }
  await finish();
})();
</script>"""

UNREACHABLE = """<script>
(async () => {""" + HELPERS + """
  try {
    step("an AP that does not answer is reported in plain words", await until(() => /Cannot reach this AP/.test(txt("loading"))) && txt("loading"));
    out.forms = shown($("login-form")) || shown($("setup-form")) || shown($("tabs"));
  } catch (e) {
    out.driver_error = String(e && e.stack || e);
  }
  await finish();
})();
</script>"""


class StandInAP:
    """What an AP answers, the state it keeps to answer it, and a record of what it was asked."""

    def __init__(self, gw, configured=True, reachable=True):
        self.gw = gw
        self.configured = configured
        self.reachable = reachable
        self.password = PASSWORD
        sample = json.loads(gw.sample_status_json().decode("utf-8"))
        self.sample = sample
        self.grid_name = sample["grid_name"]
        self.timezone, self.posix_tz = sample["timezone"], sample["posix_tz"]
        self.users = [dict(u) for u in sample["users"]]
        self.users[-1]["name"] = HOSTILE
        self.groups = [dict(g) for g in sample["groups"]]
        self.next_group = max(g["id"] for g in self.groups) + 1
        self.announce_all, self.announcers = sample["announce_all"], list(sample["announcers"])
        self.gps_fix = True
        self.power = 22
        self.carried = None      # D110: the handheld update this stand-in was given, as the status says it
        self.derived = False     # the zone rule is one the AP worked out from the zone's name (D100)
        self.uploads = []        # (bytes, X-CSRF header, had the session cookie)
        self.token = self.csrf = None
        self.posts = []          # (path, body, X-CSRF header, had the session cookie, when)
        self.refused = []        # (method, path, status)
        self.result = None
        self.done = threading.Event()

    def open_session(self):
        self.token, self.csrf = secrets.token_hex(16), secrets.token_hex(16)
        return f"lg_sid={self.token}; HttpOnly; SameSite=Strict; Path=/"

    def status(self):
        s = json.loads(json.dumps(self.sample))
        s.update(grid_name=self.grid_name, timezone=self.timezone, posix_tz=self.posix_tz, users=self.users,
                 groups=self.groups, announce_all=self.announce_all, announcers=self.announcers)
        s["gps"]["fix"] = self.gps_fix
        s["lora"]["power"] = self.power
        s["firmware"]["carried"] = self.carried
        s["posix_tz_derived"] = self.derived
        for d in s["devices"]:
            d["name"] = next((u["name"] for u in self.users if u["device"] == d["device"]), d["name"])
        return json.dumps(s).encode("utf-8")

    def groups_request(self, body):
        """POST /api/groups, as h_groups answers it. Returns (status, error text or None)."""
        known = {u["device"] for u in self.users}

        def devices():
            try:
                picked = [int(x) for x in str(body.get("devices", "")).split(",") if x != ""]
            except ValueError:
                return None
            return picked if picked and all(d in known for d in picked) else None

        op = body.get("op")
        if op == "announcers":
            if body.get("devices") == "all":
                self.announce_all, self.announcers = True, []
                return 200, None
            picked = devices()
            if picked is None:
                return 400, "Pick handhelds from the list."
            self.announce_all, self.announcers = False, picked
            return 200, None
        if op == "delete":
            if not any(g["id"] == body.get("id") for g in self.groups):
                return 404, "That group no longer exists. Reload the page."
            self.groups = [g for g in self.groups if g["id"] != body.get("id")]
            return 200, None
        if op in ("create", "update"):
            name = str(body.get("name", ""))
            if not 1 <= len(name.encode("utf-8")) <= 15:
                return 400, "A group name is 1 to 15 bytes (fewer with accents or emoji)."
            picked = devices()
            if picked is None:
                return 400, "Pick handhelds from the list."
            if op == "create":
                self.groups.append({"id": self.next_group, "name": name, "members": picked})
                self.next_group += 1
                return 200, None
            for g in self.groups:
                if g["id"] == body.get("id"):
                    g["name"], g["members"] = name, picked
                    return 200, None
            return 404, "That group no longer exists. Reload the page."
        return 400, "Unknown group action."


def make_handler(ap, page_bytes):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, body, ctype="application/json", status=200, cookie=None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            if cookie:
                self.send_header("Set-Cookie", cookie)
            self.end_headers()
            self.wfile.write(body)

        def _ok(self, **more):
            self._send(json.dumps({"ok": True, **more}).encode())

        def _error(self, status, text):
            ap.refused.append((self.command, self.path.split("?")[0], status))
            self._send(json.dumps({"error": text}).encode(), status=status)

        def _session(self):
            return ap.token is not None and f"lg_sid={ap.token}" in (self.headers.get("Cookie") or "")

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/":
                return self._send(page_bytes, "text/html; charset=utf-8")
            if path == "/favicon.svg":
                return self._send(b"<svg xmlns='http://www.w3.org/2000/svg'/>", "image/svg+xml")
            if not ap.reachable:
                return self._send(b"", status=503)
            if path == "/api/state":
                on = self._session()
                return self._send(json.dumps({"configured": ap.configured, "logged_in": on, "csrf": ap.csrf if on else "",
                                              "grid_name": ap.grid_name if ap.configured else "", "ap": 0,
                                              "ap_name": "MAIN"}).encode())
            if not self._session():
                return self._error(401, "Log in to see grid status.")
            if path == "/api/status":
                return self._send(ap.status())
            if path == "/api/history":
                return self._send(ap.gw.sample_history_blob(), "application/octet-stream")
            if path == "/api/trails":
                return self._send(ap.gw.sample_trails_blob(), "application/octet-stream")
            self._error(404, "No such page.")

        def do_POST(self):
            path = self.path.split("?")[0]
            raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if path == "/test-result":
                ap.result = json.loads(raw.decode("utf-8"))
                ap.done.set()
                return self._send(b"{}")
            if path == "/test-control":
                control = json.loads(raw.decode("utf-8"))
                if control.get("expire"):
                    ap.token = ap.csrf = None     # as an AP that restarted, or an admin who changed the password
                if control.get("derive"):
                    ap.derived = True             # the rule was worked out from the zone's name, not saved
                if control.get("no_rule"):
                    ap.derived, ap.posix_tz = False, ""   # a zone this AP has no rule for
                return self._send(b"{}")
            if path == "/api/update":
                # D110: the body is the update file's bytes. The real AP checks the signature; this one
                # only reads what the header says, as the AP does once the signature has passed.
                ap.uploads.append((raw, self.headers.get("X-CSRF"), self._session()))
                if not self._session():
                    return self._error(401, "Log in to update the firmware.")
                if ap.csrf is None or self.headers.get("X-CSRF") != ap.csrf:
                    return self._error(403, "Request blocked. Reload the page and try again.")
                if len(raw) <= 192 or raw[:4] != b"LGFW":
                    return self._error(400, "That is not a LocalGrid update file.")
                if raw[5:6] == b"H" and ap.carried and ap.carried["chip"] != raw[6] and \
                        not any(x["chip"] == ap.carried["chip"] for x in ap.sample["firmware"]["neighbours"]):
                    return self._error(409, "This access point is the only one carrying the handheld update for chip "
                                       f"{ap.carried['chip']} ({ap.carried['version']}). Give this file to another access "
                                       "point, or withdraw that one first.")
                if raw[5:6] == b"H":
                    version = raw[56:88].split(b"\0")[0].decode("ascii", "replace")
                    ap.carried = {"serial": int.from_bytes(raw[8:12], "little"), "version": version, "chip": raw[6]}
                    return self._send(json.dumps({"ok": True, "slot": "carried", "note": "That is a handheld update. "
                                      "This access point is carrying it and will offer it to handhelds and to the other "
                                      "access points. Nothing restarts here."}).encode())
                return self._send(json.dumps({"ok": True, "slot": "pending", "note": "Restart this access point to "
                                  "run the new firmware."}).encode())
            try:
                body = json.loads(raw.decode("utf-8") or "{}")
            except ValueError:
                return self._error(400, "A request body is required.")
            ap.posts.append((path, body, self.headers.get("X-CSRF"), self._session(), time.time()))
            if path == "/api/setup":
                if ap.configured:
                    return self._error(409, "LocalGrid is already set up. Log in instead.")
                name, password = str(body.get("grid_name", "")), str(body.get("password", ""))
                if not name or not password or "unix_ms" not in body:
                    return self._error(400, "Grid name, password, and time are required.")
                if not 1 <= len(name) <= 32:
                    return self._error(400, "Grid name must be 1 to 32 characters.")
                if not 12 <= len(password) <= 64:
                    return self._error(400, "Password must be 12 to 64 characters.")
                ap.configured, ap.grid_name, ap.password = True, name, password
                ap.timezone, ap.posix_tz = str(body.get("timezone", "")), str(body.get("posix_tz", ""))
                return self._setup_ok()
            if path == "/api/login":
                if not ap.configured or body.get("password") != ap.password:
                    return self._error(401, "Wrong password.")
                cookie = ap.open_session()
                return self._send(json.dumps({"ok": True, "csrf": ap.csrf}).encode(), cookie=cookie)
            if not self._session():
                return self._error(401, "Log in first.")
            if self.headers.get("X-CSRF") != ap.csrf:
                return self._error(403, "Request blocked. Reload the page and try again.")
            if path == "/api/logout":
                ap.token = ap.csrf = None
                return self._ok()
            if path == "/api/withdraw":
                if not ap.carried:
                    return self._error(409, "This access point carries no handheld update.")
                ap.carried = None
                return self._ok(note="Withdrawn here, and every access point has been told to stop offering it.")
            if path == "/api/lora":
                level = {"full": 22, "medium": 14, "low": 5}.get(body.get("power"))
                if level is None:
                    return self._error(400, "The power is full, medium or low.")
                ap.power = level
                return self._ok(power=level)
            if path == "/api/time":
                if not isinstance(body.get("unix_ms"), (int, float)) or body["unix_ms"] < 1700000000000:
                    return self._error(400, "A valid time is required.")
                ap.timezone = str(body.get("timezone", "")) or ap.timezone
                ap.posix_tz = str(body.get("posix_tz", "")) or ap.posix_tz
                if ap.gps_fix:
                    ap.gps_fix = False    # the next status has no fix, as if the sky had been lost
                    return self._error(409, "Time zone saved. Grid time comes from the GPS on this AP while it "
                                            "has a fix, so it was not changed.")
                return self._ok()
            if path == "/api/password":
                if not body.get("current") or not body.get("password"):
                    return self._error(400, "The current and the new password are required.")
                if body["current"] != ap.password:
                    return self._error(403, "The current password is wrong.")
                ap.password = body["password"]
                return self._ok()
            if path == "/api/groups":
                status, text = ap.groups_request(body)
                return self._ok() if status == 200 else self._error(status, text)
            self._error(404, "No such page.")

        def _setup_ok(self):
            cookie = ap.open_session()
            self._send(json.dumps({"ok": True, "csrf": ap.csrf}).encode(), cookie=cookie)

    return Handler


def page_with(driver):
    html = PAGE.read_text(encoding="utf-8")
    if driver is None:
        return html.encode("utf-8")
    assert "<head>" in html and "</body>" in html, "admin.html has changed shape: no <head> or </body>"
    driver = (driver.replace("__SETUP_PASSWORD__", SETUP_PASSWORD).replace("__NEW_PASSWORD__", NEW_PASSWORD)
              .replace("__PASSWORD__", PASSWORD))
    return html.replace("<head>", "<head>" + CAPTURE, 1).replace("</body>", driver + "</body>", 1).encode("utf-8")


def find_browser():
    named = os.environ.get("LG_BROWSER")
    for path in ([named] if named else []) + BROWSERS:
        if path and pathlib.Path(path).exists():
            return path
    for name in ("msedge", "chrome", "chromium", "chromium-browser", "google-chrome"):
        found = shutil.which(name)
        if found:
            return found
    return None


def visit(browser, ap, driver, fragment, timeout):
    """One fresh browser on one stand-in AP. Returns True when the page's script reported back."""
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(ap, page_with(driver)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="lg-admin-test-")
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                             "--disable-extensions", f"--user-data-dir={profile}", "--window-size=1100,1500",
                             "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
                             f"http://127.0.0.1:{server.server_address[1]}/{fragment}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        return ap.done.wait(timeout)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
        server.shutdown()


def common(ap):
    r = ap.result or {}
    return [(name, bool(value), value) for name, value in r.get("steps", [])] + [
        ("the page's own script raised no error", r.get("errors") == [] and "driver_error" not in r,
         r.get("errors") or r.get("driver_error")),
        ("a name that is markup ran nothing", r.get("xss") == "none ran", r.get("xss")),
    ]


def judge_first_run(ap, started):
    r = ap.result or {}
    o, m, n, h = r.get("overview") or {}, r.get("map") or {}, r.get("network") or {}, r.get("handhelds") or {}
    g, a, pwd, power = r.get("groups") or {}, r.get("announce") or {}, r.get("password") or {}, r.get("power") or {}
    fw = r.get("fw") or {}
    good_uploads = [u for u in ap.uploads if u[1] and u[1] == u[1] and u[2]]
    posts = lambda path: [p for p in ap.posts if p[0] == path]      # noqa: E731
    setups, times, group_posts = posts("/api/setup"), posts("/api/time"), posts("/api/groups")
    good_setup = setups[-1][1] if setups else {}
    zone, rule = good_setup.get("timezone", ""), good_setup.get("posix_tz", "")
    table = ap_zone_rules()
    trail = m.get("trail") or []
    first = trail[0] if trail else {"cells": [], "links": []}
    last = trail[-1] if trail else {"cells": [], "links": []}
    aps = n.get("aps") or [[], [], []]
    hostile_stripped = HOSTILE.replace("<", "").replace("&", "")
    after_login = [x for x in ap.refused if x[1] not in ("/api/login", "/api/setup", "/api/time", "/api/password")
                   and x != ("POST", "/api/update", 409)]   # the only-copy refusal is asked for on purpose (D111)
    checks = common(ap) + [
        # setup
        ("setup shows the zone and a running clock taken from this device", bool(r.get("setup_zone_shown")) and r.get("setup_clock_running"),
         (r.get("setup_zone_shown"), r.get("setup_clock_running"))),
        ("setup was sent twice: the refused password, then the good one, with the name, the time and the zone",
         len(setups) == 2 and good_setup.get("grid_name") == "Test Grid" and good_setup.get("password") == SETUP_PASSWORD
         and abs(good_setup.get("unix_ms", 0) / 1000 - setups[-1][4]) < 15 and set(good_setup) == {"grid_name", "password", "unix_ms", "timezone", "posix_tz"},
         [sorted(s[1]) for s in setups]),
        ("the zone rule the page worked out is the one the AP's own table has for that zone"
         + ("" if zone in table else f" (not checked: {zone or 'no zone'} is not in the AP's table)"),
         bool(zone) and bool(rule) and (zone not in table or table[zone] == rule), (zone, rule, table.get(zone))),
        ("the grid's name becomes the page's title", r.get("title_after_setup") == "Test Grid", r.get("title_after_setup")),
        ("the setup password is not left in its field", r.get("setup_password_field") == "", r.get("setup_password_field")),
        # overview
        ("the clock runs and the date is shown", re.fullmatch(r"\d\d:\d\d:\d\d", o.get("clock") or "") is not None and bool(o.get("date")),
         (o.get("clock"), o.get("date"))),
        ("a GPS keeping the time is said, with its satellites", o.get("pill") == "FROM GPS (9 satellites)", o.get("pill")),
        ("where the time came from is said", "GPS" in (o.get("source") or "") and "MAIN" in (o.get("source") or ""), o.get("source")),
        ("the zone handhelds use is the one setup saved", bool(rule) and rule in (o.get("zone_hint") or "") and zone in (o.get("zone_hint") or ""),
         o.get("zone_hint")),
        ("the page says which AP is serving it and in what zone", o.get("subtitle") == f"LocalGrid Access Point, served by AP MAIN, time zone {zone}",
         o.get("subtitle")),
        ("this AP's name, uptime, handhelds and free memory are shown",
         (o.get("node"), o.get("uptime"), o.get("handhelds"), o.get("heap")) == ("MAIN", "5h 00m", "3", "115 KB"),
         (o.get("node"), o.get("uptime"), o.get("handhelds"), o.get("heap"))),
        ("Log out is offered once logged in", o.get("logout_shown") is True, o.get("logout_shown")),
        ("only the Overview's cards are on its tab", r.get("only_overview_cards") is True, r.get("only_overview_cards")),
        ("Set with no time picked asks for one and sends nothing", r.get("time_empty_msg") == "Pick a date and time first.", r.get("time_empty_msg")),
        ("the time was sent twice: this device's clock, then the time typed in",
         len(times) == 2 and abs(times[0][1].get("unix_ms", 0) / 1000 - times[0][4]) < 15
         and times[1][1].get("unix_ms") == r.get("manual_expected_ms"), [t[1].get("unix_ms") for t in times]),
        ("every time request carries the zone and its rule", all(t[1].get("timezone") == zone and t[1].get("posix_tz") == rule for t in times) and bool(times),
         [(t[1].get("timezone"), t[1].get("posix_tz")) for t in times]),
        # map
        ("opening a tab puts it in the address, so a reload lands there", r.get("hash_on_map") == "#map", r.get("hash_on_map")),
        ("a trail of three steps is two lines and two dots, the newest being the person's pin",
         m.get("paths") == 2 and m.get("dots") == 2 and (m.get("pins") or 0) >= 2, (m.get("paths"), m.get("dots"), m.get("pins"))),
        ("each line has a length: the steps are apart on the map", all(w and w > 2 for w in m.get("widths") or [0]), m.get("widths")),
        ("each dot says whose step it is and which", [t.split(",")[1].strip() for t in m.get("dot_titles") or []] == ["step 2 of 3", "step 1 of 3"]
         and all(t.startswith("Priya") for t in m.get("dot_titles") or []), m.get("dot_titles")),
        ("every pin is named", sorted(m.get("labels") or []) == ["MAIN", "Priya"], m.get("labels")),
        ("with no Internet the page says why there is no map behind the pins", m.get("offline") is True, m.get("offline")),
        ("MAIN's position is given in figures and in degrees, and the page says the GPS has lost its fix by then",
         m.get("coords") == "47.623450, 13.045670" and "47\u00b0" in (m.get("detail") or "")
         and "last known position; the GPS has no fix now" in (m.get("detail") or "")
         and (m.get("ref") or "").startswith("MAIN"), (m.get("ref"), m.get("coords"), ascii(m.get("detail")))),
        ("MAIN opens in the phone's maps, Google Maps, Apple Maps and OpenStreetMap, at its own spot",
         len(m.get("main_links") or []) == 4 and m["main_links"][0].startswith("geo:47.623450,13.045670")
         and m["main_links"][1] == "https://www.google.com/maps/search/?api=1&query=47.623450,13.045670"
         and m["main_links"][2].startswith("https://maps.apple.com/?ll=47.623450,13.045670")
         and m["main_links"][3].startswith("https://www.openstreetmap.org/?mlat=47.623450&mlon=13.045670"), m.get("main_links")),
        ("the positions table lists everyone, with how far from MAIN", len(m.get("positions") or []) == 2
         and m["positions"][0][0].startswith("MAIN") and m["positions"][1][0] == "Priya" and m["positions"][1][1] == "47.62510, 13.04890"
         and re.search(r"\d+ m [NESW]+", m["positions"][1][2] or "") is not None, m.get("positions")),
        ("the trail is headed with the person's name and how many steps", m.get("heading") == "Priya's last 3 steps", m.get("heading")),
        ("every step is listed, newest first, numbered, the ends named", len(trail) == 3 and first["cells"][0].startswith("3")
         and "last" in first["cells"][0] and last["cells"][0].startswith("1") and "first" in last["cells"][0], [x["cells"][0] for x in trail]),
        ("each step shows its coordinates", [x["cells"][2] for x in trail] == ["47.62510, 13.04890", "47.62501, 13.04878", "47.62492, 13.04870"],
         [x["cells"][2] for x in trail]),
        ("each step opens in OpenStreetMap and in Google Maps at that spot, in a new tab",
         len(first["links"]) == 2 and first["links"][0][:3] == ["OpenStreetMap",
         "https://www.openstreetmap.org/?mlat=47.625100&mlon=13.048900#map=19/47.625100/13.048900", "_blank"]
         and "noopener" in first["links"][0][3]
         and first["links"][1][1] == "https://www.google.com/maps/search/?api=1&query=47.625100,13.048900", first["links"]),
        ("the oldest step's link is its own spot, not the newest's", len(last["links"]) == 2 and "mlat=47.624920&mlon=13.048700" in last["links"][0][1],
         last["links"][:1]),
        # network
        ("the AP table says which AP this is, which answers and how well, and which does not",
         [x[0] for x in aps] == ["MAIN", "NORTH", "SOUTH"] and aps[0][1] == "YOU ARE HERE" and aps[0][6] == "this AP"
         and aps[1][1] == "ONLINE" and aps[1][6] == "-62 dBm" and aps[2][1] == "UNREACHABLE" and aps[2][2] == "--"
         and aps[2][6].startswith("last heard"), [x[:3] + x[6:] for x in aps]),
        ("each AP's uptime, time and last restart are shown, with why it restarted", aps[0][2] == "5h 00m" and "brownout" in aps[2][5]
         and all(x[3] for x in aps) and all(x[4] for x in aps), [(x[2], x[3], x[4], x[5]) for x in aps]),
        ("only the Network's cards are on its tab", r.get("only_network_cards") is True, r.get("only_network_cards")),
        ("LoRa's state, its peers and its counters are shown", bool(n.get("lora_state")) and len(n.get("lora_peers") or []) == 2
         and "Frames out 412, in 388" in (n.get("lora_counts") or ""), (n.get("lora_state"), n.get("lora_peers"), n.get("lora_counts"))),
        ("availability is a strip of 120 minutes for each AP, with how much of it was up", n.get("strips") == 3 and n.get("minutes") == [120, 120, 120]
         and len(n.get("sums") or []) == 3 and all(re.search(r"%|no record", s) for s in n["sums"]) and "MAIN" in (n.get("avail_hint") or ""),
         (n.get("strips"), n.get("minutes"), n.get("sums"))),
        ("the outage log lists both outages, newest first, with what happened", len(n.get("incidents") or []) == 2
         and n["incidents"][0][1] == "SOUTH" and "brownout" in n["incidents"][0][3] and n["incidents"][0][2] == "4m 00s"
         and n["incidents"][1][1] == "NORTH" and "link dropped" in n["incidents"][1][3], n.get("incidents")),
        ("the outage filter counts each AP's", n.get("chips") == ["All APs (2)", "MAIN (0)", "NORTH (1)", "SOUTH (1)"], n.get("chips")),
        ("choosing an AP shows only its outages; one with none says so; All brings them back",
         len(n.get("incidents_south") or []) == 1 and n["incidents_south"][0][1] == "SOUTH" and (n.get("incidents_main") or {}).get("rows") == 0
         and n["incidents_main"]["empty"] == "No outage on record for MAIN." and n["incidents_main"]["empty_shown"] is True
         and n.get("incidents_all_again") == 2, (n.get("incidents_south"), n.get("incidents_main"), n.get("incidents_all_again"))),
        ("the LoRa power in use is shown, with the AP it belongs to", power.get("before") == "In use on MAIN: full, 22 dBm."
         and power.get("marked_before") == ["full"], (power.get("before"), power.get("marked_before"))),
        ("the page says what the power is for", "close together" in (power.get("explained") or "") and "Full reaches furthest" in (power.get("explained") or ""),
         power.get("explained")),
        ("choosing Low sent exactly that, once", [p[1] for p in posts("/api/lora")] == [{"power": "low"}], [p[1] for p in posts("/api/lora")]),
        ("the AP's new power is what the page then shows, and says is kept", power.get("after") == "In use on MAIN: low, 5 dBm."
         and power.get("marked_after") == ["low"] and ap.power == 5 and "kept across restarts" in (power.get("msg") or ""),
         (power.get("after"), power.get("marked_after"), power.get("msg"))),
        # handhelds
        ("each handheld's firmware is shown as it reported it, or that it has not",
         [r[3] if len(r) > 3 else None for r in h.get("devices") or []] == ["2026.10.6 (serial 6000)", "installed by cable", "not reported"],
         h.get("devices")),
        ("the handhelds are listed with whether each is connected and to which AP",
         [r[:3] for r in h.get("devices") or []] == [["Ana", "ONLINE", "MAIN"], ["Priya", "ONLINE", "SOUTH"], [hostile_stripped, "OFFLINE", "--"]], h.get("devices")),
        ("the groups are listed with their members by name", h.get("groups") == [["Cooks", "Ana, Priya"], ["Lake walk", f"Priya, {HOSTILE}"]],
         h.get("groups")),
        ("a name that is markup is shown as the text it is, in every list", h.get("markup_in_tables") == 0
         and [c[1] for c in h.get("choices") or []] == ["Ana", "Priya", HOSTILE], (h.get("markup_in_tables"), h.get("choices"))),
        ("a group with nobody in it is refused by the page", g.get("no_members") == "Pick at least one handheld.", g.get("no_members")),
        ("a name too long is refused by the page, counted in bytes and not letters",
         g.get("long_ascii") == "That name is too long: 15 bytes at most." and g.get("long_bytes") == "That name is too long: 15 bytes at most.",
         (g.get("long_ascii"), g.get("long_bytes"))),
        ("the page says the group was added and clears the form", g.get("added_msg") == "Hikers added." and g.get("form_after_add") == ["", "New group", 0],
         (g.get("added_msg"), g.get("form_after_add"))),
        ("Edit fills the form with the group as it is", g.get("edit_form") == ["Edit Cooks", "Cooks", ["1", "3"], "Save changes", True], g.get("edit_form")),
        ("Cancel puts the form back to a new group", g.get("after_cancel") == ["New group", "", 0, False], g.get("after_cancel")),
        ("Remove asks first, saying what is lost, and No removes nothing",
         g.get("confirm_asked") == ["Remove Lake walk? Its messages are deleted on every handheld."] * 2 and g.get("after_refused_remove") == 3,
         (g.get("confirm_asked"), g.get("after_refused_remove"))),
        ("who may announce starts as the AP has it: not everyone, only Ana", a.get("all_before") is False
         and a.get("before") == [["1", True, False], ["3", False, False], ["4", False, False]], (a.get("all_before"), a.get("before"))),
        ("choosing nobody is refused by the page", a.get("nobody") == "Pick at least one handheld, or choose everyone.", a.get("nobody")),
        ("choosing everyone ticks and locks every box", a.get("all_ticked") == [[True, True]] * 3, a.get("all_ticked")),
        ("the AP was sent exactly these group changes, in order, and no refused one",
         [p[1] for p in group_posts] == [
             {"op": "create", "id": 0, "name": "Hikers", "devices": "1,4"},
             {"op": "update", "id": 1, "name": "Kitchen", "devices": "1"},
             {"op": "delete", "id": 2},
             {"op": "announcers", "devices": "1,3"},
             {"op": "announcers", "devices": "all"}], [p[1] for p in group_posts]),
        ("the AP ends with the groups and announcers the page was told to make",
         [(x["name"], x["members"]) for x in ap.groups] == [("Kitchen", [1]), ("Hikers", [1, 4])] and ap.announce_all is True,
         ([(x["name"], x["members"]) for x in ap.groups], ap.announce_all)),
        # settings
        ("only the Settings' cards are on its tab", r.get("only_settings_cards") is True, r.get("only_settings_cards")),
        ("two different new passwords are refused by the page", pwd.get("mismatch") == "The two new passwords are different.", pwd.get("mismatch")),
        ("a new password that is too short is refused by the page", pwd.get("short") == "The new password must be 12 to 64 characters.", pwd.get("short")),
        ("a wrong current password is answered in the AP's words", pwd.get("wrong_current") == "The current password is wrong.", pwd.get("wrong_current")),
        ("no password is left in the form", pwd.get("fields_after") == ["", "", ""], pwd.get("fields_after")),
        # firmware (D110)
        ("Settings says what this AP runs and that it takes signed updates",
         "installed by cable" in (fw.get("running") or "") and "release key" in (fw.get("key") or ""), (fw.get("running"), fw.get("key"))),
        ("before anything is carried, the card says so", fw.get("carried_before") == "none", fw.get("carried_before")),
        ("a file that is not an update is refused by the page, and nothing can be sent",
         fw.get("bad") == "That is not a LocalGrid update file." and fw.get("bad_send_disabled") is True, (fw.get("bad"), fw.get("bad_send_disabled"))),
        ("the page reads the update's own header and says what it is",
         fw.get("what") == "A handheld update for ESP32: hh-test, serial 7000, 4 KB." and fw.get("send_enabled") is True, fw.get("what")),
        ("the file went to the AP whole, with the session and its CSRF token",
         len(good_uploads) == 2 and len(good_uploads[0][0]) == 192 + 4096 and good_uploads[0][2]
         and good_uploads[0][0][192:200] == bytes(range(192, 200)), [(len(u[0]), bool(u[1]), u[2]) for u in ap.uploads]),
        ("what the other access points carry is listed, with which AP",
         fw.get("others") == "ESP32-S3 serial 6100 (SOUTH)", fw.get("others")),
        ("a file that would push out the grid's only copy of another chip's update is refused, in the AP's words",
         (fw.get("only_copy") or "").startswith("This access point is the only one carrying the handheld update for chip 1"),
         fw.get("only_copy")),
        ("Withdraw is offered while something is carried, asks first, and saying no keeps it",
         fw.get("withdraw_shown") is True and fw.get("withdraw_declined_kept") is True and len(fw.get("withdraw_asked") or []) == 2,
         (fw.get("withdraw_shown"), fw.get("withdraw_declined_kept"), fw.get("withdraw_asked"))),
        ("withdrawn, the card says nothing is carried, the button goes, and the AP's answer is shown",
         (fw.get("withdraw_msg") or "").startswith("Withdrawn here") and fw.get("withdraw_hidden_after") is True,
         (fw.get("withdraw_msg"), fw.get("withdraw_hidden_after"))),
        ("the AP's answer is shown, and the card then says what it carries",
         "carrying it" in (fw.get("msg") or "") and fw.get("carried_after") == "carrying hh-test (serial 7000) for ESP32 handhelds",
         (fw.get("msg"), fw.get("carried_after"))),
        ("the AP was asked to change the password twice - the wrong current, then the right - and nothing else",
         [(p[1].get("current"), p[1].get("password")) for p in posts("/api/password")] ==
         [("not the password", NEW_PASSWORD), (SETUP_PASSWORD, NEW_PASSWORD)] and ap.password == NEW_PASSWORD,
         len(posts("/api/password"))),
        # out and in
        ("logging out told the AP", len(posts("/api/logout")) == 1, len(posts("/api/logout"))),
        ("logging in again was tried with the old password and the new, and nothing else",
         [p[1] for p in posts("/api/login")] == [{"password": SETUP_PASSWORD}, {"password": NEW_PASSWORD}], len(posts("/api/login"))),
        ("after logging in again the page is on the tab it was on", r.get("tab_after_login") == "#settings", r.get("tab_after_login")),
        # across the whole visit
        ("every change sent to the AP carried the session and its CSRF token",
         all(p[3] and p[2] for p in ap.posts if p[0] not in ("/api/setup", "/api/login")), [p[0] for p in ap.posts if not (p[3] and p[2])]),
        ("the AP refused nothing but the wrong passwords, the GPS-held time and the ended session",
         [x for x in after_login if x[2] != 401] == [] and len([x for x in after_login if x[2] == 401]) <= 4, ap.refused),
        ("the visit finished in reasonable time", time.time() - started < 90, round(time.time() - started)),
    ]
    return checks


def judge_returning(ap):
    r = ap.result or {}
    b = r.get("before_login") or {}
    return common(ap) + [
        ("before login there are no tabs, no Log out and nothing of the grid", b.get("tabs") is False and b.get("logout") is False and b.get("cards") == 0,
         b),
        ("the grid's name and the serving AP are shown before login, and nothing else about it",
         b.get("title") == "Lakeside Trip" and b.get("subtitle") == "LocalGrid Access Point, served by AP MAIN", (b.get("title"), b.get("subtitle"))),
        ("the tab in the address is the one opened and marked", r.get("hash") == "#network" and r.get("selected") == ["network"] and r.get("overview_hidden") is True,
         (r.get("hash"), r.get("selected"), r.get("overview_hidden"))),
        ("the zone handhelds use is the one the AP holds", r.get("zone_hint_default") == "Handhelds show local time for Europe/Berlin (CET-1CEST,M3.5.0,M10.5.0/3).",
         r.get("zone_hint_default")),
        ("the password is not left in its field", r.get("password_field") == "", r.get("password_field")),
        ("a rule the AP worked out from the zone's name is shown as that, not as UTC",
         r.get("zone_hint_derived") == "Handhelds show local time for Europe/Berlin (CET-1CEST,M3.5.0,M10.5.0/3, "
                                       "worked out by this AP from the zone's name).", r.get("zone_hint_derived")),
        ("a zone the AP has no rule for says so, and what to press",
         r.get("zone_hint_no_rule") == "Handhelds show UTC: this AP does not know the rule for Europe/Berlin. "
                                       "Press \"Use this device's time\" once to send it.", r.get("zone_hint_no_rule")),
        ("the password went to the AP once", [p[1] for p in ap.posts if p[0] == "/api/login"] == [{"password": PASSWORD}], len(ap.posts)),
        ("nothing was changed on the AP", [p[0] for p in ap.posts] == ["/api/login"], [p[0] for p in ap.posts]),
    ]


def judge_unreachable(ap):
    r = ap.result or {}
    return common(ap) + [
        ("no form is offered for an AP that is not there", r.get("forms") is False, r.get("forms")),
        ("nothing was sent to it", ap.posts == [], ap.posts),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serve", action="store_true", help="keep the stand-in AP up and open nothing")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=90.0, help="seconds a visit may take")
    args = parser.parse_args()
    gw = load_samples()

    if args.serve:
        ap = StandInAP(gw)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(ap, page_with(None)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print(f"Stand-in AP at http://127.0.0.1:{server.server_address[1]}/  (password: {PASSWORD}). Ctrl+C to stop.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0

    browser = find_browser()
    if browser is None:
        print("test_admin_page: no Edge or Chrome found (set LG_BROWSER to one); nothing was tested")
        return 2
    print(f"admin page in {pathlib.Path(browser).name}, against a stand-in AP")
    total = failed = 0
    visits = [
        ("first run: an AP nobody has set up, then the whole dashboard", StandInAP(gw, configured=False), FIRST_RUN, "", None),
        ("returning: an AP already set up, opened at a tab's own address", StandInAP(gw), RETURNING, "#network", judge_returning),
        ("unreachable: an AP that does not answer", StandInAP(gw, reachable=False), UNREACHABLE, "", judge_unreachable),
    ]
    for title, ap, driver, fragment, judge in visits:
        print(f"\n{title}")
        started = time.time()
        if not visit(browser, ap, driver, fragment, args.timeout):
            print(f"  FAIL  the page did not finish within {args.timeout:.0f} s")
            total, failed = total + 1, failed + 1
            continue
        checks = judge(ap) if judge is not None else judge_first_run(ap, started)
        for name, ok, seen in checks:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"\n        saw: {seen!r}"))
        total += len(checks)
        failed += len([c for c in checks if not c[1]])
    print(f"\nADMIN PAGE: {'PASS' if not failed else 'FAIL'} ({total - failed} of {total} checks)")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
