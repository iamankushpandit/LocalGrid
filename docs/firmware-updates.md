# Firmware updates: signed, sticky, and installable by somebody who is not the bench

Status, 2026-10-05: **the update file and its tools, runtime grid keys, identity over the cable, publishable
images, the installer page and two slots on every device are built. No device accepts or passes on an
update yet** - that is steps 1 and 3 of the order of work at the end. Everything from "What a device does with it" down is a plan
for the owner to review before it is built.

The owner's requirement (2026-10-04): updates over the air for handhelds and access points, "as sticky as
the grid time" - upload to one device and it spreads - and a web utility that makes the digital signature,
living next to a web flasher, so that people outside the bench can use LocalGrid at all.

## Built: the update file (`.lgfw`)

A signed header, then the application image exactly as the build made it.

| Offset | Size | What |
|---|---|---|
| 0 | 4 | `LGFW` |
| 4 | 1 | format, 1 |
| 5 | 1 | role: `N` access point, `H` handheld |
| 6 | 1 | chip: 1 ESP32, 2 ESP32-S3, 3 ESP32-C6 |
| 7 | 1 | reserved, 0 |
| 8 | 4 | serial, little-endian: the number "newer wins" compares |
| 12 | 4 | image length |
| 16 | 32 | SHA-256 of the image |
| 48 | 8 | key id: the first 8 bytes of SHA-256 of the signer's public key |
| 56 | 32 | version name, UTF-8, NUL padded |
| 88 | 40 | reserved, 0 |
| 128 | 64 | ECDSA P-256 / SHA-256 signature over bytes 0 to 127, raw r then s |
| 192 | | the image |

Why this shape:

- **The signature covers the header, and the header carries the image's hash.** A device can refuse a
  forged or foreign update after 192 bytes, before it has erased anything, and check the image as it
  arrives.
- **Role and chip are signed.** A handheld image cannot be installed on an access point, or an ESP32
  image on an S3, by renaming a file. One image serves every board of a role and chip, because a board's
  pins come from its own identity at run time.
- **The serial is a number, not a date.** Newer wins by comparing it; the tools fill in the signer's
  clock in seconds, which only ever goes up, and the version name is for people.
- **ECDSA P-256**, because the browser (WebCrypto) and the device (PSA Crypto, through `lg_crypto`) both
  have it with no added library, and both use the same raw signature form.

## Built: the tools

- **`site/sign/index.html`** - one self-contained page, published by `tools/gen_site.py` at `/sign/`.
  It makes a release key, signs an image, and checks an update file, entirely in the browser: nothing
  is uploaded and nothing is fetched. This is the utility for people who are not the bench.
- **`tools/lgfw.py`** - the same on the command line (`keygen`, `sign`, `check`, `show`), for a build
  machine.

Checked on 2026-10-04: a file signed by each tool verifies in the other; each refuses a file signed by
a different key and a file with one byte changed; the real access-point image (1.15 MB) signs and
checks.

**The private key is the whole defence.** Whoever holds it can update every device that trusts its
public half. It never goes on a device, never into git, and should exist in two places the owner
controls.

## Plan, for review: what a device does with it

1. **Which key a device trusts.** The release public key (65 bytes) is stored on the device beside its
   identity, written when the device is set up. A device with no key accepts no update over the air.
   Changing the key needs a cable.
2. **Two slots on every device.** Access points already have them (D81). Handhelds get the same
   layout, which costs each handheld **one re-flash over USB**; its identity is kept.
3. **Accepting an update.** From the admin page upload that exists today, from a neighbour, or over
   USB. The device checks, in order: the signature against its key; role and chip against itself; the
   serial against what it runs (higher only); then the image hash as the bytes arrive. Any failure
   leaves the running firmware untouched.
4. **Trial boot.** New firmware runs once on trial. If it has not rejoined the grid within two minutes
   it restarts into the old slot (the bootloader's rollback, already on for access points).
5. **When it installs.** Never during a talk, an SOS or an alert; never below 30 % battery on a
   handheld; one device per access point at a time, so a grid never updates itself into silence.
   **Open question for the owner:** install by itself when idle, or wait for a tap on each handheld?

## Plan, for review: how it spreads

"As sticky as the grid time" (D48): what a device learns that another could use is kept, versioned so
newer wins, and announced on link-up and periodically.

- **Announcement.** A small frame: "I run serial N for (role, chip), and I am carrying serial M for
  (role, chip)". Sent on link-up and every few minutes, access point to access point and access point
  to handheld.
- **Fetching.** A device that hears of a higher serial for its own (role, chip) asks that neighbour
  for the 192-byte header first, checks it, then pulls the image in chunks of about 1 KB over Wi-Fi,
  resuming where it stopped. Never over LoRa: a megabyte would take hours.
- **Carrying for others.** The spare slot holds one image. An access point carries a handheld image;
  a handheld carries the access-point image. So an update for any kind of device reaches every kind,
  a hop at a time.

**The honest limit.** There are four images (access point; handheld for ESP32, S3 and C6) and one spare
slot per device. A full rollout is therefore several rounds of carry and pass, not one, and a device
that is carrying an image for somebody else cannot at that moment hold its own rollback copy. The plan
is to prefer a device's own update over carrying, and to carry only once its own firmware is confirmed.

## Plan, for review: the web flasher, and what it depends on

A page beside the signer that installs LocalGrid on a board over USB from the browser (ESP Web Tools
over Web Serial), with no toolchain.

**Step 4 below is built (D96, 2026-10-04): a device now holds its own keys.** What is left is a published image, an identity for a board the bench has never seen, and the page. Before D96 it could not be offered at all, for one reason: a built image carries the grid's keys - the Wi-Fi
passphrase and the backbone key are compiled in from `lg_secrets.h` (see `docs/OPEN_SOURCE.md`,
decision 4). A downloadable image would make every installation the same grid.

So the flasher needs keys to move out of the build:

1. A device with no grid keys starts in setup mode.
2. The first access point makes the grid's keys on its admin page, and is given the release public
   key (pasted from the signer page).
3. Every other device receives the grid keys and the release key at setup, over the same USB cable the
   flasher has open, or from an access point in a short pairing step.
4. Then one published image per (role, chip) serves everybody, the flasher can offer it, and the signer
   page is how each grid's owner signs the builds they choose to trust.

That is the larger half of this work, and it changes how every device starts, so it is listed here
rather than begun.

## Order of work proposed

1. Device-side verification of an update file (`lg_crypto` gains ECDSA P-256 verify with RFC test
   vectors), and the admin-page upload takes `.lgfw` instead of a raw image. This retires T3.
2. Handheld two-slot layout and the one-time USB re-flash.
3. The announcement, fetch and carry frames, with on-board tests, then a bench run.
4. Keys out of the build; setup mode.
5. The web flasher.
