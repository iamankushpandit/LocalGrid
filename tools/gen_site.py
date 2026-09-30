#!/usr/bin/env python3
"""Generate the LocalGrid project site (GitHub Pages) from the repository.

Every fact on the page is read from the tree at build time, so the site cannot drift from the
code: milestones from docs/milestones/, boards from
tools/bench_devices.json (descriptions only), recent work from CHANGELOG.md, and the admin page
from firmware/node/main/web/admin.html through tools/build_web_preview.py.

Output:
  OUT/index.html           the landing page (site/index.template.html filled in)
  OUT/admin/index.html     the real admin page answering from a simulated MAIN, in the browser
  OUT/assets/...           brand icons
  OUT/.nojekyll            serve files as they are

The page is refused if it would publish bench details (COM ports, device IDs, MAC addresses,
anything from lg_secrets.h) or call the product a camp network (D19).
Pure standard library; CI runs it on every pull request and Pages runs it on every push to main.

  python tools/gen_site.py --out _site --repo https://github.com/OWNER/LocalGrid
"""
import argparse
import datetime
import html
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "site" / "index.template.html"

# Nothing from the bench may reach a public page. Ports and device IDs say which boards the
# owner has on which USB socket; a MAC is burned into eFuse and names a board for life.
LEAKS = {
    "a serial port": re.compile(r"\bCOM\d+\b|/dev/tty(USB|ACM)\d+"),
    "a device ID": re.compile(r"\bLG-[NH]-[A-Z0-9]{3}-[A-Z0-9]{8,}\b"),
    "a MAC address": re.compile(r"\b[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}\b"),
    "a secret": re.compile(r"\bLG_SECRET_[A-Z_]+"),
    "camp-network wording (D19)": re.compile(r"\bcamp(site|ing)?[- ]network\b", re.IGNORECASE),
}


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def milestones():
    """<li> per milestone: its title and the bold part of its Status line, without bench detail."""
    items = []
    for path in sorted((ROOT / "docs" / "milestones").glob("P*.md")):
        lines = path.read_text(encoding="utf-8").splitlines()
        title = re.sub(r"^#\s*Prototype milestone\s*", "", lines[0]).strip()
        number, _, name = title.partition(":")
        status = next((l for l in lines if l.lower().startswith("status:")), "")
        bold = re.search(r"\*\*(.+?)\*\*", status)
        state = (bold.group(1) if bold else status.partition(":")[2]).strip().rstrip(".")
        done = state.lower().startswith("passed")
        items.append(
            f'<li class="ms{" ms-done" if done else ""}"><span class="ms-n">P{html.escape(number.strip())}</span>'
            f'<span class="ms-t">{html.escape(name.strip().capitalize())}</span>'
            f'<span class="ms-s">{html.escape(state[0].upper() + state[1:])}</span></li>')
    if not items:
        sys.exit("gen_site: no milestone reports in docs/milestones/")
    return "\n".join(items)


def boards():
    import json
    data = json.loads((ROOT / "tools" / "bench_devices.json").read_text(encoding="utf-8"))
    roles = {}
    for dev in data["devices"]:
        roles.setdefault(dev["board"], set()).add("Access point" if dev["role"] == "N" else "Handheld")
    rows = []
    for code, board in data["boards"].items():
        name, _, detail = board["description"].partition("(")
        role = " and ".join(sorted(roles.get(code, {"Bench"})))
        rows.append(f'<tr><td>{html.escape(name.strip())}</td><td>{html.escape(board["target"].upper().replace("ESP32S3", "ESP32-S3"))}</td>'
                    f'<td>{html.escape(role)}</td><td>{html.escape(detail.rstrip(")").strip())}</td></tr>')
    return "\n".join(rows)


def recent_work(limit=6):
    """The newest '### ...' headings of the changelog, as a short list of what is moving."""
    heads = re.findall(r"^### (.+)$", (ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), re.MULTILINE)
    items = []
    for h in heads[:limit]:
        kind, _, what = h.partition("(")
        what = what.rstrip(")") or kind
        items.append(f'<li><span class="rw-k">{html.escape(kind.strip())}</span> {html.escape(what.strip())}</li>')
    return "\n".join(items)


LICENCES = [   # (text that identifies the LICENSE file, SPDX id, name)
    ("GNU AFFERO GENERAL PUBLIC LICENSE", "AGPL-3.0-or-later", "GNU AGPL v3 or later"),
    ("GNU GENERAL PUBLIC LICENSE", "GPL-3.0-or-later", "GNU GPL v3 or later"),
    ("Mozilla Public License Version 2.0", "MPL-2.0", "Mozilla Public License 2.0"),
    ("Apache License", "Apache-2.0", "Apache License 2.0"),
    ("MIT License", "MIT", "MIT License"),
]


def licence(repo):
    """The licence line for the page, from LICENSE. With none, the page says so (see docs/OPEN_SOURCE.md)."""
    path = ROOT / "LICENSE"
    if not path.exists():
        return ("No licence has been chosen yet, so for now all rights are reserved. "
                "The choice is open and tracked in <a href=\"" + repo + "/blob/main/docs/OPEN_SOURCE.md\">docs/OPEN_SOURCE.md</a>.")
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker, spdx, name in LICENCES:
        if marker in text:
            return (f'Released under the <a href="{repo}/blob/main/LICENSE">{name}</a> '
                    f'(<code>{spdx}</code>). Third-party material keeps its own licence; see '
                    f'<a href="{repo}/blob/main/THIRD_PARTY.md">THIRD_PARTY.md</a>.')
    sys.exit("gen_site: LICENSE exists but is not a licence this tool recognises; add it to LICENCES")


def fill(template, values):
    out = template
    for key, value in values.items():
        token = "{{" + key + "}}"
        if token not in out:
            sys.exit(f"gen_site: template has no {token}; update site/index.template.html or this tool")
        out = out.replace(token, value)
    left = re.findall(r"\{\{[A-Z_]+\}\}", out)
    if left:
        sys.exit(f"gen_site: unfilled placeholders: {', '.join(sorted(set(left)))}")
    return out


def check_public(path, text):
    for what, pattern in LEAKS.items():
        m = pattern.search(text)
        if m:
            line = text.count("\n", 0, m.start()) + 1
            sys.exit(f"gen_site: {path} line {line} would publish {what}: {m.group(0)!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "site"), help="output directory (replaced)")
    ap.add_argument("--repo", default="https://github.com/iamankushpandit/LocalGrid", help="repository URL for links")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    (out / "assets").mkdir(parents=True)
    (out / "admin").mkdir()

    commit = git("rev-parse", "--short=7", "HEAD") or "unknown"
    values = {
        "REPO": html.escape(args.repo.rstrip("/")),
        "COMMIT": html.escape(commit),
        "BUILT": datetime.date.today().isoformat(),
        "MILESTONES": milestones(),
        "BOARDS": boards(),
        "RECENT": recent_work(),
    }
    values["LICENCE"] = licence(values["REPO"])
    page = fill(TEMPLATE.read_text(encoding="utf-8"), values)
    check_public("index.html", page)
    (out / "index.html").write_text(page, encoding="utf-8")

    for icon in (ROOT / "assets" / "brand").glob("*.svg"):
        shutil.copy2(icon, out / "assets" / icon.name)

    # The network diagram is a file, not inline, so the README can show the same drawing.
    shutil.copy2(ROOT / "assets" / "network-diagram.svg", out / "assets" / "network-diagram.svg")

    preview = out / "admin" / "index.html"
    subprocess.run([sys.executable, str(ROOT / "tools" / "build_web_preview.py"), str(preview)], check=True)
    check_public("admin/index.html", preview.read_text(encoding="utf-8"))

    (out / ".nojekyll").write_text("", encoding="utf-8")
    print(f"site written to {out} (commit {commit})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
