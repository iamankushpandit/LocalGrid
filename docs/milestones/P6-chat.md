# Prototype milestone 6: chat on the handhelds

Status: **messaging verified on hardware over the console; screens built and flashed, on-screen checks by the owner pending.** Ordinary 1:1 and group messages cannot be accepted by the grid until the admin sets grid time (decision D6), so their hardware check is blocked on that.

## What exists

- **Service layer** (`main/service/hh_service.c`): sending and receiving 1:1 (end-to-end encrypted), group, and broadcast text through `lg_client`.
  - The UI and console queue a message; the network task drains the queue, because it is the only owner of `lg_client`.
  - A 24-message ring records what was sent and received. Our own messages carry their state: sending, accepted, delivered, rejected by the grid, or refused by this handheld with the reason.
  - The snapshot carries the roster's groups and whether this handheld belongs to each.
- **Screens** (`main/ui/ui_chat.c`):
  - Conversation list: Everyone, each group this handheld belongs to, and each handheld it has heard about with its online state. A group this handheld is not a member of never appears, because it could neither send nor read it.
  - Chat view: messages oldest first, ours on the right with their state, received ones on the left with the sender's name, each with a clock reading. Urgent messages are outlined in the warning colour.
  - Writing: a one-line text area capped at 240 bytes, a Send button, and LVGL's keyboard in the lower half (decision D8) with letters, capitals, numbers, and symbols pages.
  - Everyone has an Urgent toggle, the only thing that can be sent while grid time is unset.
- **Console** (`hh_console.c`), so messaging can be exercised without touching the glass: `send <device|group|all|urgent> <text>`, `msgs [count]`, `groups`.

## Build and flash

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
python tools/build.py --firmware handheld
python tools/flash.py --role H
```

## Test procedure

**Over the console**, which needs no screen and no grid time:

```powershell
python tools/console.py fnk0104b groups
python tools/serial_capture.py --ports COM9 COM11 --seconds 70 --reset `
  --send "COM9@14:send urgent hello from handheld one" `
  --send "COM9@22:send 2 private hello" `
  --send "COM9@36:msgs" --send "COM11@48:msgs"
```

One capture session is used rather than separate `console.py` calls, because opening a port resets the board and clears its message ring.

**On the screens** (decisions D23, D25), once the admin has set grid time:

1. **Broadcast.** On each handheld: Messages, Everyone, type a word, Send. Pass: it appears on the sender's right marked sent, and on the other handheld's left with the sender's name.
2. **1:1 encrypted.** Open the other handheld's name, send. Pass: the sender shows **delivered**, which only the recipient's own confirmation can produce.
3. **Group.** Open FAMILY on both and send. Pass: both see it. Then open KIDS: it is absent on Handheld 1, which is not a member.
4. **Urgent while time is unset.** Ask the admin to clear time, or test before setting it: Everyone with Urgent on goes out; a 1:1 attempt is refused on screen with the grid-time reason.
5. **Keyboard.** Letters, capitals, numbers, and symbols pages all type into the field; the keyboard occupies the lower half.

## Measured

Bench run, 2026-09-15, grid time unset.

| Check | Result |
|---|---|
| Urgent broadcast, device 1 to device 2 | Received in about 250 ms; sender shows accepted, receiver shows received |
| 1:1 and group while grid time unset | Both refused, each stating that only urgent broadcasts go out (D6) |
| Groups reported | Handheld 1: FAMILY member, KIDS not a member, LEADERS member |
| 1:1 with grid time set | FNK0104B to Hosyond: sender showed **delivered**, which only the recipient's own confirmation produces; receiver showed it received |
| Group with grid time set | FAMILY: sender accepted, receiver received |
| Handheld clocks | Both took grid time from their node inside 0.3 s of registering |
| Handheld image | Hosyond 1,183 KB (23% of the app partition free); FNK0104B 1,163 KB (24% free) |
| Heap, home screen only | Hosyond 112 KB free, 103 KB lowest; FNK0104B 166 KB free, 158 KB lowest |

The screens' own cost is about 4 KB of flash. The message ring and the static snapshot copies in the console and screens account for the drop in free heap since P5 (Hosyond was 146 KB).

## Bench trap: querying a node costs it grid time

Opening a serial port resets these boards, and grid time lives only in RAM (decision D6). So
every `tools/console.py` command against a node reboots it and that node reports `UNSET`
until a neighbour announces the time again, within about a minute. While the owner was
testing chat, repeated queries against LG-MAIN kept clearing the time he had just set, and
his 1:1 messages were rejected for it. `console.py` now says so before each node command.
Read grid state from NORTH or SOUTH, or from a handheld's `status`, rather than from the
master.

## Known limitations

- **Grid time** must be set before the grid accepts anything but urgent broadcasts, so tests 1 to 3 above wait on the admin.
- **Emoji** are not in the keyboard: the fonts carry ASCII and about 60 FontAwesome symbols and no emoji glyphs. Adding them needs an emoji font, which is a decision and a download.
- **Messages live in RAM.** A reboot clears the list; there is no history in flash and no store-and-forward for an offline recipient (design review answers 25 and 13).
- **Heap on the Hosyond** must be measured again with a chat screen open, because the screen and keyboard allocate on top of the 103 KB low-water mark.
- **No unread marks** on the conversation list, and no notification when a message arrives while another screen is open.
- **One user per handheld** (D7): conversations are with handhelds, and names come from the fixed prototype roster.

## Drawing only what changed, and the magnified key (D29, D30)

Build, flash, and read the boards:

```
python tools/build.py --firmware handheld
python tools/flash.py --role H
python tools/serial_capture.py --ports COM9 COM11 --seconds 40 --reset
```

Expected serial output on both handhelds:

```
[TEST] Self test: 47 checks passed in <ms> ms
[GRID] Registered with node <n> as device <n> (Handheld <n>)
[UI] Launcher ready: 240x320 panel, 109x126 tiles
[NET] Online; free heap <n> KB, lowest <n> KB
```

Measured 2026-09-15: FNK0104B 47 checks in 1782 ms, 154 KB free and 149 KB lowest; Hosyond 47 checks in 2487 ms,
116 KB free and 115 KB lowest. Flash use is 1207 KB on the ESP32 (21% of the app partition free) and 1187 KB on the
ESP32-S3 (23% free).

### What changed

- `lg_ui_set_text()` writes a label only when the text differs. LVGL repaints the areas it is told changed, so writing
  identical text was a needless invalidation, several times a second, on screens where most of the words stay the same.
- The chat list keeps what it has drawn. New messages are appended, a delivery state that moved rewrites one note, and
  nothing else is touched. The list is rebuilt only when the conversation changes or the 24-message ring has dropped the
  oldest entry the screen was showing.
- The launcher, Settings, and node screens refresh on a signature of the values they show, not on the service snapshot's
  version counter, which changes every second whether or not anything on screen did.
- A magnified keycap (`lg_ui_keycap_show`) appears above the finger while a key is held: one and a half times the touch
  minimum, the glyph in the theme's largest font, clamped to the screen edges, hidden on release. Control keys (ABC,
  More, space, backspace, enter) show none, because the bubble would only cover the row and the glyph is a word.
- The theme's largest font gained the emoji fallback, which only `font_body` and `font_small` had. Without it a magnified
  emoji key would have drawn a blank box.

### Test procedure (on the handhelds, D25)

1. Launcher: leave it up for half a minute. The tile text should sit still; only a tile whose value actually changed
   should redraw.
2. Settings: open it and wait. Rows should not blink. Change the node choice and only that row's value should change.
3. Messages, then a conversation: send a 1:1. Only the new bubble should appear, and its note should move from pending
   to accepted to delivered without the rest of the list repainting.
4. Keyboard: press and hold a letter. A magnified keycap appears above the finger and follows the key under it; it
   disappears on release. Press ABC, More, or backspace: no keycap.
5. Emoji page: hold an emoji key. The magnified bubble shows the emoji glyph, not a box.

### Known limitations

- The keycap is placed at the touch point, not centred on the key rectangle, so it sits above the finger rather than
  exactly above the key.
- The emoji font is one 20 px size, so a magnified emoji is drawn at 20 px inside a 28 px line: larger than the key it
  magnifies, but not scaled the way a letter is.
- A chat list still rebuilds in full on a conversation change or a ring eviction. That is a real change of content, not
  a refresh.
- Heap was measured with the launcher up. The chat screen and the keyboard allocate on top of it, and the Hosyond's
  116 KB free at Online is the tightest of the two boards.
- Whether the screens still appear to flicker is the owner's call on the glass; serial can only show that the screens
  built and that nothing crashed.

## Keyboard pages, markers, read receipts, icons, screen saver (D32 to D36)

Build, run the on-board suite, flash, and read the boards:

```
python tools/build.py
python tools/flash.py hosyond --firmware tests --no-build --no-verify
python tools/serial_capture.py --ports COM11 --seconds 220 --until "LG_TESTS_RESULT" --reset
python tools/flash.py --role H
python tools/serial_capture.py --ports COM9 COM11 --seconds 45 --reset
```

The suite runs first because this work reached into `lg_core` and into the node's ack forwarding, so
`LG_TESTS_RESULT: PASS` is the gate the handhelds wait behind (AGENTS.md, definition of done 2).

### What changed

- **D32 keyboard.** The letters page dropped its digit row and its punctuation, so it is four rows and a
  key is about 36 px tall instead of 22 px. Punctuation has its own six-column page. A phone keypad page
  types by multi-tap the way SMS keypads did: three columns of about 78 px, `2 abc` to `9 wxyz`, a pause
  of 900 ms ending the run. Three columns is the only way to make a key wide on a 240 px panel.
- **D33 launcher.** Three icon tiles: Messages across the top, Status and Settings beneath. The self test
  moved inside Status with its last result and a Run button, so it is in one place rather than three.
- **D34 markers and read receipts.** Our own messages carry a marker instead of a sentence, and a received
  1:1 message is reported read when the chat showing it is on the display.
- **D35 screen saver.** A minute untouched fills the panel with green characters falling down it; the next
  touch clears it and is swallowed so it cannot press what is underneath.
- **D36 icons.** One size for every bar and control icon, through `lg_ui_icon_button()` and the theme's
  `font_icon`. Launcher tiles keep the larger glyph, because there the icon is the tile's subject.

### The protocol change

A read report is the existing `MSG_ACK` message with a new status, `LG_ACK_READ = 9`, appended so no
existing status moves. The body stays 13 bytes and its length check is unchanged. Three other places had
to follow, and the first attempt missed one:

- `lg_msg_ack_dec()` ends with a range check against the highest status it knows. Adding a status without
  raising that bound made every read report malformed at the node and at the recipient. Nothing crashed
  and nothing was logged; the on-board suite reported five failing checks and no read markers. Any future
  status has to be named in that bound too, which the comment there now says.
- The node's `handle_client_ack()` accepted only `DELIVERED` from a recipient and now accepts `READ`. The
  backbone case has no status gate, so it needed nothing.
- Received messages now carry the identity of what arrived (author, boot, sequence) in `lg_in_msg_t`,
  because a read report names the message it is about. That costs the inbox 8 bytes per entry.

Read state is 1:1 only: one report per member would multiply traffic on a group and say little.
`hh_service_mark_read()` carries the request to the service task through its queue, so screens still never
touch the client (D27), and each message is reported once.

### Expected serial output

```
[TEST] Self test: 47 checks passed in <ms> ms
[GRID] Registered with node <n> as device <n> (Handheld <n>)
[NET] Online; free heap <n> KB, lowest <n> KB
[UI] Launcher ready: 240x320 panel, tiles 224x126 and 109x126
```

### Measured 2026-09-16

| | FNK0104B (ESP32-S3) | Hosyond 3.2in (ESP32) |
|---|---|---|
| Self test at boot | 47 checks, 1801 ms | 47 checks, 2463 ms |
| Heap at Online | 151 KB free, 146 KB lowest | 115 KB free, 113 KB lowest |
| Flash used | 1191 KB, 22% of the app partition free | 1211 KB, 21% free |

On-board suite on the Hosyond: 405 checks, 0 failures, min free heap 96,204 bytes. The suite takes about
two minutes there, mostly the 80,000-iteration PBKDF2 vector, which is why flashing it uses `--no-verify`
and a capture with `--until`.

### Test procedure (on the handhelds, D25)

1. Launcher: three tiles, labels on one line, tile icons larger than the icons in any bar.
2. Messages, then a conversation. Send a 1:1 with grid time set (D6) and watch the marker on your own
   bubble: `↻` going out, one tick when a node takes it, two when the other handheld confirms it.
3. Open that conversation on the receiving handheld. The sender's two ticks turn the accent colour, which
   is the read report arriving. This needs both boards and a set grid time.
4. Keyboard: hold a letter for the magnified keycap; `123` for the keypad, where tapping `2 abc` steps
   a, b, c; `.,?` for punctuation; `ABC` back to letters.
5. Urgent: the triangle is grey when off and amber when on, and only appears for Everyone.
6. Leave a handheld untouched for a minute: green rain, cleared by one touch, and the screen underneath
   comes back as it was.

### Known limitations

- Read reports cannot be turned off, and they tell the sender and the nodes that a message was opened.
- The keypad types lower case only; there is no shift on that page yet.
- The magnified keycap sits above the touch point rather than centred on the key: LVGL 9.5 has no public
  getter for a key's rectangle, and its own popover shows the key at normal size rather than magnified.
- A magnified emoji is drawn at 20 px inside a 28 px line, because the emoji font has one size.
- The screen saver leaves the backlight alone, so it saves the panel and not the battery, and it writes
  nothing to serial: only watching the panel proves it runs.
- The Hosyond has 115 KB free with the launcher up. The chat screen, the keyboard and the saver's labels
  allocate on top of that, and it is the tighter of the two boards.

