# The demo: LocalGrid presents itself

`tools/demo.py` serves a narrated presentation at `http://127.0.0.1:8770/` that explains the project
and then proves each claim live on the bench boards. One button, **Play all**, runs the whole thing
with the browser's own voice reading every line, so a recording needs nobody speaking (D89).

The presentation is in [`tools/demo_story.json`](../tools/demo_story.json), in two halves:

1. **The product, with no cables.** What LocalGrid is, why it exists and how it works, then the grid
   as people would use it: every board on its own battery, the laptop only listening to the access
   points' Bluetooth beacons, and the presenter raising an SOS, pulling a plug and switching everything
   off by hand while the page times what the grid does.
2. **How it was built and tested, with the cables.** Chaos testing; then every board is plugged into
   the laptop, the hardware is read from the boards themselves, the concepts and key decisions are
   explained, and each claim is broken on cue through the cables and measured.

It ends with what else works today, what it is not yet, and where to find it. Every claim cites a
decision or comes from the README; change the file when a claim changes.

## Running it

```bash
python tools/demo.py
```

It starts with no cables, listening over Bluetooth. At "The bench" chapter it asks for every board to
be plugged in, finds each by its own device ID and restarts it, so every measured demo starts from a
known point. `--bench` starts on the cables instead, for the second half alone; `--page-only` serves
the presentation with no boards, to rehearse the words. The page is served to this laptop only.

| Key | Does |
|---|---|
| Space | Play all from this chapter, or pause |
| Left, Right | Previous, next chapter |
| Enter | Present this chapter only |
| Esc | Stop: the running demo ends where it is and the grid is put back |
| R | Reset the grid: every radio on, every board powered |

Each live demo waits for each spoken line to finish before it acts, so what is said and what
happens stay together.

### The narrator's voice

Chosen at the top of the page, most natural first:

- **Piper, on this laptop.** An open-source neural voice, made locally, the same in every recording,
  and offline like the product. Install it once with `pip install piper-tts` and put a voice model
  (an `.onnx` file and its `.onnx.json`, from the Piper voices collection) in `build/voices/`, or name
  one with `--voice-model`. The page then offers it first. The voice is Amy (`en_US-amy-medium`), the
  owner's choice; the others downloaded with it stay in the list.
- **A browser's natural voices.** Microsoft Edge offers "Natural" voices; they sound close to a person
  but are online, so the narration text goes to Microsoft.
- **The voices installed in Windows** (David, Mark, Zira): offline, and robotic.

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
| No cables | Every board on its own power; the laptop listens over Bluetooth only | Batteries or chargers |
| An SOS, by hand | Raised from a handheld's screen; Read pressed on others; then Safe | |
| Pull a plug, by hand | Any access point unplugged; its handhelds seen arriving on the others; plugged back | |
| All off, by hand | Every access point off, then on: the grid re-forms and gets its time back | |
| The bench | Every board plugged into the laptop and restarted | Cables |

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

The first half is drawn from the access points' Bluetooth beacons (D68), the same ones
`tools/grid_watch.py` reads, and the laptop can only listen. Give each access point a battery or a
charger; the handhelds run on their own. The hands-on chapters wait up to three minutes for the
presenter, then time what the grid does. If the session is on the cables, the "No cables" chapter
lets go of every port first, and the cables can then be pulled.

## Photos and clips

A chapter can show the owner's photos or short clips under its stage. They are looked for in
`build/demo-media/` (not in git, because they show the bench) by the names the story gives, and a
chapter whose photo is not there yet shows none. The shot list:

| File | Chapter | What |
|---|---|---|
| `hero-handheld.jpg` | LocalGrid | A handheld in a hand, Messages screen on, ideally outdoors |
| `handhelds-row.jpg` | LocalGrid | Every handheld model side by side, screens on |
| `bench-wide.jpg` | How it works | The whole grid on one table |
| `batteries.jpg` | No cables | Access points on battery packs, no laptop in sight |
| `sos-lockscreen.jpg` | An SOS, by hand | An SOS on a locked handheld |
| `sos-read-by.jpg` | An SOS, by hand | The "Read by" line under the sender's SOS |
| `android-watch.png` | Watching it | A screenshot of the Android app (no Map tab) |
| `ap-lora.jpg`, `ap-gps.jpg` | Hardware | An access point with its LoRa module; MAIN's GPS |
| `handheld-back.jpg` | Hardware | The back of a handheld with its LoRa module wired on |
| `status-screen.jpg`, `keyboard.jpg`, `screensaver.jpg` | The handhelds | Three of the handheld's screens |
| `talk.mp4` | Voice | A few seconds of push to talk, muted (it loops) |
| `installer.png` | Getting started | The installer page part-way through an install |

Photos at 1600 px on the long side are plenty. Keep coordinates, faces and addresses out of frame.

## Before recording

- **Positions.** "Watching it" shows `tools/grid_watch.py` without the admin password, so it shows
  the beacon only. Do not log in on camera: the Map tab would show where the bench is.
- **Sound.** An SOS sounds on every handheld, even with the volume off (D40).
- **Run each chapter once** with `Present this chapter` before Play all. A live demo that fails on
  camera is worse than none; every result it shows is what the boards printed, including failures.
- Session logs are kept in `chaos-runs/demo/<start time>/`, which git ignores.
