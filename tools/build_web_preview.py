#!/usr/bin/env python3
"""Build a standalone preview of the admin web page with a simulated master node.

The firmware page (firmware/node/main/web/admin.html) is used unchanged except:
  - the icon is inlined as a data URI (there is no /favicon.svg outside the node),
  - web/preview/mock-master.js is injected so /api calls are answered in the browser.

Usage: python tools/build_web_preview.py OUTPUT.html
"""
import base64
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
page = (root / "firmware/node/main/web/admin.html").read_text(encoding="utf-8")
icon = (root / "assets/brand/localgrid-icon.svg").read_bytes()
mock = (root / "web/preview/mock-master.js").read_text(encoding="utf-8")

icon_uri = "data:image/svg+xml;base64," + base64.b64encode(icon).decode("ascii")
if page.count("/favicon.svg") != 2 or page.count("</head>") != 1:
    sys.exit("admin.html layout changed; update tools/build_web_preview.py")

page = page.replace('href="/favicon.svg"', f'href="{icon_uri}"').replace('src="/favicon.svg"', f'src="{icon_uri}"')
page = page.replace("<title>LocalGrid</title>", "<title>LocalGrid Admin Preview</title>")
page = page.replace("</head>", f"<script>\n{mock}\n</script>\n</head>")

out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/web-preview/index.html")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(page, encoding="utf-8")
print(f"wrote {out} ({len(page)} bytes)")
