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
