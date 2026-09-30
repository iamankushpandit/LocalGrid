# Contributing to LocalGrid

Thanks for looking. LocalGrid is a prototype with one maintainer, written in the open: every
architectural question has a numbered answer in [docs/DESIGN_REVIEW.md](docs/DESIGN_REVIEW.md), and
every standing decision is in [docs/DECISIONS.md](docs/DECISIONS.md). Cite them by number (answer 38,
D27) in issues and pull requests; it saves everyone time.

## Where help counts most

### Port a board

A display board is mostly data: pins, panel controller, colour order, inversion, backlight, and
touch wiring in a profile in [`components/lg_board`](components/lg_board). Drivers live in
`lg_bsp`, and screens read the size at runtime (D9), so a new resolution needs no screen changes.
Open a **Board port** issue first with the board's exact name and a photo of its label. Ports are
accepted once the port's owner has run `tests` (`LG_TESTS_RESULT: PASS`) and the handheld firmware
on it and shown the screen working.

### Test on real radios

Range, roaming between access points, recovery after power loss, and battery life all depend on
hardware and on the space. `tools/chaos.py` takes access points and handhelds out at random for
hours and reports how the grid recovered. A captured log from your bench is a valuable contribution
on its own.

### The admin page

[`firmware/node/main/web/admin.html`](firmware/node/main/web/admin.html) is one file served by
every access point. You can work on it with no hardware: `python tools/build_web_preview.py` writes a
copy that answers from a simulated MAIN in your browser. Devices send packed binary and the browser
formats it (D49), so new views are usually browser-only.

### Protocol and crypto review

The core in `components/lg_core` is portable C11 with no ESP-IDF includes. Crypto goes only through
`lg_crypto.h`, with RFC test vectors for every primitive. Careful review of the envelope, nonce
construction, and routing is welcome. Report anything exploitable privately (see
[SECURITY.md](SECURITY.md)).

## Before you start

- **Ask first for behaviour changes.** The maintainer decides requirements. If a change alters what
  a device does, open an issue describing the problem, the options, and your recommendation before
  writing code. Accepted decisions are recorded in `docs/DECISIONS.md`.
- **Read [AGENTS.md](AGENTS.md).** It is short and it is the rulebook: the core stays portable,
  everything is bounded, one task owns core state, nonces never repeat, layers stay separate (D27),
  information is sticky (D48), and devices hold bytes while browsers make text (D49).
- **Use the product's name for it.** LocalGrid is "an offline network" (D19). Camping is one use
  case, not what it is.

## Setting up

Pure ESP-IDF v6.1 with targets `esp32` and `esp32s3` (D1); see the README's **Build it** section.
Generate your own keys with `python tools/gen_secrets.py`. The file is gitignored; never commit it,
and never paste its contents into an issue.

## Testing

**Every test runs on ESP32 boards** (D25). A PC builds, flashes, and reads serial logs; it never
stands in for a handheld or an access point. CI builds every firmware for every target with zero
warnings, but it cannot run anything, so the pull request needs your evidence:

1. `python tools/build.py` succeeds with zero warnings for every firmware and target you touched.
2. After any change to `lg_core` or `lg_crypto`: `tests` prints `LG_TESTS_RESULT: PASS` on a board.
3. For radio or firmware behaviour: captured serial output (`tools/serial_capture.py`) showing it
   working. Firmware flashed to a handheld must show its results on the screen, not only on serial
   (D23).

Before pasting a log, check it for anything private: your grid name, people's names, and message
text. Serial logs never contain keys or 1:1 plaintext by design; tell us if you find otherwise.

## Pull requests

- Branch from and target **`dev`**. `main` is updated from `dev` by the maintainer.
- One change per pull request, with a `CHANGELOG.md` entry under `[Unreleased]`.
- Update the milestone report or `docs/DECISIONS.md` in the same pull request if your change
  affects them.
- Keep public symbols prefixed (`lg_`, or a module prefix such as `hh_`); short names collide with
  Espressif's closed libraries.
- Log network transitions with a bracketed tag (`[NET]`, `[GRID]`, `[TIME]`, ...). Never log keys,
  passphrases, or 1:1 plaintext.

The **verify** check must pass. It builds everything, runs the layer check, confirms no secrets file
is tracked, and regenerates the site.

## Licensing

LocalGrid is released under the **GNU GPL v3 or later** (`GPL-3.0-or-later`); see
[LICENSE](LICENSE). By opening a pull request you agree your contribution is licensed under those
terms. There is no contributor licence agreement and no sign-off to add. Third-party material keeps
its own licence, recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

## Conduct

Everyone taking part agrees to the [Code of Conduct](CODE_OF_CONDUCT.md).
