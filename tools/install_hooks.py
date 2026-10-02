#!/usr/bin/env python3
"""Point this repository's git hooks at .githooks, so a push can be refused.

Why this exists. Some things in this firmware are in the tree only because of the bench: an access
point that breaks itself, chaos armed over Wi-Fi and over BLE, and a firmware upload gated by
nothing stronger than the admin password. They are registered in docs/TEMPORARY.md with what must
become true before each one comes out.

    Owner, 2026-09-30: "this will absolutely need to be taken off before we do a push to GitHub or
    before we do a release."

A note in a document is a reminder. `tools/check_release.py --release` is the check, and this is
what makes git run it before a push leaves the machine.

Git does not share hooks through a clone - .git/hooks is local and never committed - so the hooks
live in .githooks, which is committed, and this sets core.hooksPath to point at them. It is set per
worktree where git supports that, and otherwise for the whole repository.

    python tools/install_hooks.py            install
    python tools/install_hooks.py --check    say whether it is installed, exit 1 if not
    python tools/install_hooks.py --remove   put git's own default back

The hook can be bypassed with LG_ALLOW_TEMPORARY=1 for one command. That is on purpose: a guard
nobody can get past in an emergency gets uninstalled altogether, and this one leaves a line in the
shell history saying the person knew.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HOOKS = ROOT / ".githooks"


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)


def current_path():
    out = git("config", "--get", "core.hooksPath")
    return out.stdout.strip() if out.returncode == 0 else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report whether the hooks are installed")
    ap.add_argument("--remove", action="store_true", help="restore git's default hooks path")
    args = ap.parse_args()

    if not (HOOKS / "pre-push").exists():
        print(f"{HOOKS / 'pre-push'} is missing; nothing to install.")
        return 1

    if args.remove:
        git("config", "--unset", "core.hooksPath")
        print("core.hooksPath unset. Git's own .git/hooks is in charge again, and a push is no "
              "longer checked against docs/TEMPORARY.md.")
        return 0

    if args.check:
        if current_path() == ".githooks":
            print("Installed: core.hooksPath is .githooks, so a push runs check_release.py --release.")
            return 0
        print("NOT installed. A push would not be checked against docs/TEMPORARY.md.\n"
              "Run: python tools/install_hooks.py")
        return 1

    res = git("config", "core.hooksPath", ".githooks")
    if res.returncode != 0:
        print("Could not set core.hooksPath:\n" + (res.stderr or res.stdout).strip())
        return 1
    print("Installed. `git push` now runs tools/check_release.py --release first and refuses while\n"
          "anything in docs/TEMPORARY.md is still in the tree.\n"
          "Bypass once, visibly, with: LG_ALLOW_TEMPORARY=1 git push ...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
