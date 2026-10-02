#!/usr/bin/env python3
"""Build the screen saver's glyph font: half-width katakana, mirrored, from Noto Sans JP (OFL-1.1).

Why these glyphs. The film's falling code is not Latin and not random symbols: Simon Whiteley built it
from **half-width katakana**, taken from his wife's Japanese cookbook, and **mirrored** them, mixed with a
few Latin digits. Our saver was dropping `0123456789ABCDEF...<>*+-/|=`, which reads as "terminal output"
rather than as the film, and the owner noticed exactly that (2026-10-01).

Montserrat has no katakana, so the glyphs have to come from somewhere. Noto Sans JP is used for the same
reason Noto Emoji is used for the emoji font: it is SIL Open Font License 1.1, so its glyphs may be
embedded in a GPL-3.0-or-later firmware and redistributed with it. A Windows system font could not be.

Steps, all offline after the first download, exactly as tools/build_emoji_font.py:
  1. Download NotoSansJP[wght].ttf and OFL.txt into tools/.cache (gitignored).
  2. Instance the variable font at one weight and subset it to the glyphs below (fontTools).
  3. Mirror every glyph horizontally, which is the film's signature (fontTools pens).
  4. Convert with lv_font_conv (npm, via npx), then to lg_draw's font types with convert_font.py (D55).
  5. Write components/lg_draw/include/lg_matrix.h so the saver and the font stay in step.

  python tools/build_matrix_font.py              build if the font file is missing
  python tools/build_matrix_font.py --force      rebuild
  python tools/build_matrix_font.py --preview p.png   render a sheet of the glyphs to look at

The generated font and header are committed, so a normal build needs neither Node nor a download.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / "tools" / ".cache"
FONT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf"
LICENSE_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notosansjp/OFL.txt"
FONT_TTF = CACHE / "NotoSansJP-variable.ttf"
FONT_STATIC = CACHE / "NotoSansJP-static.ttf"
FONT_SUBSET = CACHE / "NotoSansJP-subset.ttf"
FONT_MIRROR = CACHE / "NotoSansJP-mirrored.ttf"
FONT_DIR = ROOT / "components" / "lg_draw" / "fonts"
LICENSE_DST = FONT_DIR / "NotoSansJP-OFL.txt"
HEADER_OUT = ROOT / "components" / "lg_draw" / "include" / "lg_matrix.h"

# 12 px, to match F_SMALL (lg_font_montserrat_12): the saver's cell size comes from the font it draws
# with, and a taller font would change the rain's grid on every board at once.
SIZE_PX = 12
# 500 rather than 400: at 12 px a mirrored katakana stroke thins out, and the film's glyphs read as solid.
WEIGHT = 500
BPP = 4

# Half-width katakana, U+FF66 to U+FF9D - the film's alphabet. Full-width katakana would be square and
# twice as wide, which is wrong for a narrow falling column.
KATAKANA = list(range(0xFF66, 0xFF9E))
# And the Latin digits that appear among them. Mirrored like everything else, which is why 0-9 is the
# whole of the Latin set: mirrored letters look like a broken font, mirrored digits look like the film.
DIGITS = list(range(0x30, 0x3A))
GLYPHS = KATAKANA + DIGITS


def fetch(url, path):
    if path.exists() and path.stat().st_size > 0:
        print(f"  have {path.name}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  downloading {path.name}")
    with urllib.request.urlopen(url, timeout=300) as r, open(path, "wb") as f:
        shutil.copyfileobj(r, f)


def instance_and_subset():
    from fontTools import subset as ft_subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    print(f"  instancing weight {WEIGHT}")
    font = TTFont(FONT_TTF)
    instancer.instantiateVariableFont(font, {"wght": WEIGHT}, inplace=True, updateFontNames=False)
    font.save(FONT_STATIC)

    cmap = TTFont(FONT_STATIC).getBestCmap()
    missing = [f"U+{cp:04X}" for cp in GLYPHS if cp not in cmap]
    if missing:
        sys.exit("font has no glyph for: " + ", ".join(missing))

    print(f"  subsetting to {len(GLYPHS)} glyphs")
    ft_subset.main([
        str(FONT_STATIC),
        f"--output-file={FONT_SUBSET}",
        "--unicodes=" + ",".join(f"U+{cp:04X}" for cp in GLYPHS),
        "--layout-features=",
        "--no-hinting",
        "--desubroutinize",
        "--drop-tables+=GSUB,GPOS,morx,COLR,CPAL,CBDT,CBLC,sbix,SVG",
    ])


def mirror():
    """Flip every glyph horizontally about the middle of its own advance width.

    The film's glyphs are mirrored, and it is the single thing that most makes them read as "the Matrix"
    rather than as Japanese text: a reader who knows katakana sees something almost-but-not-quite legible.

    Each glyph is decomposed first (DecomposingRecordingPen), so a composite glyph becomes contours before
    it is transformed - transforming a composite would move its components and mirror nothing.
    """
    from fontTools.ttLib import TTFont
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    font = TTFont(FONT_SUBSET)
    glyf, hmtx, glyphset = font["glyf"], font["hmtx"], font.getGlyphSet()
    made = {}
    for name in font.getGlyphOrder():
        adv, _lsb = hmtx[name]
        rec = DecomposingRecordingPen(glyphset)
        glyphset[name].draw(rec)
        pen = TTGlyphPen(None)
        rec.replay(TransformPen(pen, (-1, 0, 0, 1, adv, 0)))   # x -> adv - x
        made[name] = (pen.glyph(), adv)
    for name, (g, adv) in made.items():
        glyf[name] = g
        g.recalcBounds(glyf)
        hmtx[name] = (adv, getattr(g, "xMin", 0))
    font.save(FONT_MIRROR)
    print(f"  mirrored {len(made)} glyph(s)")


def preview(path, source):
    """Render the glyphs to a PNG, so a human can see them before they are committed."""
    from PIL import Image, ImageDraw, ImageFont
    cell, cols = 28, 16
    rows = (len(GLYPHS) + cols - 1) // cols
    img = Image.new("RGB", (cols * cell, rows * cell), (0, 0, 0))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(str(source), 20)
    for n, cp in enumerate(GLYPHS):
        x, y = (n % cols) * cell, (n // cols) * cell
        d.text((x + cell // 2, y + cell // 2), chr(cp), font=f, fill=(0, 255, 70), anchor="mm")
    img.save(path)
    print(f"  preview written to {path}")


def font_name():
    return f"lg_font_matrix_{SIZE_PX}"


def font_out():
    return FONT_DIR / f"{font_name()}.c"


def convert(source):
    raw = CACHE / f"{font_name()}.lvgl.c"
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if npx is None:
        sys.exit("npx not found; Node.js is needed to run lv_font_conv")
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [npx, "--yes", "lv_font_conv@1.5.3",
           "--font", str(source),
           "--size", str(SIZE_PX),
           "--bpp", str(BPP),
           "--range", ",".join(f"0x{cp:X}" for cp in GLYPHS),
           "--format", "lvgl",
           "--lv-include", "lvgl.h",
           "--no-compress",
           "-o", str(raw)]
    print(f"  converting at {SIZE_PX} px with lv_font_conv")
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        sys.exit("lv_font_conv failed")
    sys.path.insert(0, str(ROOT / "tools"))
    import convert_font
    text = convert_font.convert(raw.read_text(encoding="utf-8"), font_name(), [
        "The screen saver's falling glyphs: half-width katakana, mirrored, and the digits.",
        "Generated by tools/build_matrix_font.py; do not edit.",
        "Glyphs from Noto Sans JP, SIL Open Font License 1.1.",
        "Licence text: components/lg_draw/fonts/NotoSansJP-OFL.txt"])
    font_out().write_text(text, encoding="utf-8", newline="\n")


def write_header():
    lines = ["/*",
             " * lg_matrix.h - the screen saver's falling glyphs, generated by tools/build_matrix_font.py.",
             " * Do not edit: the list must match the glyphs in the font, or the saver draws blanks.",
             " *",
             " * Half-width katakana (U+FF66-U+FF9D) and the digits, every one mirrored, which is what the",
             " * film's code actually was. Glyphs from Noto Sans JP, SIL Open Font License 1.1.",
             " */",
             "#pragma once",
             "",
             '#include "lg_font.h"',
             "",
             "#ifdef __cplusplus",
             'extern "C" {',
             "#endif",
             "",
             f"extern const lg_font_t {font_name()};",
             "",
             f"#define LG_MATRIX_COUNT {len(GLYPHS)}u",
             "",
             "/* UTF-8, one string per glyph. */",
             "static const char *const LG_MATRIX_GLYPHS[LG_MATRIX_COUNT] = {"]
    for cp in GLYPHS:
        utf8 = "".join(f"\\x{b:02X}" for b in chr(cp).encode("utf-8"))
        lines.append(f'    "{utf8}",   /* U+{cp:04X} */')
    lines += ["};", "",
              "#ifdef __cplusplus",
              "}",
              "#endif",
              ""]
    HEADER_OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"  wrote {HEADER_OUT.relative_to(ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="rebuild even if the font file exists")
    ap.add_argument("--no-mirror", action="store_true", help="leave the glyphs the right way round")
    ap.add_argument("--preview", metavar="PNG", help="also render a sheet of the glyphs to this PNG")
    args = ap.parse_args()

    if font_out().exists() and not args.force:
        print(f"{font_out().relative_to(ROOT)} exists; --force to rebuild")
        return
    fetch(FONT_URL, FONT_TTF)
    fetch(LICENSE_URL, LICENSE_DST)
    instance_and_subset()
    source = FONT_SUBSET
    if not args.no_mirror:
        mirror()
        source = FONT_MIRROR
    if args.preview:
        preview(args.preview, source)
    convert(source)
    write_header()
    print(f"  wrote {font_out().relative_to(ROOT)} "
          f"({font_out().stat().st_size // 1024} KB of C)")
    print("Add fonts/" + font_out().name + " to components/lg_draw/CMakeLists.txt if it is new.")


if __name__ == "__main__":
    main()
