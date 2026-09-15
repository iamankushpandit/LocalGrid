# Prototype milestone 3: admin web page

Status: **setup verified on hardware, 2026-09-15.** The owner joined `LG-MAIN` and completed setup at `http://192.168.4.1/`. NORTH then adopted the admin-set grid time from the master (`[TIME] Adopted grid time 1789491212 from node 0`), matching the development PC's clock to within about 2 s. Login, lockout, and the manual time controls are still to be exercised.

The development PC reaches the Internet only through Wi-Fi, so it cannot join `LG-MAIN` without dropping its own connection. The page must be tested from a phone or tablet.

## What exists

The master node (node 0, COM16) serves `http://192.168.4.1/` on the `LG-MAIN` network:

| Area | Behavior |
|---|---|
| Setup | First visit asks for the network name and an admin password of 12–64 characters. Time and time zone come from the browser. |
| Login | Password checked with PBKDF2-HMAC-SHA256, 4,000 iterations, 16-byte random salt. |
| Sessions | 128-bit random cookie, `HttpOnly; SameSite=Strict`, 30-minute idle expiry, at most 2 sessions. |
| Protection | CSRF token header plus Origin check on every change; after 5 failed logins, lockout grows from 30 s to 5 min. |
| Time | "Use this device's time" button and manual entry. The master announces time to every node, and nodes push it to handhelds. |
| Status | Grid time with a NOT SET warning, master stats, infrastructure links with signal, people's presence. |

Files: `firmware/node/main/web_admin.c`, `web/admin.html`, `settings.c`.

## Build and flash

```powershell
. C:\esp\v6.1\esp-idf\export.ps1
idf.py -C firmware/node build
idf.py -C firmware/node -p COM16 flash
```

All three nodes run the same image; only node 0 starts the web page.

## Test procedure (phone)

1. On the phone, join Wi-Fi `LG-MAIN`. The passphrase is `LG_SECRET_WIFI_PASSPHRASE` in `firmware/common/lg_secrets.h`.
2. If the phone warns that the network has no Internet, choose to stay connected. On Android, turning off mobile data avoids traffic going out over cellular.
3. Open `http://192.168.4.1/` in the browser.
4. Enter a network name and a password of at least 12 characters, then tap **Create LocalGrid**. Expect the dashboard within about 2 seconds.
5. Confirm the grid time shows **SET BY ADMIN** and matches the phone's clock.
6. On the serial console of NORTH or SOUTH, expect `[TIME] Adopted grid time ... from node 0`.
7. Confirm Infrastructure lists NORTH and SOUTH as ONLINE with a signal value.
8. Log out, then log in with a wrong password 5 times. Expect the lockout message on the 6th attempt.
9. Log in with the right password, tap **Use this device's time**, and expect the success message.
10. Reset COM16. Reload the page and expect the login screen, not setup.

## Expected serial output (COM16)

```
I (1057) WEB: [WEB] Admin page at http://192.168.4.1/ (setup mode)
I (1507) BB: [BB] Link up to node 1, RSSI -13, 0 clients there
I (...) WEB: [WEB] Setup complete: grid "Smith Family Camping", time zone America/Chicago
I (...) WEB: [WEB] Admin set grid time to 1790000000
```

## Measured

| Item | Value |
|---|---|
| PBKDF2-HMAC-SHA256 on classic ESP32 through PSA | 228 ms per 1,000 iterations |
| Login cost at 4,000 iterations | about 0.9 s |
| Master heap with web server, SoftAP, ESP-NOW, NimBLE | 63.3 KB free, 56.8 KB minimum, idle |
| Node binary with web page | 971 KB; 37% of the app partition free |
| Master boot to web page ready | about 1.1 s |

## Known limitations

- **Not yet exercised from a browser.** The HTTP handlers and page have only been built, not clicked through.
- **No captive portal yet.** The phone does not open the page automatically; type the address.
- **No factory reset from the page.** Clearing setup needs `idf.py -p COM16 erase-flash` and a reflash.
- **Grid time is lost on power-off**, by design (decision D6). The page shows NOT SET until the admin sets it again.
- **Assets are served uncompressed** (about 12 KB). Gzip comes later.
- **Master heap margin is 56.8 KB idle.** Heap under several browser connections has not been measured.
- **Nodes, groups, and devices are read-only;** editing comes with configuration distribution.
