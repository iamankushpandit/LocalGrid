# Two ad scripts

Draft, 2026-09-20. For the launch plan in [OPEN_SOURCE.md](OPEN_SOURCE.md), section 6.

The shape is the one the owner asked for: open on **"what if there were no network and you made your
own?"**, then keep asking the next question the viewer is already thinking, and let the product
answer it. Each answer is something LocalGrid does today on the boards. Section 3 checks every claim
and lists the lines we must not write.

Both run about 45 seconds. The questions are the voice-over; the shots are what is on screen. They
work as a video, as a carousel (one question per card), or as a page.

---

## 1. Camping: "No bars. No problem."

| # | Question (voice-over) | On screen |
|---|---|---|
| 1 | What if there was no network at all? | A phone held up at arm's length. "No service". The hand drops. |
| 2 | What if you could just make your own? | A hand clips a small board to a tent pole, thumbs a power bank on. A green dot lights. |
| 3 | And another one, over there? | Second board on a tree at the far side of the field. The two dots find each other. |
| 4 | So now, how do you tell everyone dinner is ready? | A thumb types "Dinner at the big tent". It lands on four handhelds at once, across the site. |
| 5 | And when you would rather just say it? | A finger holds a name. "Bring the torch." A voice comes out of a handheld two fields away. |
| 6 | Where are the kids, anyway? | The map on the admin page: everyone carrying a handheld with a GPS, and how far each is. |
| 7 | And if something actually goes wrong? | A red screen on every handheld, even the locked one. One tap: Read. The sender sees who has read it. |
| 8 | What happens when a battery dies out there? | A board goes dark. The rest of the network carries on. Power it back up, and it picks up where it left off. |
| 9 | And what does all this cost you, every month? | A hand puts three small boards on a table. Nothing else. |
| 10 | No signal. No SIM. No Internet. Nobody's server. | The camp at dusk, green dots glowing in three places. |

**Close:** "LocalGrid. Your own network, wherever you are." Then: open source, build it yourself,
the site address.

---

## 2. Carpool: "Three cars. One conversation."

| # | Question (voice-over) | On screen |
|---|---|---|
| 1 | What if the convoy lost each other at the first junction? | Three cars leave a car park. One takes the wrong lane. |
| 2 | What if there is no signal on the mountain road anyway? | A phone on a cradle: one bar, then none. |
| 3 | What if every car brought its own network? | A board on the dash of each car, plugged into the 12 volt socket. Green dots. |
| 4 | So, fuel stop in two miles? | Typed once, read in all three cars. |
| 5 | Hands on the wheel, though? | A passenger holds a button and speaks. It comes out of the other cars. |
| 6 | Who is still with us? | The list: two cars close, one gone quiet. It says so, straight away, rather than pretending. |
| 7 | And if the quiet one has a problem? | The car pulls over. The emergency fills every screen in the convoy, and they see who has read it. |
| 8 | What about the kids in the back? | A handheld passed between the back seats. Their own conversation, in their own car. |
| 9 | No towers. No data. No apps. Nothing to sign up to. | Three cars, three green dots, moving together. |

**Close:** "LocalGrid. Your own network, wherever you are."

---

## 3. Claims check

Every line above is backed by something on the bench. If a script changes, check it here first.

| Line | Backed by |
|---|---|
| Messages to everyone, groups, and one person | Working since the messaging milestones |
| Hold to talk, voice out of another handheld | D61, verified on the boards |
| Emergency on every screen, including locked; who has read it | D57, D58, D62 |
| Where people are, and distance to the main access point | D63, D64, D65: a GPS on MAIN and on a handheld |
| A device restarts and gets its state back; losing one loses its coverage only | D48, D53 |
| The list says who is reachable, right away | Presence, and D13: a message to someone offline is refused, not held |
| No Internet, no SIM, no account, no server | The product's central promise |

**Lines we must not write:**

- **"It will reach them when they come back in range."** There is no store-and-forward (D13). A
  message to someone unreachable is refused and the sender is told. Ad 2 question 6 says exactly
  that, deliberately.
- **"Miles apart", or any distance at all.** Nothing has been measured. Access point to access
  point is estimated at 100 to 200 metres and a handheld reaches its access point at an estimated
  30 to 60 metres, so the convoy in ad 2 must look like cars within sight of each other, not
  scattered across a county. Say nothing about range until the field test measures it.
- **"Talk to anyone, anywhere."** It is a local network, by design.
- **Anything about LoRa working.** The modules are fitted to the access points; the firmware is
  being written. When it works, ad 2 gets a new question ("And when the convoy stretches out?") and
  ad 1 gets one about the far end of a big site.
- **Encryption claims beyond the design.** One-to-one messages are end to end. Group and everyone
  messages are encrypted over the air but readable by an access point (D5). No ad should imply
  otherwise.
- **Anything about a camp network.** It is an offline network; camping is one use of it (D19).

## 4. Notes for shooting

- The product has no logo animation yet, and the handheld screens are the best thing about it: shoot
  the screens, not the boards.
- Question 7 in ad 1 and question 7 in ad 2 are the same beat, the emergency. That is the beat worth
  paying for a proper shot of.
- Both ads end on the same line, so they can run as a pair.
- A cheaper first version: shoot ad 1 on the bench with three boards on a table and a phone camera,
  and cut the field to a garden. The demo video in the launch plan can be the same footage.
