#!/usr/bin/env python3
"""Put LocalGrid on a bench board through the installer page, the way a person would, and check it.

`site/flash/index.html` is the page somebody with a new board and no tools uses: it flashes a published
image over USB (ESP Web Tools) and then gives the board its place in a grid over Web Serial. Both steps
need a real browser, a real cable and the browser's own "choose a serial port" dialog, which is why
this was left to a person until 2026-10-06. This does the whole of it:

  1. builds the site with the published images (`tools/build_release.py` must have made them),
  2. opens the installer page in a real, visible Chrome or Edge window,
  3. presses the page's buttons with real mouse events (a script's `.click()` is not a user gesture,
     and Web Serial refuses to ask for a port without one),
  4. picks the board's port in the browser's chooser through Windows UI Automation,
  5. lets ESP Web Tools flash the image, without erasing the board,
  6. runs the page's set-up step with this bench's own grid keys, so the board stays in this grid,
  7. asks the board over its cable who it is, what it runs and whether it is back on the grid.

  python tools/build_release.py --out build/release --version 2026.10.6 --only handheld
  python tools/test_installer_page.py e28                    # a handheld from tools/bench_devices.json
  python tools/test_installer_page.py e28 --release bench.pub.hex   # and give it a release key again

Windows only (the chooser is driven through UI Automation), system Python with `websocket-client` and
`pyserial`. The grid file it makes holds this grid's passphrase and backbone key: it is written to a
temporary folder, given to the page as a person would give theirs, and deleted. Nothing in it is printed.

The installer writes the whole image from address 0, which clears the board's settings; its identity
is in a partition of its own and is kept. Afterwards the board runs the published image, not the bench
build: flash it again with `tools/flash.py` before a chaos run.
"""
import argparse
import http.server
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEVICES = ROOT / "tools" / "bench_devices.json"
SECRETS = ROOT / "firmware" / "common" / "lg_secrets.h"
BROWSERS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]
CDP_PORT = 9334

# The chooser, by UI Automation: it is browser furniture, not part of the page, so nothing in the page
# can reach it. Finds the pane that says a site "wants to connect to a serial port", selects the row
# ending in "(COMn)" and presses Connect.
CHOOSER_PS = r'''
param([string]$Port)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$auto = [System.Windows.Automation.AutomationElement]
$tree = [System.Windows.Automation.TreeScope]
$ctl  = [System.Windows.Automation.ControlType]
function ByType($el, $type) { $el.FindAll($tree::Descendants, (New-Object System.Windows.Automation.PropertyCondition($auto::ControlTypeProperty, $type))) }
$deadline = (Get-Date).AddSeconds(20)
$dialog = $null
while ((Get-Date) -lt $deadline -and $null -eq $dialog) {
    foreach ($w in $auto::RootElement.FindAll($tree::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
        try {
            if ($w.Current.ClassName -notlike "Chrome_WidgetWin*") { continue }
            foreach ($p in (ByType $w $ctl::Pane)) {
                if ($p.Current.Name -like "*wants to connect to a serial port*") { $dialog = $p; break }
            }
        } catch {}
        if ($null -ne $dialog) { break }
    }
    if ($null -eq $dialog) { Start-Sleep -Milliseconds 400 }
}
if ($null -eq $dialog) { "CHOOSER: not found"; exit 2 }
function Press($name) {
    foreach ($b in (ByType $dialog $ctl::Button)) {
        if ($b.Current.Name -eq $name) { $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); return $true }
    }
    return $false
}
$want = $null
$names = @()
$until = (Get-Date).AddSeconds(6)   # the list fills a moment after the dialog appears
while ((Get-Date) -lt $until -and $null -eq $want) {
    $names = @()
    foreach ($i in (ByType $dialog $ctl::DataItem)) {
        $names += $i.Current.Name
        if ($i.Current.Name -like "*($Port)*") { $want = $i }
    }
    if ($null -eq $want) { Start-Sleep -Milliseconds 400 }
}
"CHOOSER: offers " + $names.Count + " port(s)"
if ($null -eq $want) { [void](Press "Cancel"); "CHOOSER: $Port is not offered; rows: " + (($names | Where-Object { $_ }) -join " | "); exit 4 }
# Which row ends up chosen is the whole question: Connect opens *that* port. UI Automation cannot be
# trusted to say: asked to select a row, the list selects its first row and reports so; after arrow keys
# it reports nearly every row as selected (both seen 2026-10-06, the first by flashing an access point
# with handheld firmware). So the row is reached with the keyboard - focus the first, then one Down
# arrow per row above it - and the page itself proves the result before anything is written: see
# GUARD_JS, which opens whatever port was chosen and asks the board its ID.
Add-Type -Namespace LG -Name Keys -MemberDefinition @"
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
"@
$rows = @(); foreach ($i in (ByType $dialog $ctl::DataItem)) { if ($i.Current.Name) { $rows += $i } }
"CHOOSER: rows " + (($rows | ForEach-Object { $_.Current.Name -replace '^.*\((COM\d+)\).*$', '$1' }) -join " ")
$index = -1
for ($k = 0; $k -lt $rows.Count; $k++) { if ($rows[$k].Current.Name -like "*($Port)*") { $index = $k } }
if ($index -lt 0) { [void](Press "Cancel"); "CHOOSER: $Port is not offered"; exit 4 }
try { $rows[0].SetFocus() } catch { [void](Press "Cancel"); "CHOOSER: the list could not be focused"; exit 5 }
Start-Sleep -Milliseconds 400
for ($k = 0; $k -lt $index; $k++) {
    [LG.Keys]::keybd_event(0x28, 0, 0, [System.UIntPtr]::Zero)
    [LG.Keys]::keybd_event(0x28, 0, 2, [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 400
if (-not (Press "Connect")) { "CHOOSER: no Connect button"; exit 3 }
"CHOOSER: row $($index + 1) of $($rows.Count) chosen with the keyboard; the page checks which board it is"
'''

# Everything on the page that can be pressed, through every shadow root: ESP Web Tools draws its
# dialog inside web components, where document.querySelector does not see.
DEEP_JS = r'''
(() => {
  const out = [];
  const walk = root => {
    for (const el of root.querySelectorAll("*")) {
      const tag = el.tagName.toLowerCase();
      if (/button|list-item|checkbox/.test(tag) || tag === "a") {
        const r = el.getBoundingClientRect();
        if (r.width > 0 && r.height > 0) {
          out.push({tag, text: (el.getAttribute("label") || el.textContent || "").replace(/\s+/g, " ").trim(),
                    x: r.x + r.width / 2, y: r.y + r.height / 2, checked: !!el.checked,
                    disabled: !!el.disabled || el.hasAttribute("disabled")});
        }
      }
      if (el.shadowRoot) walk(el.shadowRoot);
    }
  };
  walk(document);
  const dlg = document.querySelector("ewt-install-dialog");
  const text = root => { let t = ""; for (const n of root.childNodes) { if (n.nodeType === 3) t += n.textContent + " ";
      else if (n.nodeType === 1) { if (n.tagName !== "STYLE" && n.tagName !== "SCRIPT") t += text(n) + " "; if (n.shadowRoot) t += text(n.shadowRoot) + " "; } } return t; };
  return {open: !!dlg, text: dlg ? text(dlg.shadowRoot || dlg).replace(/\s+/g, " ").trim() : "", controls: out};
})()
'''


# Before ESP Web Tools or the page may use the port that was chosen, it is opened here and the board is
# asked who it is; anything but the expected ID and the port is refused, so nothing is ever written to
# the wrong board, whatever the chooser did. Wraps navigator.serial.requestPort in the page.
GUARD_JS = r"""
(() => {
  if (window.__lgGuard) return "already";
  const original = navigator.serial.requestPort.bind(navigator.serial);
  window.__lgGuardLog = [];
  navigator.serial.requestPort = async (...args) => {
    const port = await original(...args);
    await port.open({baudRate: 115200});
    try { await port.setSignals({dataTerminalReady: false, requestToSend: false}); } catch (e) {}
    const reader = port.readable.getReader(), writer = port.writable.getWriter();
    let text = "";
    try {
      await writer.write(new TextEncoder().encode("\r\nid\r\n"));
      const end = Date.now() + 5000;
      while (Date.now() < end && !/LGID: \S+/.test(text)) {
        const got = await Promise.race([reader.read(), new Promise(r => setTimeout(() => r(null), 250))]);
        if (got && got.value) text += new TextDecoder().decode(got.value);
      }
    } finally {
      try { await reader.cancel(); } catch (e) {}
      reader.releaseLock(); writer.releaseLock();
      await port.close();
    }
    const m = text.match(/LGID: (\S+)/);
    window.__lgGuardLog.push(m ? m[1] : "no answer");
    if (!m || m[1] !== window.__lgExpect) throw new Error("the chosen port is " + (m ? m[1] : "a board that did not answer") + ", not the board under test; refused");
    return port;
  };
  window.__lgGuard = true;
  return "installed";
})()
"""


class Page:
    """One browser tab over the DevTools protocol."""

    def __init__(self, ws_url):
        import websocket
        self.ws = websocket.create_connection(ws_url, timeout=30, suppress_origin=True)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == self.n:
                if "error" in msg:
                    raise RuntimeError(method + ": " + json.dumps(msg["error"]))
                return msg.get("result", {})

    def js(self, expression):
        r = self.call("Runtime.evaluate", expression=expression, returnByValue=True, awaitPromise=True)
        if "exceptionDetails" in r:
            raise RuntimeError(json.dumps(r["exceptionDetails"])[:300])
        return r["result"].get("value")

    def press_at(self, x, y):
        """A real mouse press: the only kind Web Serial accepts as somebody asking."""
        for kind in ("mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=kind, x=x, y=y, button="left", clickCount=1)

    def press(self, selector):
        box = self.js("(()=>{const e=document.querySelector(%s);e.scrollIntoView({block:'center'});"
                      "const r=e.getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2];})()" % json.dumps(selector))
        self.press_at(box[0], box[1])

    def set_file(self, selector, path):
        doc = self.call("DOM.getDocument")
        node = self.call("DOM.querySelector", nodeId=doc["root"]["nodeId"], selector=selector)
        self.call("DOM.setFileInputFiles", nodeId=node["nodeId"], files=[str(path)])


def pick_port(port):
    proc = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                           "& { " + CHOOSER_PS + " } -Port " + port], capture_output=True, text=True, timeout=90)
    lines = proc.stdout.strip().splitlines() or [proc.stderr.strip()[:200]]
    return proc.returncode == 0, " / ".join(lines[-2:])


def grid_file(folder, release_hex):
    """This bench's grid, as the file the page asks for. Holds the grid's secrets: never printed, and deleted."""
    text = SECRETS.read_text(encoding="utf-8")
    m = re.search(r'#define\s+LG_SECRET_WIFI_PASSPHRASE\s+"([^"]*)"', text)
    k = re.search(r"#define\s+LG_SECRET_BACKBONE_KEY\s*\{([^}]*)\}", text)
    if not m or not k:
        sys.exit("test_installer_page: firmware/common/lg_secrets.h has no passphrase or backbone key")
    key = bytes(int(x.strip(), 0) for x in k.group(1).split(",") if x.strip())
    if len(key) != 32 or not any(key):
        sys.exit("test_installer_page: the backbone key in lg_secrets.h is not a real 32-byte key")
    path = pathlib.Path(folder) / "bench.lggrid.json"
    path.write_text(json.dumps({"localgrid_grid": 1, "name": "Bench", "passphrase": m.group(1), "backbone": key.hex(),
                                "release": release_hex}), encoding="utf-8")
    return path


def ask(port, command, marker, wait_s=6.0):
    """One console command over the cable, without resetting the board. The line that starts with marker."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False
    s.open()
    try:
        s.reset_input_buffer()
        s.write(("\r\n" + command + "\r\n").encode())
        text, end = b"", time.time() + wait_s
        while time.time() < end:
            text += s.read(4096)
            for line in text.decode("utf-8", "replace").splitlines()[:-1]:
                if marker in line:
                    return line.strip()
        return ""
    finally:
        s.dtr = s.rts = False
        s.close()


ELF_SHA = re.compile(r"ELF file SHA256:\s+([0-9a-f]{6,})")


def running_build(port):
    """Restarts the board and reads which build it says it is running (its boot banner)."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False
    s.open()
    try:
        s.write(b"\r\nreboot\r\n")          # a handheld; an access point has no such command
        time.sleep(0.3)
        s.rts = True                         # and EN through the bridge, for a board that has one
        time.sleep(0.2)
        s.rts = False
        text, end = b"", time.time() + 10
        while time.time() < end:
            text += s.read(4096)
            m = ELF_SHA.search(text.decode("utf-8", "replace"))
            if m:
                return m.group(1)
        return ""
    finally:
        s.dtr = s.rts = False
        s.close()


def elf_sha(path):
    import hashlib
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() if pathlib.Path(path).exists() else ""


def serve(folder):
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(folder), **k)

        def log_message(self, *a):
            pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    # The installer page's own words include an emoji; a console or pipe in cp1252 cannot print it.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help="a handheld or access point from tools/bench_devices.json")
    ap.add_argument("--firmware", default=str(ROOT / "build" / "release"),
                    help="published images and manifests from tools/build_release.py (default build/release)")
    ap.add_argument("--release", help="a release public key file (hex) to give the board, as the page's field takes it")
    ap.add_argument("--timeout", type=int, default=900, help="seconds allowed for the flash (default 900)")
    args = ap.parse_args()
    if sys.platform != "win32":
        sys.exit("test_installer_page: the port chooser is driven through Windows UI Automation; Windows only")

    bench = json.loads(DEVICES.read_text(encoding="utf-8"))
    d = next((x for x in bench["devices"] if x.get("name") == args.board), None)
    if d is None or not d.get("port") or not d.get("id"):
        sys.exit(f"test_installer_page: {args.board} is not a board with a port and an ID in {DEVICES.name}")
    role, code = d["id"].split("-")[1], d["id"].split("-")[2]
    target = bench["boards"][d["board"]]["target"]
    kind = ("node-" if role == "N" else "handheld-") + target
    if not (pathlib.Path(args.firmware) / f"manifest-{kind}.json").exists():
        sys.exit(f"test_installer_page: no manifest-{kind}.json in {args.firmware}; run tools/build_release.py first")
    browser = next((b for b in BROWSERS if pathlib.Path(b).exists()), None)
    if browser is None:
        sys.exit("test_installer_page: no Chrome or Edge found")
    release_hex = pathlib.Path(args.release).read_text(encoding="utf-8").strip() if args.release else ""

    checks = []

    def check(name, ok, got=None):
        checks.append((name, bool(ok)))
        print(("  ok    " if ok else "  FAIL  ") + name + ("" if ok or got is None else f"   [{str(got)[:160]}]"), flush=True)
        return bool(ok)

    before = ask(d["port"], "id", "LGID")
    check("the board answers on its cable before anything is done", d["id"] in before, before)
    project = "node" if role == "N" else "handheld"
    published = elf_sha(ROOT / "firmware" / project / f"build-release-{target}" / f"lg_{project}.elf")
    was = running_build(d["port"])
    if not check("the board says which build it runs", bool(was) and bool(published), (was, published[:16])):
        return 1
    if published.startswith(was):
        print("        (it already runs the published image; the install writes it again)", flush=True)
    others = {x["port"]: x["id"] for x in bench["devices"] if x.get("port") and x is not d}

    work = tempfile.mkdtemp(prefix="lg-installer-")
    site = pathlib.Path(work) / "site"
    made = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_site.py"), "--out", str(site), "--firmware", args.firmware],
                          capture_output=True, text=True)
    if made.returncode != 0:
        shutil.rmtree(work, ignore_errors=True)
        sys.exit("test_installer_page: tools/gen_site.py failed:\n" + made.stdout[-400:] + made.stderr[-400:])
    server = serve(site)
    url = f"http://127.0.0.1:{server.server_address[1]}/flash/"
    proc = subprocess.Popen([browser, f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={work}\\profile",
                             "--no-first-run", "--no-default-browser-check", "--disable-extensions", "--disable-sync",
                             "--window-size=1000,950", "--remote-allow-origins=*", url],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        tab = None
        for _ in range(60):
            time.sleep(0.5)
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json", timeout=2))
                tab = next((t for t in tabs if t.get("type") == "page" and "/flash" in t.get("url", "")), None)
                if tab:
                    break
            except OSError:
                pass
        if not check("the installer page opens in a real browser window", tab is not None):
            return 1
        page = Page(tab["webSocketDebuggerUrl"])
        page.call("Runtime.enable")
        page.call("DOM.enable")
        time.sleep(2.5)
        check("the browser has Web Serial and the page says nothing against it",
              page.js("'serial' in navigator && document.getElementById('noserial').classList.contains('hide')"))
        check(f"the page offers an image for this kind of board ({kind})",
              page.js("Array.from(document.querySelectorAll('#kind option'),o=>o.value).includes(%s)" % json.dumps(kind)))
        page.js("(()=>{const k=document.getElementById('kind');k.value=%s;k.onchange();})()" % json.dumps(kind))
        time.sleep(1.0)
        check("this board is in the list for that kind", page.js(
            "Array.from(document.querySelectorAll('#board option'),o=>o.value).includes(%s)" % json.dumps(code)), code)
        page.js("document.getElementById('board').value=%s;document.getElementById('index').value=%d;1"
                % (json.dumps(code), int(d.get("device_index") or d.get("index") or 1)))
        check("the install button has been drawn by ESP Web Tools",
              page.js("!!customElements.get('esp-web-install-button') && !!document.querySelector('esp-web-install-button')"))

        # ---- step 1: flash ----
        page.js("window.__lgExpect = %s; 1" % json.dumps(d["id"]))
        check("the page will refuse any port that is not this board's", page.js(GUARD_JS) == "installed")
        page.press("esp-web-install-button")
        ok, said = pick_port(d["port"])
        check("the browser asked which serial port, and a row was chosen", ok, said)
        if not ok:
            return 1
        time.sleep(6)
        guard = page.js("window.__lgGuardLog") or []
        if not check("the port chosen is this board's own: it answered with its ID before anything was written",
                     guard[-1:] == [d["id"]], guard):
            return 1
        flashed, started, pressed, seen = False, time.time(), set(), ""
        while time.time() - started < args.timeout:
            time.sleep(1.5)
            st = page.js(DEEP_JS)
            text = st["text"]
            if text and text != seen:
                seen = text
                print("        installer: " + text[:110], flush=True)
            if re.search(r"installation complete|installed", text, re.I) and "installing" not in text.lower():
                flashed = True
                break
            if re.search(r"fail|could not|error", text, re.I) and "erase" not in text.lower():
                break
            ctl = [c for c in st["controls"] if not c["disabled"]]
            # never erase: the board keeps its identity and whatever it had
            box = next((c for c in ctl if "checkbox" in c["tag"] and c["checked"]), None)
            if box is not None:
                page.press_at(box["x"], box["y"])
                continue
            for want in (r"^install\b", r"^next$", r"^continue$"):
                hit = next((c for c in ctl if re.search(want, c["text"], re.I) and c["tag"] != "esp-web-install-button"
                            and (want, c["text"], text[:40]) not in pressed), None)
                if hit is not None:
                    pressed.add((want, hit["text"], text[:40]))
                    page.press_at(hit["x"], hit["y"])
                    break
        check("ESP Web Tools wrote the image and said the installation was complete", flashed, seen[-200:])
        if not flashed:
            return 1
        # leave the dialog: its Next, then whatever closes it, so the port is let go
        for _ in range(6):
            st = page.js(DEEP_JS)
            if not st["open"]:
                break
            hit = next((c for c in st["controls"] if re.search(r"^(next|close|done)$", c["text"], re.I)), None)
            if hit is not None:
                page.press_at(hit["x"], hit["y"])
            else:
                page.call("Input.dispatchKeyEvent", type="keyDown", key="Escape", code="Escape", windowsVirtualKeyCode=27)
                page.call("Input.dispatchKeyEvent", type="keyUp", key="Escape", code="Escape", windowsVirtualKeyCode=27)
            time.sleep(1.5)
        check("the installer's dialog closes", not page.js(DEEP_JS)["open"])
        time.sleep(6)   # the board restarts into what was written

        # ---- steps 2 and 3: the grid file, then set up over the same cable ----
        grid = grid_file(work, release_hex)
        page.set_file("#gridfile", grid)
        time.sleep(1.0)
        info = page.js("document.getElementById('gridinfo').textContent") or ""
        check("the page takes the grid file", "ready" in info and "from your file" in info, info)
        check("Connect and set up is offered", page.js("!document.getElementById('setup').disabled"))
        guarded = len(page.js("window.__lgGuardLog") or [])
        page.press("#setup")
        ok, said = pick_port(d["port"])
        check("the browser asked for the port again, and a row was chosen", ok, said)
        time.sleep(6)
        guard = page.js("window.__lgGuardLog") or []
        if not check("again it is this board's own port", len(guard) > guarded and guard[-1] == d["id"], guard):
            return 1
        log, end = "", time.time() + 60
        while time.time() < end:
            time.sleep(1.0)
            log = page.js("document.getElementById('log').textContent") or ""
            if "Done." in log or "Not set up" in log:
                break
        check("the board told the page who it is", "LGID" in log and d["id"] in log, log[:200])
        check("the board kept the identity it had", "already has an identity" in log, log[:200])
        check("the board took the grid keys, and the page said it was done", "LGPROV" in log and "Done." in log, log[-160:])
        check("the page's own script raised nothing", "Not set up" not in log, log[-160:])
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        server.shutdown()
        time.sleep(1.5)
        shutil.rmtree(work, ignore_errors=True)   # the grid file with it

    # ---- the board itself, over its cable ----
    time.sleep(12)
    now = running_build(d["port"])
    check("this board, and not another, now runs the published image (its boot banner names that build)",
          now and published.startswith(now), (now, published[:16]))
    time.sleep(8)
    for port, ident in others.items():   # nothing else was touched
        if ident and ask(port, "id", "LGID") and ident not in ask(port, "id", "LGID"):
            check(f"the board on {port} is still the one it was", False, ident)
    after = ask(d["port"], "id", "LGID")
    check("after all of it the board still answers with its own ID", d["id"] in after, after)
    version = ask(d["port"], "update", "LGFWU:")
    check("it is running from the first firmware slot, as a fresh install does", "slot ota_0" in version, version)
    if release_hex:
        check("it was given the release key", "no release key" not in version, version)
    online, end = "", time.time() + 90
    while time.time() < end and "ONLINE" not in online:
        time.sleep(5)
        online = ask(d["port"], "status", "link:") if role != "N" else ask(d["port"], "status", "Links")
    check("it is back on this grid" if role != "N" else "it has its backbone links again",
          "ONLINE" in online if role != "N" else bool(re.search(r"Links [1-9]", online)), online)

    good = sum(1 for _, ok in checks if ok)
    print(f"INSTALLER PAGE: {'PASS' if good == len(checks) else 'FAIL'} ({good} of {len(checks)} checks)")
    return 0 if good == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
