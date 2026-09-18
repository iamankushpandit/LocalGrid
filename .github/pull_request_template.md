<!--
LocalGrid is not accepting outside contributions yet: it has no licence. Only the owner's pull
requests can be merged until one is chosen. TODO (owner): licence, and whether commits need a
contributor sign-off (DCO). See CONTRIBUTING.md.
-->

## What and why

<!-- One or two plain sentences. Cite the decision (Dnn) this follows. -->

## Checklist

- [ ] `python tools/build.py` builds every firmware type and target with zero warnings (ESP-IDF v6.1)
- [ ] `python tools/check_layers.py` passes (D27)
- [ ] No pixel values or colours outside board profiles and the theme (D9, D10)
- [ ] `lg_core` or `lg_crypto` changed: scenarios in `tests/target` and `LG_TESTS_RESULT: PASS` on a board
- [ ] No keys, passphrases, 1:1 plaintext, or hardware addresses in code, logs, or evidence (D21)
- [ ] `CHANGELOG.md` entry
- [ ] Facts from any other project recorded in `THIRD_PARTY.md`, with no copied code
- [ ] `docs/DECISIONS.md` untouched, or the owner decided the change

## Board bring-up (for a board or driver change)

Board code and name:

Evidence from the checklist in `docs/BOARDS.md` (attach logs as text, photos of the screen):

- [ ] 1. `tests` firmware: `LG_TESTS_RESULT: PASS`, results on screen
- [ ] 2. Boot: self test passes, `[UI] Launcher ready` at the panel's size
- [ ] 3. Colours, orientation, backlight
- [ ] 4. Touch: calibration (resistive) and `touch` raw-to-screen at four corners and the centre
- [ ] 5. Every screen at runtime size: home, Status, messages, groups, settings, chat, keypad and keyboard pages, lock, alerts
- [ ] 6. Sound cues and volume steps (or: no audio)
- [ ] 7. Push-to-talk playback (or: no audio)
- [ ] 8. Microphone: `mic level`, `mic loop`, push-to-talk sending (or: cannot record)
- [ ] 9. `power` against a multimeter, and the battery badge (or: no supply sense)
- [ ] 10. `gps` (or: no GPS UART or no module)
- [ ] 11. Wi-Fi join and registration with an AP
- [ ] 12. A 1:1 message both ways
- [ ] 13. Optional: short chaos run report

Not tested, and why:
