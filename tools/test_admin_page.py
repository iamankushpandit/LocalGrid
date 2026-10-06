#!/usr/bin/env python3
"""Tests the access point's admin page in a real browser, against a stand-in access point.

The admin page (firmware/node/main/web/admin.html) is served by an AP over the grid's own Wi-Fi, which
the development PC cannot join without losing its Internet connection. But the page is a web page and
the AP is, to it, a handful of HTTP answers. This tool is those answers: it serves the page exactly as
it is in the tree, answers /api/* with the same JSON and the same packed bytes an AP sends (the sample
replies tools/grid_watch.py already checks its own decoders against), and drives the page in a headless
Edge or Chrome: log in with the wrong password, then the right one, open the Map tab and read the trail
drawn there, open the Network tab and change the LoRa power. What the page showed and what it sent back
are checked.

  python tools/test_admin_page.py            run the checks; exit 0 when all pass
  python tools/test_admin_page.py --serve    keep the stand-in AP up, to look at the page by hand
                                             (password: test-password)

No board is touched and nothing leaves this machine: the browser is started with every host but
127.0.0.1 unresolvable, so the map's tiles fail exactly as they do with no Internet, and the page's
"no map tiles" note is one of the things checked.

What this does not prove: that a real AP's web server delivers the page. It proves the page.
"""
import argparse
import http.server
import importlib.util
import json
import os
import pathlib
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "firmware" / "node" / "main" / "web" / "admin.html"
PASSWORD = "test-password"
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


# Runs first in the page: anything the page's own script throws is kept for the report.
CAPTURE = """<script>
window.__errors = [];
window.addEventListener("error", (e) => window.__errors.push(String(e.message || e)));
window.addEventListener("unhandledrejection", (e) => window.__errors.push("rejected: " + String(e.reason)));
</script>"""

# Runs last: does what a person would, and reports what the page then showed.
DRIVER = """<script>
(async () => {
  const $ = (id) => document.getElementById(id);
  const shown = (el) => !!el && el.getClientRects().length > 0;
  const until = async (test, ms = 8000) => {
    const end = Date.now() + ms;
    while (Date.now() < end) { try { if (test()) return true; } catch (e) {} await new Promise((r) => setTimeout(r, 50)); }
    return false;
  };
  const out = { steps: [] };
  const step = (name, value) => out.steps.push([name, value]);
  try {
    step("login form shown before anything else", await until(() => shown($("login-form"))));
    step("nothing of the grid is on screen before login", !shown($("location-card")) && !shown($("lora-card")));

    $("login-password").value = "not the password";
    $("login-form").requestSubmit();
    step("a wrong password is answered with the AP's own words",
         await until(() => /wrong|incorrect|not right/i.test($("login-msg").textContent)) && $("login-msg").textContent);

    $("login-password").value = "__PASSWORD__";
    $("login-form").requestSubmit();
    step("the right password opens the dashboard", await until(() => !shown($("login-form")) && shown($("tabs") || document.querySelector('[role="tab"]'))));
    out.title = $("title").textContent;
    out.password_field_after_login = $("login-password").value;

    document.querySelector('[role="tab"][data-tab="map"]').click();
    step("the Map tab draws the trail", await until(() => document.querySelectorAll(".loc-path").length > 0));
    out.paths = document.querySelectorAll(".loc-path").length;
    out.step_dots = document.querySelectorAll(".loc-step").length;
    out.pins = document.querySelectorAll(".loc-pin").length;
    out.path_widths = [...document.querySelectorAll(".loc-path")].map((p) => parseFloat(p.style.width));
    out.dot_titles = [...document.querySelectorAll(".loc-step")].map((d) => d.title);
    out.offline_note_shown = shown($("loc-offline"));
    const trails = $("loc-trails");
    out.trail_heading = (trails.querySelector("h2") || {}).textContent || "";
    out.trail_rows = [...trails.querySelectorAll("tbody tr")].map((tr) => ({
      cells: [...tr.children].map((td) => td.textContent.trim()),
      links: [...tr.querySelectorAll("a")].map((a) => [a.textContent, a.href, a.target, a.rel]),
    }));
    out.position_rows = [...$("loc-body").querySelectorAll("tr")].map((tr) => [...tr.children].map((td) => td.textContent.trim()));

    document.querySelector('[role="tab"][data-tab="network"]').click();
    step("the Network tab shows the LoRa power", await until(() => shown($("lora-power")) && /dBm/.test($("lora-power-now").textContent)));
    out.power_before = $("lora-power-now").textContent;
    out.power_explained = $("lora-power").querySelector("p").textContent.replace(/\\s+/g, " ").trim();
    out.power_marked_before = [...document.querySelectorAll("#lora-power button")].filter((b) => !b.classList.contains("ghost")).map((b) => b.dataset.power);
    document.querySelector('#lora-power button[data-power="low"]').click();
    step("choosing Low is taken by the AP and shown back", await until(() => /low, 5 dBm/.test($("lora-power-now").textContent)));
    out.power_after = $("lora-power-now").textContent;
    out.power_msg = $("lora-power-msg").textContent;
    out.power_marked_after = [...document.querySelectorAll("#lora-power button")].filter((b) => !b.classList.contains("ghost")).map((b) => b.dataset.power);
  } catch (e) {
    out.driver_error = String(e && e.stack || e);
  }
  out.errors = window.__errors;
  await fetch("/test-result", { method: "POST", body: JSON.stringify(out) });
})();
</script>"""


class StandInAP:
    """What an AP answers, and a record of what it was asked."""

    def __init__(self, gw):
        self.gw = gw
        self.token = None
        self.csrf = None
        self.power = 22
        self.posts = []          # (path, body, csrf header, had the session cookie)
        self.refused = []        # requests answered 401 or 403
        self.result = None
        self.done = threading.Event()

    def status(self):
        s = json.loads(self.gw.sample_status_json().decode("utf-8"))
        s["lora"]["power"] = self.power
        return json.dumps(s).encode("utf-8")


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

        def _error(self, status, text):
            ap.refused.append((self.command, self.path, status))
            self._send(json.dumps({"error": text}).encode(), status=status)

        def _session(self):
            return ap.token is not None and f"lg_session={ap.token}" in (self.headers.get("Cookie") or "")

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/":
                return self._send(page_bytes, "text/html; charset=utf-8")
            if path == "/favicon.svg":
                return self._send(b"<svg xmlns='http://www.w3.org/2000/svg'/>", "image/svg+xml")
            if path == "/api/state":
                on = self._session()
                return self._send(json.dumps({"configured": True, "logged_in": on, "csrf": ap.csrf if on else "",
                                              "grid_name": "Lakeside Trip", "ap": 0, "ap_name": "MAIN"}).encode())
            if not self._session():
                return self._error(401, "Log in to see grid status.")
            if path == "/api/status":
                return self._send(ap.status())
            if path == "/api/history":
                return self._send(ap.gw.sample_history_blob(), "application/octet-stream")
            if path == "/api/trails":
                return self._send(ap.gw.sample_trails_blob(), "application/octet-stream")
            if path == "/api/chaos":
                return self._send(b'{"available":false,"armed":false,"window_s":0,"faults":0,"max_faults":0,'
                                  b'"journal":0,"journal_full":false}')
            self._error(404, "No such page.")

        def do_POST(self):
            path = self.path.split("?")[0]
            raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if path == "/test-result":
                ap.result = json.loads(raw.decode("utf-8"))
                ap.done.set()
                return self._send(b"{}")
            try:
                body = json.loads(raw.decode("utf-8") or "{}")
            except ValueError:
                return self._error(400, "A request body is required.")
            ap.posts.append((path, body, self.headers.get("X-CSRF"), self._session()))
            if path == "/api/login":
                if body.get("password") != PASSWORD:
                    return self._error(401, "Wrong password.")
                ap.token, ap.csrf = secrets.token_hex(16), secrets.token_hex(16)
                return self._send(json.dumps({"ok": True, "csrf": ap.csrf}).encode(),
                                  cookie=f"lg_session={ap.token}; HttpOnly; SameSite=Strict; Path=/")
            if not self._session():
                return self._error(401, "Log in first.")
            if self.headers.get("X-CSRF") != ap.csrf:
                return self._error(403, "Request blocked. Reload the page and try again.")
            if path == "/api/lora":
                level = {"full": 22, "medium": 14, "low": 5}.get(body.get("power"))
                if level is None:
                    return self._error(400, "The power is full, medium or low.")
                ap.power = level
                return self._send(json.dumps({"ok": True, "power": level}).encode())
            if path == "/api/logout":
                ap.token = ap.csrf = None
                return self._send(b"{}")
            self._error(404, "No such page.")

    return Handler


def page_with_driver(drive):
    html = PAGE.read_text(encoding="utf-8")
    if not drive:
        return html.encode("utf-8")
    assert "<head>" in html and "</body>" in html, "admin.html has changed shape: no <head> or </body>"
    html = html.replace("<head>", "<head>" + CAPTURE, 1)
    html = html.replace("</body>", DRIVER.replace("__PASSWORD__", PASSWORD) + "</body>", 1)
    return html.encode("utf-8")


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


def judge(ap):
    """Every claim checked, as (what, passed, what was seen)."""
    r = ap.result or {}
    rows = r.get("trail_rows") or []
    first = rows[0] if rows else {"cells": [], "links": []}
    last = rows[-1] if rows else {"cells": [], "links": []}
    lora_posts = [p for p in ap.posts if p[0] == "/api/lora"]
    login_posts = [p for p in ap.posts if p[0] == "/api/login"]
    checks = [(name, bool(value), value) for name, value in r.get("steps", [])]
    checks += [
        ("the page's own script raised no error", r.get("errors") == [] and "driver_error" not in r,
         r.get("errors") or r.get("driver_error")),
        ("the grid's name is the page's title", r.get("title") == "Lakeside Trip", r.get("title")),
        ("the password is not left in its field", r.get("password_field_after_login") == "", r.get("password_field_after_login")),
        ("the password went to the AP twice, and nowhere else: once wrong, once right",
         [p[1].get("password") for p in login_posts] == ["not the password", PASSWORD] and
         all(set(p[1]) == {"password"} for p in login_posts), len(login_posts)),
        ("a trail of three steps is two lines and two dots, the newest being the person's pin",
         r.get("paths") == 2 and r.get("step_dots") == 2 and (r.get("pins") or 0) >= 2,
         (r.get("paths"), r.get("step_dots"), r.get("pins"))),
        ("each line has a length: the steps are apart on the map, not on one spot",
         all(w and w > 2 for w in r.get("path_widths") or [0]), r.get("path_widths")),
        ("each dot says whose step it is and which",
         [t.split(",")[1].strip() for t in r.get("dot_titles") or []] == ["step 2 of 3", "step 1 of 3"] and
         all(t.startswith("Priya") for t in r.get("dot_titles") or []), r.get("dot_titles")),
        ("with no Internet the page says why there is no map behind the pins", r.get("offline_note_shown") is True,
         r.get("offline_note_shown")),
        ("the trail is headed with the person's name and how many steps", r.get("trail_heading") == "Priya's last 3 steps",
         r.get("trail_heading")),
        ("every step is listed, newest first, numbered, the ends named",
         len(rows) == 3 and first["cells"][0].startswith("3") and "last" in first["cells"][0] and
         last["cells"][0].startswith("1") and "first" in last["cells"][0], [x["cells"][0] for x in rows]),
        ("each step shows its coordinates", [x["cells"][2] for x in rows] ==
         ["47.62510, 13.04890", "47.62501, 13.04878", "47.62492, 13.04870"], [x["cells"][2] for x in rows]),
        ("each step opens in OpenStreetMap at that spot, in a new tab",
         len(first["links"]) == 2 and first["links"][0][0] == "OpenStreetMap" and
         first["links"][0][1] == "https://www.openstreetmap.org/?mlat=47.625100&mlon=13.048900#map=19/47.625100/13.048900"
         and first["links"][0][2] == "_blank" and "noopener" in first["links"][0][3], first["links"][:1]),
        ("... and in Google Maps",
         len(first["links"]) == 2 and first["links"][1][0] == "Google Maps" and
         first["links"][1][1] == "https://www.google.com/maps/search/?api=1&query=47.625100,13.048900", first["links"][1:]),
        ("the oldest step's link is its own spot, not the newest's",
         len(last["links"]) == 2 and "mlat=47.624920&mlon=13.048700" in last["links"][0][1], last["links"][:1]),
        ("the positions table still lists everyone", len(r.get("position_rows") or []) == 2 and
         {x[0].split()[0] for x in r.get("position_rows")} == {"MAIN", "Priya"}, r.get("position_rows")),
        ("the LoRa power in use is shown, with the AP it belongs to",
         r.get("power_before") == "In use on MAIN: full, 22 dBm." and r.get("power_marked_before") == ["full"],
         (r.get("power_before"), r.get("power_marked_before"))),
        ("the page says what the power is for", "close together" in (r.get("power_explained") or "") and
         "Full reaches furthest" in (r.get("power_explained") or ""), r.get("power_explained")),
        ("choosing Low sent exactly that, once, with the session and its CSRF token",
         len(lora_posts) == 1 and lora_posts[0][1] == {"power": "low"} and lora_posts[0][2] == ap.csrf and lora_posts[0][3],
         [(p[1], p[2] == ap.csrf, p[3]) for p in lora_posts]),
        ("the AP's new power is what the page then shows",
         r.get("power_after") == "In use on MAIN: low, 5 dBm." and r.get("power_marked_after") == ["low"] and ap.power == 5,
         (r.get("power_after"), r.get("power_marked_after"), ap.power)),
        ("... and it says the change was made and is kept", "now low" in (r.get("power_msg") or "") and
         "kept across restarts" in (r.get("power_msg") or ""), r.get("power_msg")),
        ("after login the AP refused nothing the page asked for",
         [x for x in ap.refused if x[1] != "/api/login"] == [], ap.refused),
    ]
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serve", action="store_true", help="keep the stand-in AP up and open nothing")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    ap = StandInAP(load_samples())
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(ap, page_with_driver(not args.serve)))
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    threading.Thread(target=server.serve_forever, daemon=True).start()
    if args.serve:
        print(f"Stand-in AP at {url}  (password: {PASSWORD}). Ctrl+C to stop.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0

    browser = find_browser()
    if browser is None:
        print("test_admin_page: no Edge or Chrome found (set LG_BROWSER to one); nothing was tested")
        return 2
    profile = tempfile.mkdtemp(prefix="lg-admin-test-")
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                             "--disable-extensions", f"--user-data-dir={profile}", "--window-size=1100,1500",
                             "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1", url],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        finished = ap.done.wait(args.timeout)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
        server.shutdown()
    if not finished:
        print(f"test_admin_page: the page did not finish within {args.timeout:.0f} s ({pathlib.Path(browser).name})")
        return 1
    checks = judge(ap)
    failed = [c for c in checks if not c[1]]
    print(f"admin page in {pathlib.Path(browser).name}, against a stand-in AP:")
    for name, ok, seen in checks:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"\n        saw: {seen!r}"))
    print(f"\nADMIN PAGE: {'PASS' if not failed else 'FAIL'} ({len(checks) - len(failed)} of {len(checks)} checks)")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
