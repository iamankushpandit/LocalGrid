# Prototype milestone 1: messaging core

Status: **passed on hardware, 2026-09-15.** 313 checks, 0 failures, on an Elegoo ESP32 (COM16).

## What exists

| Component | Path | Purpose |
|---|---|---|
| `lg_core` | `components/lg_core` | Portable C11 protocol core: 32-byte envelope, message IDs, duplicate windows, roster and groups, node routing, handheld outbox and inbox. No ESP-IDF headers. |
| `lg_crypto` | `components/lg_crypto` | X25519, HKDF-SHA256, ChaCha20-Poly1305 over PSA Crypto, plus the 1:1 end-to-end context |
| Test app | `tests/target` | Runs unit tests, RFC test vectors, and a simulated three-node network on a board |

## What the tests prove

- **1:1 messages** cross two node hops, reach only the recipient, and report `delivered`. The plaintext never appears in any node-to-node frame. A third user cannot decrypt a captured frame.
- **Group messages** reach only members. A non-member's send is rejected with "not a member".
- **Broadcasts** reach every other user once. Routine broadcasts are limited to one per 10 s per user; urgent ones to one per 2 s.
- **Duplicates** are suppressed with a full-mesh topology and every backbone frame delivered twice. A retransmitted message is shown once.
- **Offline and roaming:** a message to an offline user is rejected as offline; after the user reconnects on another node, messages reach them there.
- **Time rule:** with grid time unset, a handheld can only receive and send urgent broadcasts. A handheld whose clock is more than 2 minutes off its node is refused.
- **Safety:** nodes refuse unencrypted 1:1 text, author spoofing, unknown devices, and malformed frames.
- **Key pinning:** a changed public key raises a warning and the pinned key stays in use.
- **Crypto correctness:** ChaCha20-Poly1305 matches RFC 8439 §2.8.2, X25519 matches RFC 7748 §6.1, HKDF matches RFC 5869 A.1.

## Build and flash

Open a PowerShell terminal:

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
idf.py -C C:\Users\Ankus\esp32\LocalGrid\tests\target set-target esp32
idf.py -C C:\Users\Ankus\esp32\LocalGrid\tests\target -p COM16 flash monitor
```

Any of the three Elegoo boards works; use COM16, COM17, or COM18. Exit the monitor with Ctrl+].

## Expected serial output

```
LG_TESTS_START
core: 71 checks, 0 failures so far
crypto: 110 checks, 0 failures so far
messaging: 313 checks, 0 failures so far
LG_TESTS: 313 checks, 0 failures, min free heap 147352 bytes
LG_TESTS_RESULT: PASS
```

Any failure prints a `FAIL file:line: condition` line before the summary.

## Measured

| Item | Value |
|---|---|
| Build warnings with ESP-IDF v6.1 defaults (warnings are errors) | 0 |
| Minimum free heap during the run, classic ESP32, no Wi-Fi | 147,352 bytes |

The heap figure includes three node cores, four handheld cores, and a 40 KB event queue in one process, so it is not a firmware budget.

## Known limitations

- Radio is not involved yet. Links are in-memory queues.
- No store-and-forward: a 1:1 message to an offline user is rejected.
- Public keys are trusted on first use; a master-signed directory comes in Phase 1.
- Group and broadcast text is readable by nodes. Backbone frame encryption is added with the ESP-NOW transport.
- Message bodies use fixed binary layouts; nanopb comes in Phase 1.
- Presence has no timeout for a node that disappears; that arrives with the node firmware's HELLO handling.

## Next milestone

Node firmware on the three Elegoo boards: SoftAP on a fixed channel, TCP sessions for handhelds, ESP-NOW backbone with HELLO, BLE advertising, and the serial console. Then handheld firmware on the CYD and the FNK0104B.
