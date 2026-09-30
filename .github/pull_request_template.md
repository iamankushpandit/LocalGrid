## What changed

<!-- One or two sentences: what does a device do now that it did not before? -->

## Why

<!-- The problem, not the patch. Link the issue and any decision (D-number) it implements. -->

## How it was tested

<!--
CI builds every firmware for every target and runs the layer check. It cannot run anything: every
test runs on ESP32 boards (D25). Say what you ran. Remove grid names, people's names, and message
text from any log you paste.
-->

- [ ] `python tools/build.py` builds everything I touched with zero warnings
- [ ] `tests` prints `LG_TESTS_RESULT: PASS` on a board (required for `lg_core` or `lg_crypto` changes)
- [ ] Flashed to boards; serial output below shows the change working
- [ ] Handheld changes checked on the screen, not only on serial (D23)
- [ ] `python tools/check_layers.py` passes (D27); no pixel values or colours outside board profiles and the theme (D9, D10)
- [ ] No keys, passphrases, 1:1 plaintext, or hardware addresses in code, logs, or evidence (D21)

```text
(serial output)
```

## Docs in the same pull request

- [ ] `CHANGELOG.md` entry under `[Unreleased]`
- [ ] `docs/DECISIONS.md` if a decision changed
- [ ] The milestone report, if this changes its procedure or results
- [ ] Facts taken from another project recorded in `THIRD_PARTY.md`, with no copied code
- [ ] Nothing above applies

<!-- Target `dev`. Please leave "Allow edits by maintainers" ticked. -->
