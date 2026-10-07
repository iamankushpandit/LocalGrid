# The demo: LocalGrid presents itself

`tools/demo.py` serves a narrated presentation at `http://127.0.0.1:8770/` that explains the project
and then proves each claim live on the bench boards. One button, **Play all**, runs the whole thing
with a narrator's voice made on this laptop, so a recording needs nobody speaking (D89).

The presentation is in [`tools/demo_story.json`](../tools/demo_story.json), in two halves:

1. **The product, with no cables.** What LocalGrid is, its three radios, ESP-NOW, how a handheld
   joins, and the boards on the bench. Then the grid as people would use it: every board on its own
   power, the laptop only listening to the access points' Bluetooth beacons, and the presenter pulling
   a plug and switching everything off while the page times what the grid does, down to the handhelds
   spreading themselves out again. Then the watcher, tab by tab.
2. **How it was built and tested, with the cables.** Chaos testing; then every board is plugged into
   the laptop, the hardware is read from the boards, the core ideas and key decisions are explained,
   and everything else is driven through the cables and measured: messages to one person, a group and
   everyone, push to talk with real speech, a new name, the SOS, every kind of failure, both ways of
   updating, and installing a board from the web page.

It ends with what else works today, how it can be extended, where it fits, what it is not yet, and
where to find it. Every claim cites a decision or comes from the README; change the file when a claim
changes.

## Running it

```bash
python tools/demo.py --sign-key <release key .pem> --install-board e28 --release <release key .pub.hex>
```

It starts with no cables, listening over Bluetooth. At "The bench" it asks for every board to be
plugged in, finds each by its own device ID and restarts it, so every measured demo starts from a
known point, and sets every handheld's volume to high. `--bench` starts on the cables instead, for the
second half alone; `--page-only` serves the presentation with no boards, to rehearse the words. The
page is served to this laptop only. Set `LG_ADMIN_PASSWORD` in the environment to have the watcher log
in by itself; otherwise its chapter asks the presenter to log in.

| Key | Does |
|---|---|
| Space | Play all from this chapter, or pause; Continue on a step only the presenter can do |
| Left, Right | Previous, next chapter |
| Enter | Present this chapter only |
| C | The chapter list (it also slides out briefly between chapters) |
| L | Every board's own output, live, or the Bluetooth event log with no cables |
| Esc | Stop: the running demo ends where it is and the grid is put back |
| R | Reset the grid: every radio on, every board powered |

Each live demo waits for each spoken line to finish before it acts, so what is said and what happens
stay together. Everything is drawn on the site's own network diagram (`assets/network-diagram.svg`):
explanations zoom into it and make the part being talked about glow, and the live chapters draw the
bench's boards on it.

### Recording, one chapter a video

Tick **Recording** in the header: the controls disappear, and **Present this chapter** (Enter) plays
the chapter between a title card and a held last frame, a clean cut. Open a chapter directly with its
id in the address, for example `http://127.0.0.1:8770/#d-voice`.

### The narrator's voice

Chosen at the top of the page. The default is Amy (`en_US-amy-medium`), a Piper voice: open-source,
made on this laptop, the same in every recording and offline like the product. Install it once with
`pip install piper-tts` and put voice models (an `.onnx` file and its `.onnx.json`) in `build/voices/`.
A browser's own voices are offered after it.

## The live chapters

| Chapter | What happens | Needs |
|---|---|---|
| Pulling a plug | The presenter unplugs any access point; the laptop sees it go quiet over Bluetooth and its handhelds arrive on the others | No cables |
| Everything off | Every access point off, then on: the grid re-forms, gets its time back, and the handhelds spread out (D54) | No cables |
| Watching the grid | The watcher's overview, then after a login its Network, Handhelds and Traffic tabs (the Map stays shut) | `LG_ADMIN_PASSWORD`, or the presenter logs in |
| The handheld screens | Status, messages, groups and settings opened on every handheld | Cables |
| One to one, a group, an announcement | Each sent by a handheld, timed to every screen it should reach, with the conversations open | Cables |
| Push to talk | Real speech, made here with Piper and given to a handheld over its cable (D112), to everyone, the group and one person; frames played per listener, and that nobody else played the 1:1 talk | Cables |
| A new name | A handheld renames itself; the time until another knows it | Cables |
| An SOS, timed | To every screen, with the LoRa frames that carried it too; then the all clear | Cables |
| A power cut, a crash, a handheld restarting | Each timed back, with what came back | Holdable ports for the power cut |
| LoRa between access points, a handheld on LoRa | Wi-Fi taken away; messages and voice cross by LoRa | LoRa modules |
| A split grid, a blackout | Halves that agree again; everything off and back, with the load balancing | Holdable ports |
| Updates | A fresh signed update by cable to one access point, spreading by itself | `--sign-key` |
| An update from a browser | The laptop joins the grid's Wi-Fi, the admin page takes the update, the grid passes it on; the laptop goes back to its own network | `--sign-key`, the presenter at the upload |
| Installing from a web page | A board installed through the installer page in a real browser, then joining | `--install-board`, `tools/build_release.py` images |

A chapter that needs setup says what in the chapter list, and Play all passes over it.

### Updates (`--sign-key`)

Each update chapter signs a fresh update from the current handheld build with the release key the
boards were given, because a device takes only an update newer than the last one it saw. It is the
same code with a newer serial, so the handhelds restart into what they already run. Only a device
given that release key takes it (D97). It takes minutes: handhelds wait until they are idle, including
five minutes after an urgent message. Each ends with `spread drop`, which withdraws the update from the
whole grid.

The browser update has the laptop join the grid's Wi-Fi with a profile made from the grid's secrets
file, which is deleted afterwards, and then rejoin the network it was on. Nothing else on the laptop
reaches the Internet meanwhile; the page and the narrator are served from the laptop itself.

### Installing from a web page (`--install-board`)

`tools/test_installer_page.py` installs the named bench handheld through the installer page in a real
Chrome or Edge window, which is what the camera should see. It needs `tools/build_release.py` to have
built the published images. The board's settings are cleared and its identity kept; it then runs the
published image, not the bench build, so flash it again with `tools/flash.py` before a chaos run.

## Photos and clips

A chapter can show the owner's photos or short clips under its stage, or one full size on the stage at
a given point. They are looked for in `build/demo-media/` (not in git, because they show the bench) by
the names the story gives, and a chapter whose photo is not there yet shows none. The shot list:

| File | Chapter | What |
|---|---|---|
| `hero-handheld.jpg` | LocalGrid | A handheld in a hand, Messages screen on, ideally outdoors |
| `handhelds-row.jpg` | LocalGrid, the demo's hardware | Every handheld model side by side, screens on |
| `bench-wide.jpg` | The demo's hardware | The whole grid on one table |
| `batteries.jpg` | No cables | Access points on battery packs, no laptop in sight |
| `android-watch.png` | Watching the grid | A screenshot of the Android app (no Map tab) |
| `ap-lora.jpg`, `ap-gps.jpg` | Hardware | An access point with its LoRa module; Everest's GPS |
| `handheld-back.jpg` | Hardware | The back of a handheld with its LoRa module wired on |
| `status-screen.jpg`, `keyboard.jpg`, `screensaver.jpg` | The handheld screens | Three of the handheld's screens |
| `installer.png` | Installing from a web page | The installer page part-way through an install |

Photos at 1600 px on the long side are plenty. Keep coordinates, faces and addresses out of frame.

## Before recording

- **Positions.** The watcher's Map tab shows where the bench is: the demo never opens it.
- **Sound.** An SOS sounds on every handheld, even with the volume off (D40).
- **Run each chapter once** with Present this chapter before recording it. A live demo that fails on
  camera is worse than none; every result it shows is what the boards printed, including failures.
- Session logs are kept in `chaos-runs/demo/<start time>/`, which git ignores.
