#!/usr/bin/env python3
"""Refuse to release while the bench concessions are still in the firmware.

Some things exist only because of the bench: an access point that breaks itself on purpose, chaos
armed over the network, and a firmware upload gated by nothing stronger than the admin password.
They are recorded in docs/TEMPORARY.md with what must be true before each one comes out.

The owner's rule (2026-09-30): "Before we do an initial release, and take this out of the prototype
phase, we definitely need to have a blocker that we can't release while taking out some of these
backdoors that we are creating right now to test the resiliency."

This is that blocker. A note in a document is a reminder; this fails.

  python tools/check_release.py            report the register, exit 0
  python tools/check_release.py --release  fail if anything is still present (a release, and a push)

A PUSH is refused as well as a release (owner, 2026-09-30: "this will absolutely need to be taken
off before we do a push to GitHub or before we do a release"). The git pre-push hook in .githooks
runs --release; install it with `python tools/install_hooks.py`, which this repository's own setup
does. CI runs --release on pushes to the default branch and on tags.

Add a check here on the day a concession is added, never later.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read(rel):
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""


def exists(rel):
    return (ROOT / rel).exists()


def check_t1_chaos_firmware():
    """T1: an AP that attacks itself must not be in the tree at release."""
    found = []
    for f in ("firmware/node/main/chaos_self.c", "firmware/node/main/chaos_self.h",
              "firmware/node/main/chaos_log.c", "firmware/node/main/chaos_log.h"):
        if exists(f):
            found.append(f)
    if "LG_NODE_CHAOS" in read("firmware/node/main/Kconfig.projbuild"):
        found.append("the LG_NODE_CHAOS build option")
    if "chaoslog" in read("firmware/common/partitions_4mb_ota.csv"):
        found.append("the chaoslog partition")
    return found


def check_t2_chaos_over_network():
    """T2: chaos must not be reachable over Wi-Fi at release."""
    web = read("firmware/node/main/web_admin.c")
    found = []
    for route in ('"/api/chaos"', '"/api/chaoslog"'):
        if route in web:
            found.append(f"the {route} route on the admin page")
    return found


def check_t3_unsigned_ota():
    """
    T3 is hardened rather than removed: OTA stays, but an AP must not accept just any image. Satisfied
    by signed images (or full secure boot) being enabled in the AP's build.
    """
    cfg = read("firmware/node/sdkconfig.defaults")
    signed = re.search(r"^CONFIG_SECURE_(SIGNED_APP|BOOT)\w*=y", cfg, re.MULTILINE)
    if "/api/update" in read("firmware/node/main/web_admin.c") and not signed:
        return ["firmware upload is enabled but images are not signed "
                "(set CONFIG_SECURE_SIGNED_APP_NO_SECURE_BOOT=y, or full secure boot)"]
    return []


def check_t5_ble_chaos():
    """
    T5: chaos must not be reachable over BLE at release.

    This is the one that most plainly contradicts a standing decision - D70 says a BLE watcher "can
    never take part" - so it is checked by opcode, not by a comment that could be edited away.
    """
    link = read("firmware/node/main/ble_link.c")
    found = []
    for op in ("OP_CHAOS_ARM", "OP_CHAOS_DISARM", "OP_CHAOS_LOG", "OP_CHAOS_GET"):
        if op in link:
            found.append(f"the {op} opcode on the BLE admin link")
    return found


def check_prototype_wording():
    """
    A release also means the README and the site stop calling this a prototype. If that wording is
    already gone while concessions remain, something has been released by accident.
    """
    readme = read("README.md").lower()
    return [] if "prototype" in readme else ["README no longer says prototype, so this may already be released"]


CHECKS = [
    ("T1", "chaos firmware in the tree", check_t1_chaos_firmware),
    ("T2", "chaos armed over the network", check_t2_chaos_over_network),
    ("T3", "firmware upload accepts unsigned images", check_t3_unsigned_ota),
    ("T5", "chaos armed over BLE", check_t5_ble_chaos),
    ("--", "README still calls this a prototype", check_prototype_wording),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release", action="store_true",
                    help="fail if any concession is still present (run this for a release)")
    args = ap.parse_args()

    if not exists("docs/TEMPORARY.md"):
        print("docs/TEMPORARY.md is missing: the register of what must come out before a release.")
        return 1

    blocking = []
    print(f"{'ID':<4} {'CONCESSION':<44} STATE")
    for ident, title, fn in CHECKS:
        found = fn()
        # A concession's findings say it is still here. The wording check is the other way round:
        # its findings are a warning that the prototype label has already gone, so the same
        # "present/gone" wording would read backwards and did.
        if ident == "--":
            state = "WARNING" if found else "ok"
        else:
            state = "present" if found else "gone"
        print(f"{ident:<4} {title:<44} {state}")
        for f in found:
            print(f"       - {f}")
        if found and ident != "--":
            blocking.extend(found)

    print()
    if not args.release:
        print("Reporting only. A release runs this with --release, which fails while anything is present.")
        print("See docs/TEMPORARY.md for what must be true before each one can come out.")
        return 0

    if blocking:
        print("RELEASE BLOCKED. These exist for the bench and must not ship:")
        for b in blocking:
            print(f"  - {b}")
        print("\ndocs/TEMPORARY.md says what has to become true before each one comes out.")
        return 1
    print("Nothing from the register is left in the tree; this may be released.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
