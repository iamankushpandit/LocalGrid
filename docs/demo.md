# The demo: LocalGrid presents itself

`tools/demo.py` serves a narrated presentation at `http://127.0.0.1:8770/` that explains the project
and then proves each claim live on the bench boards. One button, **Play all**, runs the whole thing
with the browser's own voice reading every line, so a recording needs nobody speaking (D89).

The presentation is in [`tools/demo_story.json`](../tools/demo_story.json): what LocalGrid is, why it
exists, how it works, the hardware (read from the boards themselves), the concepts, key decisions,
and how it was chaos tested. Then come the live chapters, the Bluetooth view, a part with no cables
at all, and what it is not yet. Every claim in it cites a decision or comes from the README; change
the file when a claim changes.

## Running it

```bash
python tools/demo.py
```

Every board on a USB cable is found by its own device ID and restarted, so the demo starts from a
known point. The access points then form the grid by themselves, and the page reports how long that took.
The page is local to this laptop and talks to nothing else. `--page-only` serves the presentation with
no boards, to rehearse the words.

| Key | Does |
|---|---|
| Space | Play all from this chapter, or pause |
| Left, Right | Previous, next chapter |
| Enter | Present this chapter only |
| Esc | Stop: the running demo ends where it is and the grid is put back |
| R | Reset the grid: every radio on, every board powered |

The narrator's voice is chosen at the top of the page; voices installed on the laptop work offline.
Each live demo waits for each spoken line to finish before it acts, so what is said and what
happens stay together.

## The live chapters

Each one checks the grid is ready, says what is about to happen, does it through the cables, shows it
on a live map with a stopwatch, ends with what the boards themselves printed, then puts the grid back.
Faults go through the cable and no other way (D103); the handhelds send and receive every message
themselves, and the laptop never joins the grid (D25).

| Chapter | What happens | Needs |
|---|---|---|
| The handhelds | Status, messages, groups and settings opened on every handheld's screen | |
| An emergency | An SOS, timed to every other screen, with the LoRa frames that carried it too; then the all clear | Two handhelds |
| Voice | Push to talk to everyone with a generated voice; frames played per listener | Two handhelds, one with a speaker |
| Losing an access point | Power cut on the busiest one; each handheld's move timed; a message across; power back | A holdable port (CP210x, CH34x, FTDI) |
| A crash | A restart; links, time, settings, groups and handhelds timed back | |
| The second radio | Wi-Fi between access points off everywhere; a message crosses by LoRa | LoRa modules on the APs |
| Out of range | A handheld's Wi-Fi off; messages both ways over LoRa | A handheld with a LoRa module |
| A split grid | One access point cut off; a group renamed on its side; the halves meet and agree | Handhelds on two APs |
| The worst case | Every access point's power cut for 20 s; links, time and handhelds timed back | Holdable ports |
| Updates | A signed handheld update given to one AP spreads by itself, then is withdrawn | `--update FILE` |
| Getting started | A board is installed from the installer page in a real browser and joins | `--install-board NAME` |
| No cables | The laptop lets go of every port; the presenter unplugs them | Batteries or chargers |
| By hand | Pull a plug, raise an SOS from a screen, switch everything off: the laptop sees each over Bluetooth | |

A chapter that needs setup says what in the chapter list, and Play all passes over it.

### Updates (`--update`)

Sign the current handheld build with the release key the boards were given:

```bash
python tools/lgfw.py sign --key <release key> --role H --chip esp32 --version demo firmware/handheld/build-esp32/lg_handheld.bin -o hh-demo.lgfw
python tools/demo.py --update hh-demo.lgfw
```

The same code with a newer serial, so the handhelds restart into what they already run. Only a
handheld given that release key takes it (D97); the others refuse, and the page says so. It takes
minutes: handhelds wait until they are idle, including five minutes after an urgent message. The demo
ends with `spread drop`, which withdraws the update from the whole grid.

### Getting started (`--install-board`)

`tools/test_installer_page.py` installs the named bench handheld through the installer page in a real
Chrome or Edge window, which is what the camera should see. It needs `tools/build_release.py` to have
built the published images. The board's settings are cleared and its identity kept; it then runs the
published image, not the bench build, so flash it again with `tools/flash.py` before a chaos run.

## No cables

The part after "Watching it" lets go of every cable. From then on the page is drawn from the access
points' Bluetooth beacons (D68), the same ones `tools/grid_watch.py` reads, and the laptop can only
listen. Give each access point a battery or a charger before unplugging it: taking a cable out
otherwise cuts its power, which the grid survives, but it is not the point being made.

To start with no cables at all, for a demo away from the bench:

```bash
python tools/demo.py --untethered
```

The hands-on chapters wait up to three minutes for the presenter, then time what the grid does.

## Before recording

- **Positions.** "Watching it" shows `tools/grid_watch.py` without the admin password, so it shows
  the beacon only. Do not log in on camera: the Map tab would show where the bench is.
- **Sound.** An SOS sounds on every handheld, even with the volume off (D40).
- **Run each chapter once** with `Present this chapter` before Play all. A live demo that fails on
  camera is worse than none; every result it shows is what the boards printed, including failures.
- Session logs are kept in `chaos-runs/demo/<start time>/`, which git ignores.
