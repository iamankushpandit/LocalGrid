#!/usr/bin/env python3
"""Check that LocalGrid's layers stay separate (decision D27). Exits 1 on any violation.

Rules:
  1. Infrastructure (lg_core, lg_crypto, lg_identity, node firmware) never includes UI,
     LVGL, or board-support headers, and never requires those components.
  2. lg_bsp (drivers) never includes LVGL or UI headers.
  3. lg_ui never includes ESP-IDF driver, esp_lcd, NVS, or heap-caps headers; it reaches
     hardware only through lg_bsp.
  4. A node build folder contains no UI-side components, and no LVGL was downloaded for it.

tools/build.py runs this before and after every build.
  python tools/check_layers.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

INFRA_SOURCES = ["components/lg_core", "components/lg_crypto", "components/lg_identity", "firmware/node/main"]
INFRA_CMAKE = ["components/lg_core/CMakeLists.txt", "components/lg_crypto/CMakeLists.txt",
               "components/lg_identity/CMakeLists.txt", "firmware/node/main/CMakeLists.txt"]
NODE_PROJECT = "firmware/node"

UI_SIDE_INCLUDE = re.compile(r'#\s*include\s*[<"](lvgl|lg_display|lg_theme|lg_ui_input|lg_bsp_\w+)\b')
LVGL_OR_UI_INCLUDE = re.compile(r'#\s*include\s*[<"](lvgl|lg_display|lg_theme|lg_ui_input)\b')
DRIVER_INCLUDE = re.compile(r'#\s*include\s*[<"](driver/|esp_lcd|nvs|esp_heap_caps)')
UI_SIDE_COMPONENT = re.compile(r"\b(lg_bsp|lg_ui|lvgl\w*)\b")


def sources(rel):
    base = ROOT / rel
    if not base.exists():
        return []
    skip = ("managed_components",)
    return sorted(p for p in base.rglob("*")
                  if p.suffix in (".c", ".h") and not any(part in skip or part.startswith("build") for part in p.parts))


def scan(rel, pattern, rule):
    found = []
    for path in sources(rel):
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if pattern.search(line):
                found.append(f"{path.relative_to(ROOT).as_posix()}:{n}: {rule}: {line.strip()}")
    return found


def violations():
    found = []
    for rel in INFRA_SOURCES:
        found += scan(rel, UI_SIDE_INCLUDE, "infrastructure includes a UI or board-support header")
    for rel in INFRA_CMAKE:
        path = ROOT / rel
        if not path.exists():
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("#", 1)[0]
            if UI_SIDE_COMPONENT.search(code):
                found.append(f"{rel}:{n}: infrastructure requires a UI-side component: {line.strip()}")
    found += scan("components/lg_bsp", LVGL_OR_UI_INCLUDE, "lg_bsp includes LVGL or UI headers")
    found += scan("components/lg_ui", DRIVER_INCLUDE, "lg_ui includes a driver header; go through lg_bsp")

    node = ROOT / NODE_PROJECT
    for build in sorted(node.glob("build-*/esp-idf")):
        for comp in sorted(p.name for p in build.iterdir() if p.is_dir()):
            if UI_SIDE_COMPONENT.fullmatch(comp):
                found.append(f"{build.relative_to(ROOT).as_posix()}/{comp}: node build contains a UI-side component")
    managed = node / "managed_components"
    if managed.exists():
        for comp in sorted(p.name for p in managed.iterdir() if p.is_dir()):
            if UI_SIDE_COMPONENT.search(comp):
                found.append(f"{managed.relative_to(ROOT).as_posix()}/{comp}: UI-side component downloaded for the node")
    return found


def main():
    found = violations()
    for v in found:
        print(f"LAYER {v}")
    print("layers ok" if not found else f"{len(found)} layer violation(s) (decision D27)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
