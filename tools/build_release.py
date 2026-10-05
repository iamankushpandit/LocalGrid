#!/usr/bin/env python3
"""Build the images that can be published: firmware with no grid's keys in it (D96).

The bench builds carry the bench grid's keys from lg_secrets.h, so they can never be offered to anyone.
This builds each firmware again with LG_PUBLISHED_BUILD set, which leaves the keys out: such an image
starts, says it has not been set up, and waits for `provision`. For each (firmware, chip) it writes

  OUT/lg_<firmware>-<chip>.bin          one file for a blank board: bootloader, partitions and the app
  OUT/manifest-<firmware>-<chip>.json   what the installer page's flash button reads (ESP Web Tools)
  OUT/lg_<firmware>-<chip>.lgfw         the signed update file, when --key is given

and `tools/gen_site.py --firmware OUT` publishes them beside the installer page.

  python tools/build_release.py --out build/release --version 2026.10.5
  python tools/build_release.py --out build/release --version 2026.10.5 --key release.key.pem

Run inside the ESP-IDF environment. It uses its own build folders (build-release-<chip>), so the bench
builds and what the bench boards are running are left alone.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILDS = [("node", "firmware/node", "lg_node", "N", ["esp32"]),
          ("handheld", "firmware/handheld", "lg_handheld", "H", ["esp32", "esp32s3", "esp32c6"])]
FAMILY = {"esp32": "ESP32", "esp32s3": "ESP32-S3", "esp32c6": "ESP32-C6"}
NAMES = {"node": "LocalGrid access point", "handheld": "LocalGrid handheld"}


def run(cmd, env=None, log=None):
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    if log is not None:
        log.write_text(r.stdout + r.stderr, encoding="utf-8", errors="replace")
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "release"))
    ap.add_argument("--version", required=True, help="the version name, shown by the installer and signed into updates")
    ap.add_argument("--key", help="a release private key (tools/lgfw.py keygen): also write signed update files")
    ap.add_argument("--only", help="one firmware: node or handheld")
    args = ap.parse_args()
    if "IDF_PATH" not in os.environ:
        sys.exit("build_release: run inside the ESP-IDF environment")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    idf_py = pathlib.Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"
    env = dict(os.environ, LG_PUBLISHED_BUILD="1")
    failed = 0
    for fw, project, app, role, chips in BUILDS:
        if args.only and args.only != fw:
            continue
        for chip in chips:
            proj = ROOT / project
            build = proj / f"build-release-{chip}"
            print(f"building {fw} for {chip} with no grid keys ...", flush=True)
            ok = run([sys.executable, str(idf_py), "-C", str(proj), "-B", str(build),
                      "-D", f"SDKCONFIG={build / 'sdkconfig'}", "-D", f"IDF_TARGET={chip}", "build"],
                     env=env, log=out / f"build-{fw}-{chip}.log")
            if not ok:
                print(f"  FAIL: see {out / f'build-{fw}-{chip}.log'}")
                failed += 1
                continue
            flasher = json.loads((build / "flasher_args.json").read_text())
            merged = out / f"lg_{fw}-{chip}.bin"
            cmd = [sys.executable, "-m", "esptool", "--chip", chip, "merge-bin", "-o", str(merged),
                   *flasher["write_flash_args"]]
            for offset, name in flasher["flash_files"].items():
                cmd += [offset, str(build / name)]
            if not run(cmd):
                print("  FAIL: the images could not be merged")
                failed += 1
                continue
            manifest = {"name": NAMES[fw], "version": args.version, "new_install_prompt_erase": True,
                        "builds": [{"chipFamily": FAMILY[chip], "parts": [{"path": merged.name, "offset": 0}]}]}
            (out / f"manifest-{fw}-{chip}.json").write_text(json.dumps(manifest, indent=1) + "\n")
            line = f"  {merged.name}: {merged.stat().st_size // 1024} KB"
            if args.key:
                ok = run([sys.executable, str(ROOT / "tools" / "lgfw.py"), "sign", "--key", args.key, "--role", role,
                          "--chip", chip, "--version", args.version, str(build / f"{app}.bin"),
                          "-o", str(out / f"lg_{fw}-{chip}.lgfw")])
                line += ", signed update written" if ok else ", SIGNING FAILED"
                failed += 0 if ok else 1
            print(line)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
