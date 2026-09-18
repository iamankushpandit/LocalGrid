# Contributing to LocalGrid (draft)

> **Not accepting outside contributions yet.** LocalGrid has no licence. Until the owner chooses one, nobody else has the right to use, change, or share this code, and the project cannot accept a pull request from anyone but the owner. This guide is prepared so the project can open quickly once that decision is made. Board requests through issues are welcome as information; code is not.
>
> **TODO (owner): licence.** Choose a licence, add `LICENSE`, and update this notice, `THIRD_PARTY.md`, and the templates in `.github/`.
>
> **TODO (owner): contributor sign-off.** Decide whether contributions need a Developer Certificate of Origin sign-off (`git commit -s`) or a contributor agreement, and describe it here.

LocalGrid is an offline, self-forming ESP32 messaging network (D19): APs (D43) carry 1:1, group, and broadcast text between touchscreen handhelds, with no Internet at runtime. It is a prototype.

## Scope

The contribution the project is being prepared for first is **hardware support**: a new handheld board, a new panel, touch controller, or codec driver. [`docs/BOARDS.md`](docs/BOARDS.md) is the guide. Protocol, security, and product behaviour are the owner's decisions (see below); propose them in an issue before writing code.

## Building

- ESP-IDF v6.1. Load it first (`. C:\esp\v6.1\esp-idf\export.ps1` in PowerShell, or `. $IDF_PATH/export.sh`).
- Generate throwaway prototype secrets once: `python tools/gen_secrets.py`. The output, `firmware/common/lg_secrets.h`, is gitignored; never commit it. Every board in one grid must be built from the same file.
- Build every firmware type for every target: `python tools/build.py`. It refuses builds with warnings and runs the layer check before and after.
- Flash with `python tools/flash.py`; see the `build` and `flash` skills in `.claude/skills/` for every option.

CI (`.github/workflows/build.yml`) runs the same build with fresh throwaway secrets on every push and pull request.

## Rules

The full list is in [`AGENTS.md`](AGENTS.md#rules); these are the ones a contribution most often meets.

- **Layers stay separate (D27).** Infrastructure never includes UI headers. `lg_bsp` is the only UI-side code that includes ESP-IDF drivers, and `lg_draw` reaches hardware only through it. Screens and services meet only through `hh_service.h`. LVGL is retired (D55). `python tools/check_layers.py` checks this.
- **No pixel constants or colours outside board profiles and the theme (D9, D10).** Screens read the screen size at runtime and style through the theme table.
- **Zero warnings** on every firmware type and target under ESP-IDF v6.1 defaults. Fix the warning; keep the warning level.
- **`CHANGELOG.md` entry** for every change, newest first.
- **Decisions are the owner's.** `docs/DECISIONS.md` changes only when the owner decides. If a requirement looks wrong or hardware blocks it, explain the problem, the limit, the options, and a recommendation in an issue.
- **Never log keys, passphrases, or the plaintext of 1:1 messages**, and never put hardware addresses in the repository or tool output (D21).
- **Core stays portable and bounded.** `lg_core` includes only C standard headers and its own; fixed-size tables, no heap allocation per message.
- **Tests for core changes.** A change to `lg_core` or `lg_crypto` needs scenarios in `tests/target` and `LG_TESTS_RESULT: PASS` on a board. See the `protocol-change` skill.
- **Test on ESP32 boards only (D25).** The PC builds, flashes, and reads serial logs; it never stands in for a handheld or an AP. Handheld firmware, test builds included, shows its results on the screen (D23).
- **Call it an offline network (D19)** and the infrastructure boards APs (D43) in everything a person reads.

## Provenance

LocalGrid must be able to choose its own licence, so everything in the tree needs a known origin.

- Write code yourself, against ESP-IDF. Do not copy code from other projects.
- Facts are not code: pins, register meanings, and divider ratios may be restated from datasheets, vendor pin tables, or other projects, with the source named in a comment.
- Braino (github.com/iamankushpandit/Gume, GPLv3) is the owner's project and the source of the measured board facts for the existing handhelds. Restate its facts; never copy its code. The owner can reuse his own code; contributors cannot.
- Record every outside source in [`THIRD_PARTY.md`](THIRD_PARTY.md): the project, its licence, the file here that uses it, and what was taken.

## Submitting

Use the pull request template. For a board, attach the bring-up evidence from `docs/BOARDS.md`. For a board request, use the "New board" issue template.
