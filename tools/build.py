#!/usr/bin/env python3
"""Build LocalGrid firmware without touching any board: every firmware type, some, or what named boards need.

Firmware types, their projects, and their targets live in tools/bench_devices.json.
Uses the same build step as tools/flash.py (per-target build folders, warnings refused)
and the same run lock, so a build never collides with a flash run.
Run inside the ESP-IDF environment (PowerShell: . C:\\esp\\v6.1\\esp-idf\\export.ps1).

Examples:
  python tools/build.py                        # every firmware type for every target it supports
  python tools/build.py --firmware node        # one firmware type, all its targets
  python tools/build.py --firmware tests --target esp32s3
  python tools/build.py hosyond node-main      # what these boards run, for their chips
  python tools/build.py --role H               # what every handheld runs
  python tools/build.py --firmware tests --clean   # delete the build folder and sdkconfig first
  python tools/build.py --list                 # the build matrix and last result of each
"""
import argparse
import os
import pathlib
import re
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from flash import ROOT, acquire_lock, build, load_map, select  # noqa: E402
import check_layers  # noqa: E402

SIZE = re.compile(r"\.bin binary size (0x[0-9a-f]+) bytes\. Smallest app partition.*?\((\d+)%\) free", re.IGNORECASE)


def matrix(data):
    return [(name, target) for name, fw in data["firmware"].items() for target in fw["targets"]]


def last_result(fw, target):
    log = ROOT / fw["project"] / f"build-{target}.log"
    if not log.exists():
        return "never built"
    text = log.read_text(encoding="utf-8", errors="replace")
    sizes = SIZE.findall(text)
    if "Project build complete" in text and sizes:
        size, free = sizes[-1]
        return f"built, {int(size, 16) / 1024:.0f} KB, {free}% of app partition free"
    return "last build did not complete"


def jobs_for(data, args):
    if args.boards or args.role:
        chosen = select(data, args)
        jobs = []
        for d in chosen:
            job = (args.firmware[0] if args.firmware else d["firmware"], data["boards"][d["board"]]["target"])
            if job not in jobs:
                jobs.append(job)
    else:
        wanted = args.firmware or list(data["firmware"])
        jobs = [(n, t) for n, t in matrix(data) if n in wanted]
    if args.target:
        jobs = [(n, t) for n, t in jobs if t in args.target]
    for name, target in jobs:
        if target not in data["firmware"][name]["targets"]:
            sys.exit(f"Firmware '{name}' does not support {target}. Targets: {', '.join(data['firmware'][name]['targets'])}")
    return jobs


def layers_ok(when):
    problems = check_layers.violations()
    if problems:
        print(f"\nLayer check failed {when} (decision D27):")
        for p in problems:
            print(f"  LAYER {p}")
    else:
        print(f"  layers ok {when}", flush=True)
    return not problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*", help="build what these boards run (names, ports, or IDs from the device map)")
    ap.add_argument("--role", choices=["N", "H"], help="build what every board with this role runs")
    ap.add_argument("--firmware", action="append", help="firmware type to build (repeatable)")
    ap.add_argument("--target", action="append", help="limit to this chip target (repeatable), e.g. esp32, esp32s3")
    ap.add_argument("--clean", action="store_true", help="delete the build folder and generated sdkconfig first")
    ap.add_argument("--list", action="store_true", help="show every firmware type and target with its last result")
    args = ap.parse_args()
    args.all = False   # select() reads it

    data = load_map()
    for name in args.firmware or []:
        if name not in data["firmware"]:
            sys.exit(f"Unknown firmware '{name}'. Types: {', '.join(data['firmware'])}")
    if args.list:
        print(f"{'FIRMWARE':<9} {'TARGET':<8} {'PROJECT':<16} LAST RESULT")
        for name, target in matrix(data):
            fw = data["firmware"][name]
            print(f"{name:<9} {target:<8} {fw['project']:<16} {last_result(fw, target)}")
        return 0
    if "IDF_PATH" not in os.environ:
        sys.exit("ESP-IDF environment not loaded. In PowerShell run: . C:\\esp\\v6.1\\esp-idf\\export.ps1")
    jobs = jobs_for(data, args)
    if not jobs:
        sys.exit("Nothing to build for that selection.")
    if not layers_ok("before building"):
        return 1
    acquire_lock()

    results = []
    for name, target in jobs:
        fw = data["firmware"][name]
        proj = ROOT / fw["project"]
        if args.clean:
            print(f"  cleaning {fw['project']} for {target}", flush=True)
            shutil.rmtree(proj / f"build-{target}", ignore_errors=True)
            (proj / f"sdkconfig.{target}").unlink(missing_ok=True)
        ok, detail = build(name, fw, target)
        results.append((name, target, ok, last_result(fw, target) if ok else detail))

    print("\nRESULT")
    for name, target, ok, detail in results:
        print(f"  {'OK  ' if ok else 'FAIL'} {name:<9} {target:<8} {detail}")
    layers = layers_ok("after building")   # build folders show what each project really compiled
    return 0 if layers and all(ok for _, _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
