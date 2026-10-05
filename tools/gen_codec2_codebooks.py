"""Generate Codec2's codebook C files from its text tables.

Codec2 builds these with a small host program, generate_codebook.c. This bench has no host C compiler,
and a build step that needs one would be a second toolchain for one job, so this does the same thing in
Python and the results are committed in components/codec2/src. Run it again only when the text tables in
components/codec2/codebook change (they come from Codec2 1.2.0, LGPL-2.1; see THIRD_PARTY.md).

    python tools/gen_codec2_codebooks.py
"""
import math
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent / "components" / "codec2"
SETS = {
    "codebook.c": ("lsp_cb", [f"lsp{i}" for i in range(1, 11)]),
    "codebookd.c": ("lsp_cbd", [f"dlsp{i}" for i in range(1, 11)]),
    "codebookjmv.c": ("lsp_cbjmv", ["lspjmv1", "lspjmv2", "lspjmv3"]),
    "codebookge.c": ("ge_cb", ["gecb"]),
}
HEADER = """/* THIS IS A GENERATED FILE. Edit tools/gen_codec2_codebooks.py and its input */

/*
 * This intermediary file and the files that used to create it are under
 * The LGPL. See the file COPYING.
 */

#include "defines.h"

"""
NUMBER = re.compile(r"[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)")


def load(path):
    text = "\n".join(line.split("#", 1)[0] for line in path.read_text().splitlines())
    values = [float(x) for x in NUMBER.findall(text)]
    k, m = int(values[0]), int(values[1])
    body = values[2:2 + k * m]
    if len(body) != k * m:
        raise SystemExit(f"{path.name}: wanted {k * m} values, found {len(body)}")
    return k, m, body


def main():
    for out_name, (symbol, names) in SETS.items():
        books = [(n, *load(ROOT / "codebook" / f"{n}.txt")) for n in names]
        out = [HEADER]
        for index, (name, k, m, body) in enumerate(books):
            out.append(f"  /* {name}.txt */\n")
            out.append(f"static const float codes{index}[] = {{\n")
            for i, v in enumerate(body):
                out.append(f"  {v:g}" + ("," if i < len(body) - 1 else ""))
                if (i + 1) % k == 0:
                    out.append("\n")
            out.append("};\n")
        out.append(f"\nconst struct lsp_codebook {symbol}[] = {{\n")
        for index, (name, k, m, body) in enumerate(books):
            out.append(f"  /* {name}.txt */\n  {{\n    {k},\n    {round(math.log(m) / math.log(2))},\n    {m},\n"
                       f"    codes{index}\n  }},\n")
        out.append("  { 0, 0, 0, 0 }\n};\n")
        (ROOT / "src" / out_name).write_text("".join(out), newline="\n")
        print(f"{out_name}: {symbol}, {len(books)} table(s), {sum(len(b[3]) for b in books)} values")


if __name__ == "__main__":
    main()
