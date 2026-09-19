# Grid watch: see the grid from a laptop (D68)

`tools/grid_watch.py` shows how an offline network is doing from any laptop with Bluetooth, without
joining the grid's Wi-Fi. Every AP broadcasts a small sealed status over BLE; the laptop only
listens and shows it on a local web page.

It shows:

- **An alert banner:** an active SOS or urgent message (who, near which AP, how long ago, read by
  how many), or "all clear" when the sender marks themselves safe.
- **Each AP:** heard or not and signal strength, uptime, backbone links to the other APs, handhelds
  connected, time source (GPS, carried, or unset), GPS fix and satellites, lowest free memory,
  restarts, brownouts, and why it last restarted.
- **Each handheld:** chosen name, online or not, which AP it is on, and battery.
- **An event log:** APs lost and heard again, restarts, alerts, time source changes.

Positions are never broadcast.

## What you need

- This repository, with the **same `firmware/common/lg_secrets.h` your APs were built from**. The
  tool reads the grid key and grid ID from it. Each grid makes its own with
  `python tools/gen_secrets.py`; the example file's all-zero key is refused.
- Python 3.10 or newer and two packages:

      pip install bleak cryptography

- A laptop with Bluetooth, within BLE range of at least one AP (tens of metres, less through walls).
- APs running firmware with D68 (the scan-response status). Older APs still show as heard, with
  "discovery advert only".

## Run it

    python tools/grid_watch.py

The page opens at `http://127.0.0.1:8768/`. It exists only while the tool runs; stop it with
Ctrl+C. The page refreshes every second, and the first handheld names and alert state fill in
within about a minute.

| Option | What it does |
|---|---|
| `--port N` | serve the page on another port |
| `--no-browser` | do not open a browser |
| `--secrets PATH` | read another grid's `lg_secrets.h` |
| `--log FILE` | append events as JSON lines |
| `--seconds N` | listen for N seconds, print a summary, and exit |
| `--self-check` | test the decoder offline with frames sealed by a test key; no hardware |

## Security

- **Only this grid can read it.** The status is encrypted and authenticated with a key derived from
  the grid's backbone secret (ChaCha20-Poly1305, `docs/ble-status.md`). A stranger's scanner sees
  random bytes, and nobody without the key can fake an "all fine". Anyone holding `lg_secrets.h`
  can read the status, so share that file as carefully as the grid itself.
- **The laptop never transmits into the grid** (D25) and never joins its Wi-Fi.
- **The page is served on 127.0.0.1 only**, from one self-contained file with nothing fetched from
  the Internet. Other machines on your network cannot open it.
- **No BLE addresses** are printed, logged, or served (D21).

## Platforms

Tested on Windows 11. `bleak` also supports macOS and Linux; on Linux, active scanning (needed to
receive scan responses) can require running with Bluetooth permissions. Windows passes on a scan
response only every few seconds, so frame types arrive slowly but steadily.

## If something looks wrong

| You see | Meaning |
|---|---|
| No APs heard | Bluetooth off, out of range, or APs not powered |
| APs heard, "discovery advert only" | the APs run firmware older than D68 |
| "wrong key?" in the event log | this `lg_secrets.h` is not the one the APs were built from |
| An AP not heard for 10 s | it is off, restarting, or out of range |
| Battery blank | that handheld has no battery sense, or runs firmware older than D68 |
| After erasing an AP's flash, its status stops | its boot counter went back; restart the tool |
