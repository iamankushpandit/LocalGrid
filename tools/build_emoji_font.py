#!/usr/bin/env python3
"""Build the handheld's emoji font from Noto Emoji (monochrome, OFL-1.1).

LVGL's font engine draws single-colour glyphs, so the monochrome Noto Emoji is used, not a
colour emoji font: the colour builds are 3 to 10 MB of bitmaps or COLRv1 layers that LVGL
cannot render. Only the curated set below is embedded, because every glyph costs flash.

Steps, all offline after the first download:
  1. Download NotoEmoji[wght].ttf and OFL.txt into tools/.cache (gitignored).
  2. Instance the variable font at weight 400 and subset it to the curated set (fontTools).
  3. Convert to an LVGL C font with lv_font_conv (npm, run through npx).
  4. Write components/lg_ui/include/lg_emoji.h so the keyboard page and the font stay in step.

  python tools/build_emoji_font.py            build if the font file is missing
  python tools/build_emoji_font.py --force    rebuild
  python tools/build_emoji_font.py --list     show the curated set

The generated font and header are committed, so a normal build needs neither Node nor a
download. Rerun this only to change the set or the size.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / "tools" / ".cache"
FONT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
LICENSE_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/OFL.txt"
FONT_TTF = CACHE / "NotoEmoji-variable.ttf"
FONT_STATIC = CACHE / "NotoEmoji-400.ttf"
FONT_SUBSET = CACHE / "NotoEmoji-subset.ttf"
LICENSE_DST = ROOT / "components" / "lg_ui" / "fonts" / "NotoEmoji-OFL.txt"
FONT_OUT = ROOT / "components" / "lg_ui" / "fonts" / "lg_font_emoji_20.c"
HEADER_OUT = ROOT / "components" / "lg_ui" / "include" / "lg_emoji.h"
FONT_NAME = "lg_font_emoji_20"
SIZE_PX = 20        # matches the theme's body text on a 240 px wide screen
BPP = 4

# Curated set: what a message on an offline grid actually needs. Keep it short; each glyph
# costs flash. Order is the keyboard order, six per row.
EMOJI = [
    (0x1F642, "slight smile"),
    (0x1F603, "grin"),
    (0x1F602, "laughing"),
    (0x1F609, "wink"),
    (0x1F60D, "heart eyes"),
    (0x1F61B, "tongue"),
    (0x1F614, "sad"),
    (0x1F622, "crying"),
    (0x1F620, "angry"),
    (0x1F62E, "surprised"),
    (0x1F630, "worried"),
    (0x1F634, "sleeping"),
    (0x1F44D, "thumbs up"),
    (0x1F44E, "thumbs down"),
    (0x1F44C, "ok hand"),
    (0x1F44B, "wave"),
    (0x1F64F, "please"),
    (0x1F4AA, "strong"),
    (0x2764, "heart"),
    (0x1F525, "fire"),
    (0x2B50, "star"),
    (0x2705, "done"),
    (0x274C, "no"),
    (0x2757, "important"),
    (0x26A0, "warning"),
    (0x1F198, "sos"),
    (0x1F6D1, "stop"),
    (0x1F691, "ambulance"),
    (0x1F692, "fire engine"),
    (0x1F46E, "police"),
    (0x26FA, "tent"),
    (0x1F3E0, "home"),
    (0x1F5FA, "map"),
    (0x1F9ED, "compass"),
    (0x1F526, "torch"),
    (0x1F50B, "battery"),
    (0x1F4F6, "signal"),
    (0x1F4DE, "phone"),
    (0x1F4AC, "talk"),
    (0x1F553, "clock"),
    (0x1F327, "rain"),
    (0x2600, "sun"),
    (0x1F319, "night"),
    (0x2744, "cold"),
    (0x1F35C, "food"),
    (0x2615, "hot drink"),
    (0x1F6B0, "water"),
    (0x1F6BD, "toilet"),
]


def fetch(url, path):
    if path.exists() and path.stat().st_size > 0:
        print(f"  have {path.name}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  downloading {path.name}")
    with urllib.request.urlopen(url, timeout=120) as r, open(path, "wb") as f:
        shutil.copyfileobj(r, f)


def instance_and_subset():
    from fontTools import subset as ft_subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    print("  instancing weight 400")
    font = TTFont(FONT_TTF)
    instancer.instantiateVariableFont(font, {"wght": 400}, inplace=True, updateFontNames=False)
    font.save(FONT_STATIC)

    missing = []
    cmap = TTFont(FONT_STATIC).getBestCmap()
    for cp, name in EMOJI:
        if cp not in cmap:
            missing.append(f"U+{cp:04X} {name}")
    if missing:
        sys.exit("font has no glyph for: " + ", ".join(missing))

    print(f"  subsetting to {len(EMOJI)} glyphs")
    ft_subset.main([
        str(FONT_STATIC),
        f"--output-file={FONT_SUBSET}",
        "--unicodes=" + ",".join(f"U+{cp:04X}" for cp, _ in EMOJI),
        "--layout-features=",
        "--no-hinting",
        "--desubroutinize",
        "--drop-tables+=GSUB,GPOS,morx,COLR,CPAL,CBDT,CBLC,sbix,SVG",
    ])


def convert():
    ranges = ",".join(f"0x{cp:X}" for cp, _ in EMOJI)
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if npx is None:
        sys.exit("npx not found; Node.js is needed to run lv_font_conv")
    FONT_OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [npx, "--yes", "lv_font_conv@1.5.3",
           "--font", str(FONT_SUBSET),
           "--size", str(SIZE_PX),
           "--bpp", str(BPP),
           "--range", ranges,
           "--format", "lvgl",
           "--lv-include", "lvgl.h",
           "--no-compress",
           "-o", str(FONT_OUT)]
    print("  converting with lv_font_conv")
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        sys.exit("lv_font_conv failed")
    text = FONT_OUT.read_text(encoding="utf-8")
    header = ("/*\n"
              " * Generated by tools/build_emoji_font.py. Do not edit.\n"
              " * Glyphs from Noto Emoji (monochrome), SIL Open Font License 1.1.\n"
              " * Licence text: components/lg_ui/fonts/NotoEmoji-OFL.txt\n"
              " */\n")
    FONT_OUT.write_text(header + text, encoding="utf-8", newline="\n")


def write_header():
    lines = ["/*",
             " * lg_emoji.h - the emoji this firmware can draw, generated by tools/build_emoji_font.py.",
             " * Do not edit: the list must match the glyphs in the font, or a key draws nothing.",
             " *",
             " * Glyphs from Noto Emoji (monochrome), SIL Open Font License 1.1.",
             " */",
             "#pragma once",
             "",
             "#include \"lvgl.h\"",
             "",
             "#ifdef __cplusplus",
             "extern \"C\" {",
             "#endif",
             "",
             f"LV_FONT_DECLARE({FONT_NAME})",
             "",
             f"#define LG_EMOJI_COUNT {len(EMOJI)}u",
             "",
             "/* UTF-8, in keyboard order. */",
             "static const char *const LG_EMOJI[LG_EMOJI_COUNT] = {"]
    for cp, name in EMOJI:
        utf8 = "".join(f"\\x{b:02X}" for b in chr(cp).encode("utf-8"))
        lines.append(f'    "{utf8}",   /* {name} */')
    lines += ["};", "",
              "#ifdef __cplusplus",
              "}",
              "#endif",
              ""]
    HEADER_OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="rebuild even if the font file exists")
    ap.add_argument("--list", action="store_true", help="show the curated set and exit")
    args = ap.parse_args()

    if args.list:
        for cp, name in EMOJI:
            print(f"U+{cp:04X}  {chr(cp)}  {name}")
        print(f"{len(EMOJI)} emoji")
        return 0
    if FONT_OUT.exists() and not args.force:
        print(f"{FONT_OUT.relative_to(ROOT)} exists; pass --force to rebuild")
        return 0

    print("Emoji font")
    fetch(FONT_URL, FONT_TTF)
    fetch(LICENSE_URL, LICENSE_DST)
    instance_and_subset()
    convert()
    write_header()
    size = FONT_OUT.stat().st_size
    print(f"  wrote {FONT_OUT.relative_to(ROOT)} ({size / 1024:.0f} KB of C source)")
    print(f"  wrote {HEADER_OUT.relative_to(ROOT)} with {len(EMOJI)} emoji")
    print("  licence kept at " + str(LICENSE_DST.relative_to(ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
