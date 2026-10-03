# Temporary concessions, and what must be true before they come out

Some things in this firmware exist because of the bench, not because they belong in the product. They
were taken deliberately, with the owner's decision recorded in `docs/DECISIONS.md`, and they are
**meant to be removed**. This file is the register: what was conceded, why, what it costs, what has
to become true before it can go, and how to check it is really gone.

> Owner, 2026-09-30: "I'm not making these decisions because they will always remain in the firmware.
> We just need to make sure that our system is resilient and once that becomes a fact, then we can
> take these things out."

The bench problem behind all of it: ten devices on one laptop's USB, a supply that cannot carry them,
and access points that brown out **before Wi-Fi even initialises** — 28 brownouts per AP in 18 s,
every one of them right after `heap_init`, with neither AP ever reaching `[NET]`. Moving the APs to
battery fixes the power and takes their serial cable away, and everything below follows from that.

> **Update, 2026-09-30, later the same day: the owner changed the power supplies and the brownouts
> are gone.** All three APs now come up clean — zero brownouts, `[NET]` reached, `links 2` on every
> one of them, which is a complete backbone mesh — and all nine boards answer their device ID on
> USB. That is the condition T1 and T2 name for their own removal, and it arrived before either of
> them had injected a single fault.
>
> They are **not removed yet**, deliberately, and this is the reasoning rather than inertia: the
> owner's plan is still to move the APs to battery, which is what takes the cable away again, and
> throwing the work out now would mean writing it twice. What changes today is the *order* — chaos
> testing runs over USB first, because that framework is strictly better (serial logs from every
> board, and the power-out fault a self-chaos build cannot perform), and the wireless path becomes
> the fallback it was meant to be rather than the only way in. If the APs move and stay stable on
> battery, T1 and T2 keep their reason to exist; if they stay on this bench, both should go.
>
> Nothing about the release blocker changes. `tools/check_release.py` still fails on a release tag
> while any of this is in the tree, which is the point of it not being a promise.

## The register

### T1 — Chaos code inside the AP firmware (D82)

**What.** `firmware/node/main/chaos_self.c`: an AP that takes its own backbone down, takes its own
LoRa down, and restarts itself on a seeded schedule.

**Why.** `tools/chaos.py` injects every fault from the PC side of a USB serial port. On battery there
is no port, so without this there is no way to cause a fault at all.

**What it costs.** A firmware that deliberately breaks the grid exists, and could be flashed to a real
device by mistake.

**What must be true to remove it.** Either the bench can hold every device on USB reliably — a powered
hub, a supply per device, or fewer devices — or a dedicated chaos harness exists that can cut power
per device without the laptop's USB rail. Then `chaos.py` over USB does this again, better, and with
full serial logs.

**How to remove.** Delete `chaos_self.c`, `chaos_log.c`, the `LG_NODE_CHAOS` Kconfig option, the
`chaos` console command, and the `chaoslog` partition. Nothing else depends on them.

**How to check it is gone.** `strings build/lg_node.bin | grep SELF-CHAOS` finds nothing, and
`grep -r LG_NODE_CHAOS firmware/` finds nothing.

### T2 — Chaos armed over the network (D83)

**What.** `POST /api/chaos` on the admin page arms, renews and disarms a chaos run; `GET /api/chaoslog`
reads the journal back.

**Why.** Same as T1: no cable, so the run has to be started and stopped over Wi-Fi.

**What it costs.** This is the one that most directly contradicts what
`docs/chaos-engineering.md` used to claim — that the control surface "exists **only on the physical
USB cable**". It no longer does. The honest reading is that the boundary moved to the admin password
when D81 added a firmware upload, not when chaos was added; an attacker with that password could
already upload an image that does anything chaos does. But the bar to *misuse* is lower now, and that
is a real cost, not a rhetorical one.

**What must be true to remove it.** T1 is removed, or the bench can reach the APs by cable again.

**How to remove.** Delete `h_chaos` and `h_chaos_log` and their two routes from `web_admin.c`, and
restore the "not a backdoor" section of `docs/chaos-engineering.md` to a claim that is true again.

**Guards it has while it stays.** The chaos code is absent from an ordinary build, so a production AP
has nothing to reach. Two secrets are needed, not one: the grid's WPA2 passphrase to be on the
network at all, and the admin password to log in. The fault budget is bounded and counted in flash,
so it cannot be raised from the network. And the armed window closes by itself unless renewed, so
losing contact ends a run rather than sustaining it.

### T3 — Firmware replaceable with only the admin password (D81)

**What.** `POST /api/update` takes a raw image and makes it bootable.

**Why.** An AP on battery has no USB, so without this its firmware is frozen until it is physically
carried back — with none of the LoRa work yet proven on a radio.

**What it costs.** Anyone who can join the AP's Wi-Fi **and** has the admin password can replace the
firmware entirely. That is a much larger thing to put behind that one password than setting the time.

**This one is not simply removed — it is hardened.** OTA is worth keeping; what is missing is that the
AP will accept *any* image. The fix is **signed images** (`CONFIG_SECURE_SIGNED_APP_NO_SECURE_BOOT`,
or full secure boot), so that even with the password only a firmware signed by the project's key is
accepted, and a stolen password cannot become arbitrary code execution.

**What must be true to do it.** A signing key exists and is kept somewhere other than the repository,
and `tools/flash.py` and the build sign images as a matter of course. Worth doing before anyone else
runs a grid, and independently of the bench.

**How to check.** An unsigned image is refused by the bootloader on a board with signing enabled.

### T5 — Chaos armed over BLE (D85)

**What.** Four opcodes on the BLE admin link — `OP_CHAOS_GET`, `OP_CHAOS_ARM`, `OP_CHAOS_DISARM`,
`OP_CHAOS_LOG`. They are the **only** opcodes on that link that change anything.

**Why.** The owner asked for a chaos run startable from the phone, without USB, once the access
points move to battery, and said plainly that it breaks earlier decisions and is acceptable for as
long as the bench cannot power its own APs. It was asked for twice: the first time it was answered
with Wi-Fi control instead (D83), which was not what was asked.

**What it costs.** This is the clearest contradiction of a standing decision in the tree. D70 says a
BLE watcher "can never take part", and the Android app being read-only was part of that promise.
BLE is also reachable from outside a building without the grid's Wi-Fi passphrase, so where Wi-Fi
control needs two secrets (the passphrase and the admin password), this needs one.

**Guards it has while it stays.** The admin password is proved on this link already — `OP_LOGIN` is
an HMAC over a random 32-byte challenge keyed by the stored password hash — and every chunk is
sealed with `K_link`, so these opcodes inherit real authentication rather than inventing one.
Arming is refused unless the firmware is a chaos build, so a production AP has no chaos code to
reach. The dead-man window is unchanged: arming is refused when the grid has no time, the window is
capped, and the run stops itself when nobody renews it — **losing the phone ends a run, it cannot
sustain one.** The fault budget stays counted in flash.

**What must be true to remove it.** The access points can be reached by cable again, or they have
proved they survive on battery and no longer need chaos run against them remotely. Removing T1
removes the reason for this one too.

**How to remove.** Delete `do_chaos_status`, `do_chaos_arm`, `do_chaos_disarm` and `do_chaos_log`
from `firmware/node/main/ble_link.c`, the four opcodes from its enum, the two reply opcodes, and the
chaos names from the login gate and the dispatch switch. Then restore `docs/ble-link.md` and D70's
claim that nothing on this link writes.

**How to check it is gone.** `python tools/check_release.py --release` passes, and
`grep -c OP_CHAOS firmware/node/main/ble_link.c` finds nothing.

### T6 — One fault on request, over BLE (D90)

**What.** One opcode on the BLE admin link, `OP_FAULT` (kind, seconds): backbone off, LoRa off, LoRa
reset, or restart, performed now, by the access point that receives it. It replaces the self-scheduler
of T1 as the way a chaos night reaches an access point that has no cable.

**Why.** The owner asked why the firmware was being changed for chaos at all when BLE could simply
carry the fault commands (2026-10-03). The faults were never the special part - `bb off`, `lora off`
and `lora reset` are ordinary console commands in every build, each self-restoring - and only the
scheduling needed a special image, because D83 had once refused BLE control. D85 reversed that refusal
and left the scheduler in place. With the laptop naming each fault there is nothing autonomous on the
device, so the seed, the NVS budget and the dead-man window have nothing left to protect.

**What it costs.** This is in **every** image. The chaos build at least kept its code out of an
ordinary access point; this puts a remote "break yourself" opcode behind the admin password on every
one. It is a smaller surface than T1, T2 and T5 together, and a wider one than a build flag.

**Guards it has while it stays.** The same login as every other opcode on the link: `OP_LOGIN` is an
HMAC over a random 32-byte challenge keyed by the stored password hash, and every chunk is sealed with
`K_link`. Seconds are capped at `LG_FAULT_MAX_S` (3600), defined once in `node_app.h` and shared with
the console, so the two cannot disagree. **Every fault is its own dead-man switch:** the radio comes back
on the access point's own clock whether or not the laptop is still there. The allow-list is three radios
and a restart; nothing here reads a message or changes a setting.

**What must be true to remove it.** The same as T1: a bench that can hold every device on USB
reliably, so `tools/chaos.py` can type the faults at a serial port again - or a harness that can cut
power per device, which this opcode cannot do either.

**How to remove.** Delete `do_fault`, `fault_restart_cb` and the `FAULT_*` enum from
`firmware/node/main/ble_link.c`, `OP_FAULT` and `OP_FAULT_OK` from its opcode enum, the login gate and
the dispatch switch; delete `OP_FAULT`, `fault_body` and `decode_fault_ok` from `tools/grid_watch.py`
and the `fault` command from `tools/chaos_ble.py`; and take `chaos.py`'s BLE fault path out with them.
`LG_FAULT_MAX_S` stays: the console hooks still need the cap.

**How to check it is gone.** `python tools/check_release.py --release` passes, and
`grep -c OP_FAULT firmware/node/main/ble_link.c` finds nothing.

### T4 — The handhelds' Wi-Fi switch is sticky (D78)

**What.** `wifi off` with no seconds survives a restart.

**Why.** It is meant for carrying a handheld into a field to test LoRa, where a restart must not
quietly put Wi-Fi back.

**What it costs.** Little, and it is bounded: `wifi off <seconds>` is the self-restoring form, and the
sticky one is a deliberate field-test switch rather than a fault. Listed here only so the register is
complete, and so nobody mistakes it for the chaos hooks.

**What must be true to remove it.** Nothing; this one is expected to stay. Revisit if handhelds ever
ship to people who are not testing the radio.

## How this list is meant to be used

- **Nothing here goes into a release build of anything.** T1, T2 and T5 are absent from an ordinary
  build today (the chaos code is not compiled in); T3 is present and is the one to harden rather
  than delete.
- **A push is refused, not only a release.** `.githooks/pre-push` runs `tools/check_release.py
  --release` and stops `git push` while any of this is in the tree; install it with
  `python tools/install_hooks.py`. One bypass exists, `LG_ALLOW_TEMPORARY=1 git push`, which is
  deliberately awkward and leaves a line in the shell history saying the person knew.
- **Revisit this file whenever the bench changes.** The moment every device can sit on USB reliably,
  T1 and T2 have lost their reason to exist and should go the same week.
- **Anything added for the bench belongs here**, with the same five headings, on the day it is added.
