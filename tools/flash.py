#!/usr/bin/env python3
"""Build and flash LocalGrid firmware to bench boards, one or many, and manage device IDs.

Boards, board types, and assigned firmware live in tools/bench_devices.json.
Run inside the ESP-IDF environment (PowerShell: . C:\\esp\\v6.1\\esp-idf\\export.ps1).

Examples:
  python tools/flash.py --list
  python tools/flash.py node-north                 # one board, keeps its settings and ID
  python tools/flash.py --all                      # every board
  python tools/flash.py --role N                   # all infrastructure nodes, one at a time
  python tools/flash.py hosyond --erase            # wipe everything, reflash, keep the same ID
  python tools/flash.py hosyond --firmware tests   # different firmware for this run
  python tools/flash.py --identify                 # ask every board for its ID
  python tools/flash.py --decode LG-N-ELG-7K3QX9PA2M

Device IDs:
  LG-<role>-<board>-<10 base32 chars>. Role and board decode from the device map.
  The tail is a hash of the MAC, provisioning time, a text label, and random bytes.
  The MAC is read only in memory while minting an ID; it is never printed or saved.

Modes:
  default   flash the app; the board keeps its saved settings and its ID
  --erase   erase the whole flash first (factory reset), then flash and rewrite the ID
  --new-id  mint a new ID for the board (implies rewriting the identity partition)
  --update-identity  rewrite the identity partition from the map (node index, handheld device index), keeping the ID
"""
import argparse
import atexit
import csv
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEVICES = ROOT / "tools" / "bench_devices.json"
PARTITIONS = ROOT / "firmware" / "common" / "partitions_4mb.csv"
PANIC = re.compile(r"Guru Meditation|abort\(\) was called|assert failed", re.IGNORECASE)
LGID = re.compile(r"LGID: (\S+)\s")   # whitespace after the ID proves it arrived whole; the line continues with role and board
ID_RE = re.compile(r"^LG-([A-Z])-([A-Z0-9]{3})-([0-9A-HJKMNP-TV-Z]{10})$")
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
ID_LABEL = "LocalGrid device id v1"


# ---- run lock: one flash.py at a time owns the serial ports, builds, and device map ----

LOCK = ROOT / "tools" / ".flash.lock"


def pid_alive(pid):
    if os.name == "nt":
        # os.kill(pid, 0) terminates the process on Windows, so query it instead.
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259                # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def acquire_lock():
    for _ in range(2):
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                pid = int(LOCK.read_text(encoding="utf-8").split()[0])
            except (OSError, ValueError, IndexError):
                pid = 0
            if pid and pid_alive(pid):
                sys.exit(f"Another flash.py run (process {pid}) is using the boards. Wait for it to finish, "
                         "or stop it if it was left behind by an ended session.")
            LOCK.unlink(missing_ok=True)   # left by a run that died
            continue
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(f"{os.getpid()} {int(time.time())}\n")
        atexit.register(lambda: LOCK.unlink(missing_ok=True))
        return
    sys.exit(f"Could not take the flash lock {LOCK}.")


# ---- device map ----

def load_map():
    return json.loads(DEVICES.read_text(encoding="utf-8"))


def save_map(data):
    DEVICES.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def decode(data, device_id):
    m = ID_RE.match(device_id or "")
    if not m:
        return None
    role, board, _ = m.groups()
    role_desc = data["roles"].get(role, f"unknown role {role}")
    board_info = data["boards"].get(board)
    board_desc = board_info["description"] if board_info else f"unknown board {board}"
    return f"{role_desc} on {board_desc}"


def list_devices(data):
    print(f"{'NAME':<12} {'PORT':<6} {'ROLE':<5} {'BOARD':<6} {'TARGET':<8} {'FIRMWARE':<9} ID")
    for d in data["devices"]:
        target = data["boards"][d["board"]]["target"]
        print(f"{d['name']:<12} {d['port']:<6} {d['role']:<5} {d['board']:<6} {target:<8} {d['firmware']:<9} "
              f"{d.get('id') or '(not provisioned)'}")
    print("\nFirmware types:")
    for name, fw in data["firmware"].items():
        print(f"  {name:<6} {fw['project']:<16} {', '.join(fw['targets'])}: {fw['description']}")
    print("\nBoards:")
    for code, b in data["boards"].items():
        print(f"  {code}  {b['target']:<8} {b['description']}")


def select(data, args):
    devices = data["devices"]
    if args.all:
        chosen = list(devices)
    elif args.role:
        chosen = [d for d in devices if d["role"] == args.role]
    elif args.boards:
        chosen = []
        for key in args.boards:
            match = [d for d in devices if key.lower() in (d["name"].lower(), d["port"].lower(), (d.get("id") or "").lower())]
            if not match:
                sys.exit(f"Unknown board '{key}'. Run with --list to see names, ports, and IDs.")
            chosen.extend(m for m in match if m not in chosen)
    else:
        chosen = []
    # Handhelds first, then nodes one at a time so the grid keeps running while nodes update.
    return sorted(chosen, key=lambda d: (d["role"] == "N", d["name"]))


# ---- process helpers ----

def run(cmd, log):
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT).returncode


def tail(log, pattern=r"error|fatal|failed", lines=6):
    text = pathlib.Path(log).read_text(encoding="utf-8", errors="replace").splitlines()
    text = [l for l in text if "MAC" not in l]   # never surface hardware addresses
    hits = [l for l in text if re.search(pattern, l, re.IGNORECASE)]
    return "\n    ".join((hits or text)[-lines:])


def idf(project, target, *extra):
    idf_py = pathlib.Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"
    proj = ROOT / project
    return [sys.executable, str(idf_py), "-C", str(proj), "-B", str(proj / f"build-{target}"),
            "-D", f"SDKCONFIG={proj / f'sdkconfig.{target}'}", *extra]


def esptool(port, *args, capture=True):
    return subprocess.run([sys.executable, "-m", "esptool", "--port", port, *args],
                          capture_output=capture, text=True, cwd=ROOT)


def build(fw_name, fw, target):
    proj = ROOT / fw["project"]
    log = proj / f"build-{target}.log"
    if not (proj / f"sdkconfig.{target}").exists():
        print(f"  set-target {target} for {fw['project']} (first build takes a few minutes)", flush=True)
        if run(idf(fw["project"], target, "set-target", target), log) != 0:
            return False, f"set-target failed:\n    {tail(log)}"
    print(f"  building {fw_name} for {target} ...", flush=True)
    if run(idf(fw["project"], target, "build"), log) != 0:
        return False, f"build failed:\n    {tail(log, r'error')}"
    warnings = [l for l in log.read_text(encoding="utf-8", errors="replace").splitlines() if "warning:" in l]
    if warnings:
        return False, f"build has {len(warnings)} warning(s); fix before flashing:\n    " + "\n    ".join(warnings[:5])
    return True, "built"


# ---- serial ports ----

def release_and_close(ser):
    """Close a port so the board keeps running.

    On the bench's CP210x/CH340 auto-reset boards a plain close leaves the control lines in a
    state that holds the chip in reset. Releasing RTS and then DTR leaves the board
    running without a reboot; the opposite order reboots it once more on close."""
    try:
        # RTS first. The auto-reset circuit pulls EN low while RTS is asserted and DTR is not,
        # so dropping DTR first resets the board on the way out: every tool run used to restart
        # a board twice, once opening and once closing, and the second one, unseen, wiped grid
        # time on every AP a tool had touched. Dropping RTS first passes through the state that
        # only holds IO0 low, which a running board ignores.
        ser.rts = False
        ser.dtr = False
        time.sleep(0.1)
    except (OSError, ValueError):
        pass
    ser.close()


# ---- board identity ----

def query_id(port, wait_s=9.0):
    """Opens the port (which resets most boards), then asks 'id' until an LGID line arrives."""
    import serial
    start = time.time()
    last = 0.0
    buf = b""
    try:
        # write_timeout: a native-USB board that is not reading input would otherwise block the write forever.
        ser = serial.Serial(port, 115200, timeout=0.2, write_timeout=0.5)
    except serial.SerialException as e:
        return f"ERROR {e}"
    try:
        while time.time() - start < wait_s:
            buf += ser.read(4096)
            m = LGID.search(buf.decode("utf-8", "replace"))
            if m:
                return m.group(1)
            now = time.time()
            if now - start > 2.0 and now - last > 1.5:
                last = now
                try:
                    ser.write(b"id\r\n")
                except serial.SerialTimeoutException:
                    pass   # board not reading its console; keep listening until the deadline
    except serial.SerialException as e:
        return f"ERROR {e}"
    finally:
        release_and_close(ser)
    return None


def chip_and_mac(port):
    """Returns (chip name, mac). The MAC stays in memory for minting an ID only."""
    proc = esptool(port, "chip-id")
    out = proc.stdout + proc.stderr
    chip = re.search(r"Connected to (ESP32[-A-Z0-9]*) on", out)
    mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    return (chip.group(1) if chip else None), (mac.group(1).lower() if mac else None)


def mint_id(role, board, mac, created):
    nonce = os.urandom(8).hex()
    digest = hashlib.sha256(f"{ID_LABEL}|{role}{board}|{mac}|{created}|{nonce}".encode()).digest()
    value = int.from_bytes(digest[:7], "big") >> 6      # 50 bits
    tail_chars = "".join(CROCKFORD[(value >> (5 * (9 - i))) & 31] for i in range(10))
    return f"LG-{role}-{board}-{tail_chars}"


def identity_partition():
    for row in csv.reader(l for l in PARTITIONS.read_text(encoding="utf-8").splitlines() if l and not l.startswith("#")):
        cells = [c.strip() for c in row]
        if cells and cells[0] == "lgid":
            return cells[3], int(cells[4], 16)
    sys.exit(f"No lgid partition in {PARTITIONS}")


def write_identity(device, port):
    offset, size = identity_partition()
    rows = [["key", "type", "encoding", "value"], ["lgid", "namespace", "", ""],
            ["id", "data", "string", device["id"]], ["role", "data", "string", device["role"]],
            ["board", "data", "string", device["board"]], ["created", "data", "u32", str(device["id_created"])]]
    if device["role"] == "N":
        rows += [["node_idx", "data", "u8", str(device["node_index"])], ["node_name", "data", "string", device["node_name"]]]
    if device["role"] == "H" and device.get("device_index"):
        rows += [["device_idx", "data", "u32", str(device["device_index"])]]
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "lgid.csv"
        bin_path = pathlib.Path(tmp) / "lgid.bin"
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(rows)
        gen = subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", str(csv_path), str(bin_path),
                              str(size)], capture_output=True, text=True, cwd=ROOT)
        if gen.returncode != 0:
            return False, "identity image generation failed: " + (gen.stderr or gen.stdout).strip()[-300:]
        wr = esptool(port, "write-flash", offset, str(bin_path))
        if wr.returncode != 0:
            return False, "identity write failed"
    return True, "identity written"


def verify(device, fw, expected_id):
    import serial
    ok_re = re.compile(fw["verify_ok"])
    bad_re = re.compile(fw["verify_fail"]) if fw.get("verify_fail") else None
    start = time.time()
    deadline = start + fw.get("verify_timeout_s", 30)
    last_query = 0.0
    buf = b""
    ok = False
    got_id = None
    try:
        ser = serial.Serial(device["port"], 115200, timeout=0.2, write_timeout=0.5)   # opening resets the board
    except serial.SerialException as e:
        return False, f"serial error: {e}"
    try:
        if True:   # keeps the loop body at its original indentation
            while time.time() < deadline and not (ok and got_id):
                buf += ser.read(4096)
                text = buf.decode("utf-8", "replace")
                if PANIC.search(text):
                    return False, "crashed: " + PANIC.search(text).group(0)
                if bad_re and bad_re.search(text):
                    fails = [l for l in text.splitlines() if l.startswith("FAIL")]
                    return False, "tests failed:\n    " + "\n    ".join(fails[:8])
                ok = ok or bool(ok_re.search(text))
                m = LGID.search(text)
                got_id = got_id or (m.group(1) if m else None)
                now = time.time()
                if got_id is None and now - start > 2.0 and now - last_query > 2.0:
                    last_query = now
                    try:
                        ser.write(b"id\r\n")
                    except serial.SerialTimeoutException:
                        pass
    except serial.SerialException as e:
        return False, f"serial error: {e}"
    finally:
        release_and_close(ser)
    if not ok:
        return False, f"no '{fw['verify_ok']}' within {fw.get('verify_timeout_s', 30)} s"
    if got_id != expected_id:
        return False, f"firmware started but answered id {got_id or 'nothing'}, expected {expected_id}"
    summary = [l.strip() for l in buf.decode("utf-8", "replace").splitlines()
               if re.search(r"LG_TESTS:|\[WEB\] Admin|\[BB\] Link up|\[GRID\] Registered", l)]
    return True, "; ".join(["id ok"] + summary[:3])


# ---- commands ----

def identify(data, chosen):
    print(f"{'NAME':<12} {'PORT':<6} {'ANSWERED ID':<24} DEVICE TYPE")
    for d in chosen:
        got = query_id(d["port"])
        if got and got.startswith("ERROR"):
            desc = got
            got = "-"
        else:
            desc = decode(data, got) if got else "no answer (not provisioned or older firmware)"
            if got and d.get("id") and got != d["id"]:
                desc += f"  MISMATCH: map expects {d['id']}"
        print(f"{d['name']:<12} {d['port']:<6} {got or '-':<24} {desc}")


def flash_board(data, d, args, built):
    boards, firmware = data["boards"], data["firmware"]
    target = boards[d["board"]]["target"]
    fw_name = args.firmware or d["firmware"]
    fw = firmware[fw_name]
    print(f"\n== {d['name']} ({d['port']}, {d['board']} {target}) <- {fw_name}", flush=True)
    if target not in fw["targets"] or d["role"] not in fw["roles"]:
        return False, f"{fw_name} firmware does not support role {d['role']} on {target}"

    # 1. Recognize the board by its ID, never by hardware address.
    answered = query_id(d["port"])
    if answered and answered.startswith("ERROR"):
        return False, answered
    if d.get("id"):
        if answered and answered != d["id"]:
            return False, f"port {d['port']} has {answered} ({decode(data, answered)}), expected {d['id']}"
        if not answered and not args.trust_port:
            return False, ("board did not answer 'id'. If it runs firmware from before device IDs, rerun with "
                           "--trust-port; otherwise check the port")
    elif answered:
        m = ID_RE.match(answered)
        if not m or m.group(1) != d["role"] or m.group(2) != d["board"]:
            return False, f"port {d['port']} answered {answered}, which is not a {d['role']}/{d['board']} board"
        d["id"] = answered
        save_map(data)
        print(f"  recognized existing ID {answered}")

    chip, mac = chip_and_mac(d["port"])
    expected_chip = "ESP32-S3" if target == "esp32s3" else "ESP32"
    if chip != expected_chip:
        return False, f"port {d['port']} has chip {chip or 'unknown'}, board type {d['board']} needs {expected_chip}"

    mint = args.new_id or not d.get("id")
    if mint:
        if not mac:
            return False, "could not read the board to mint an ID"
        d["id_created"] = int(time.time())
        d["id"] = mint_id(d["role"], d["board"], mac, d["id_created"])
        print(f"  minted new ID {d['id']}")
    del mac

    # 2. Build.
    key = (fw_name, target)
    if not args.no_build:
        if key not in built:
            built[key] = build(fw_name, fw, target)
        if not built[key][0]:
            return False, built[key][1]

    # 3. Erase (factory reset) when asked.
    if args.erase:
        print("  erasing all flash (settings, boot counter, identity) ...", flush=True)
        if esptool(d["port"], "erase-flash").returncode != 0:
            return False, "erase failed"

    # 4. Flash the application.
    print("  flashing ...", flush=True)
    log = ROOT / fw["project"] / f"flash-{d['name']}.log"
    if run(idf(fw["project"], target, "-p", d["port"], "flash"), log) != 0:
        return False, f"flash failed:\n    {tail(log)}"

    # 5. Identity partition: for new IDs, after an erase, or when asked to rewrite it from the map.
    if mint or args.erase or args.update_identity:
        ok, detail = write_identity(d, d["port"])
        if not ok:
            return False, detail
        save_map(data)

    if args.no_verify:
        return True, "flashed (not verified)"
    print("  verifying over serial ...", flush=True)
    return verify(d, fw, d["id"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*", help="board names, ports, or IDs from tools/bench_devices.json")
    ap.add_argument("--all", action="store_true", help="every board in the device map")
    ap.add_argument("--role", choices=["N", "H"], help="every board with this role (N node, H handheld)")
    ap.add_argument("--firmware", help="flash this firmware type instead of each board's assignment")
    ap.add_argument("--erase", action="store_true", help="factory reset: erase all flash first; the ID is kept")
    ap.add_argument("--new-id", action="store_true", help="mint a new device ID for the selected boards")
    ap.add_argument("--update-identity", action="store_true",
                    help="rewrite the identity partition from the map, keeping the ID and all settings")
    ap.add_argument("--no-build", action="store_true", help="flash the existing build")
    ap.add_argument("--no-verify", action="store_true", help="skip the serial check after flashing")
    ap.add_argument("--trust-port", action="store_true", help="flash a provisioned board that does not answer 'id'")
    ap.add_argument("--stagger", type=int, default=10, help="seconds between node boards (default 10)")
    ap.add_argument("--list", action="store_true", help="show boards, board types, and firmware types")
    ap.add_argument("--identify", action="store_true", help="ask the selected boards (default all) for their IDs")
    ap.add_argument("--decode", metavar="ID", help="show the device type encoded in an ID")
    args = ap.parse_args()

    data = load_map()
    if args.list:
        list_devices(data)
        return 0
    if args.decode:
        desc = decode(data, args.decode)
        print(desc or f"{args.decode} is not a LocalGrid device ID")
        return 0 if desc else 1
    acquire_lock()   # everything below opens ports, builds, or writes the device map
    if args.identify:
        identify(data, select(data, args) or data["devices"])
        return 0
    if "IDF_PATH" not in os.environ:
        sys.exit("ESP-IDF environment not loaded. In PowerShell run: . C:\\esp\\v6.1\\esp-idf\\export.ps1")
    if args.firmware and args.firmware not in data["firmware"]:
        sys.exit(f"Unknown firmware '{args.firmware}'. Types: {', '.join(data['firmware'])}")
    chosen = select(data, args)
    if not chosen:
        sys.exit("No boards selected. Name boards, or use --all or --role.")

    results = []
    built = {}
    last_node_done = None
    for d in chosen:
        if d["role"] == "N" and last_node_done is not None:
            wait = args.stagger - (time.time() - last_node_done)
            if wait > 0:
                print(f"\n  waiting {wait:.0f} s so the grid keeps its time", flush=True)
                time.sleep(wait)
        ok, detail = flash_board(data, d, args, built)
        results.append((d, args.firmware or d["firmware"], ok, detail))
        if d["role"] == "N":
            last_node_done = time.time()

    print("\nRESULT")
    for d, fw_name, ok, detail in results:
        print(f"  {'OK  ' if ok else 'FAIL'} {d['name']:<12} {d['port']:<6} {fw_name:<6} {d.get('id') or '-':<22} {detail}")
    return 0 if all(ok for _, _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
