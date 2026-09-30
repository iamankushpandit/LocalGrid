# Security policy

LocalGrid carries people's messages, including emergencies, so security reports are taken
seriously even though this is a prototype.

## Reporting a vulnerability

**Please do not open a public issue.** Use GitHub's private vulnerability reporting: the **Report a
vulnerability** button under this repository's **Security** tab. It keeps the report private until
there is a fix.

Say what you did, what happened, and what you expected. Name the boards and the commit, and include
serial output if it helps. Before you paste a log, remove grid names, people's names, and message
text. Proof-of-concept code is welcome but not required.

This is a one-maintainer project. Expect an acknowledgement within about a week. There is no bounty.
You may disclose publicly once a fix ships, or after 90 days, whichever comes first.

## Supported versions

Only the latest commit on `main`. There are no releases yet, and devices update only when someone
reflashes them.

## In scope

- **The radio protocol.** Anything that lets someone outside the grid read, forge, replay, or
  suppress messages; recovers a 1:1 message without being one of its two ends; or reuses a nonce.
  The envelope and crypto are in `components/lg_core` and `components/lg_crypto`.
- **The access point.** Memory-safety bugs or crashes reachable over Wi-Fi, ESP-NOW, BLE, or the
  handheld TCP session.
- **The admin page.** Authentication bypass, CSRF, or injection against the page every access point
  serves.
- **Leaks.** Keys, passphrases, or 1:1 plaintext reaching a log, the serial console, the admin page,
  or the air unencrypted.

## Known limits, not vulnerabilities

These are documented decisions, stated here so they are not reported as findings:

- **Group and broadcast messages are readable by access points** (D5). They are encrypted in
  transit; only 1:1 messages are end to end.
- **The admin page is plain HTTP inside the grid's WPA2 network** (D11, D45). Anyone who has the
  grid's Wi-Fi passphrase can observe admin traffic. The protection is the passphrase.
- **One set of keys per grid, compiled in** (`tools/gen_secrets.py`). Anyone holding a device from
  your grid can extract them. Build your own; never use keys from someone else's build.
- **Physical access is full access.** There is no secure boot and no flash encryption on the
  prototype.
- **Radio denial of service** (jamming, flooding the channel) is out of scope.
