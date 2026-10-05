#!/usr/bin/env python3
"""Give a LocalGrid device its grid keys over its serial cable (D96).

A device holds its grid's keys in its own storage; firmware no longer has to be built with them. This
makes the setup bundle a device's `provision` command takes and, with a board name, types it at that
board's console. The web flasher does the same from a browser.

  python tools/provision.py --show                       # the bundles, as the device would be given them
  python tools/provision.py hosyond                      # set up one board from firmware/common/lg_secrets.h
  python tools/provision.py node-main --release release.pub.hex   # and tell it whose updates to accept
  python tools/provision.py hosyond --erase              # forget its keys again

An access point is given the Wi-Fi passphrase and the backbone key. A handheld is given the passphrase
and the two derived keys, and never the backbone key; a bundle with the wrong contents for the kind of
device is refused by the device. Keys are read from lg_secrets.h, which stays out of git; the bundle is
as secret as that file and is never written to disk by this tool.
"""
import argparse
import base64
import hashlib
import hmac
import pathlib
import re
import sys
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SECRETS = ROOT / "firmware" / "common" / "lg_secrets.h"
BACKBONE, DERIVED, RELEASE = 0x01, 0x02, 0x04


def hkdf(key, info, length):
    """RFC 5869 HKDF-SHA256 with an empty salt, as lg_hkdf_sha256 computes it on the boards."""
    prk = hmac.new(b"\0" * 32, key, hashlib.sha256).digest()
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


def read_secrets(path):
    text = path.read_text(encoding="utf-8")
    m = re.search(r'#define\s+LG_SECRET_WIFI_PASSPHRASE\s+"([^"]+)"', text)
    k = re.search(r"#define\s+LG_SECRET_BACKBONE_KEY\s*\{([^}]*)\}", text)
    if not m or not k:
        sys.exit(f"provision: {path} has no passphrase or backbone key; run tools/gen_secrets.py")
    key = bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", k.group(1)))
    if len(key) != 32 or not any(key):
        sys.exit(f"provision: the backbone key in {path} is not a real 32-byte key")
    return m.group(1).encode("ascii"), key


def bundle(passphrase, backbone, access_point, release=None):
    if not 8 <= len(passphrase) <= 63:
        sys.exit("provision: the Wi-Fi passphrase must be 8 to 63 characters")
    flags = (BACKBONE if access_point else DERIVED) | (RELEASE if release else 0)
    body = b"LGK1" + bytes([flags, len(passphrase)]) + passphrase
    body += backbone if access_point else hkdf(backbone, b"lg-disc", 4) + hkdf(backbone, b"lg-lora-hh", 32)
    if release:
        if len(release) != 65 or release[0] != 4:
            sys.exit("provision: the release public key should be 65 bytes beginning with 04")
        body += release
    return base64.b64encode(body + zlib.crc32(body).to_bytes(4, "little")).decode("ascii")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*", help="board names, ports or IDs from the device map")
    ap.add_argument("--secrets", default=str(SECRETS))
    ap.add_argument("--release", help="a release public key file (hex), so the device accepts signed updates")
    ap.add_argument("--erase", action="store_true", help="make the boards forget their own keys")
    ap.add_argument("--show", action="store_true", help="print how long each bundle is and stop; never the keys")
    args = ap.parse_args()

    passphrase, backbone = read_secrets(pathlib.Path(args.secrets))
    release = bytes.fromhex(pathlib.Path(args.release).read_text().strip()) if args.release else None
    if args.show:
        for name, is_ap in (("access point", True), ("handheld", False)):
            print(f"{name}: a bundle of {len(bundle(passphrase, backbone, is_ap, release))} characters"
                  f"{' with a release key' if release else ''}")
        return 0
    if not args.boards:
        ap.error("name the boards to set up, or use --show")

    sys.path.insert(0, str(ROOT / "tools"))
    import flash  # noqa: E402

    data = flash.load_map()
    failed = 0
    for want in args.boards:
        device = next((d for d in data["devices"] if want in (d.get("name"), d.get("port"), d.get("id"))), None)
        if device is None or not device.get("port"):
            print(f"{want}: not in the device map, or it has no port")
            failed += 1
            continue
        command = "provision erase" if args.erase else \
            "provision " + bundle(passphrase, backbone, device["role"] == "N", release)
        s = flash.open_quiet(device["port"], 115200, 0.5)
        try:
            s.reset_input_buffer()
            s.write(b"\r\n" + command.encode("ascii") + b"\r\n")
            reply, end = b"", time.time() + 6
            while time.time() < end and b"LGPROV:" not in reply.split(b"provision", 1)[-1]:
                reply += s.read(512)
            reply += s.read(256)
        finally:
            s.rts = False
            s.dtr = False
            time.sleep(0.1)
            s.close()
        lines = [l.strip() for l in reply.decode("utf-8", "replace").splitlines() if l.strip().startswith("LGPROV:")]
        print(f"{device['name']}: {lines[-1] if lines else 'no answer (is this firmware older than D96?)'}")
        failed += 0 if lines and ("ok" in lines[-1] or "erased" in lines[-1]) else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
