# LocalGrid Phase 0 Design Review

Status: proposal for owner review. No firmware has been written.
Date: 2026-09-15. Working name: LocalGrid (nothing below depends on the name).

## How to read this review

Every hardware claim in this document came from a research pass over primary sources: ESP-IDF v6.1 documentation, ESP-IDF source and Kconfig files, Espressif GitHub issues, RFCs, and vendor board documentation. Ten research topics were run. Some claims were then re-checked by independent verifiers; the rest are marked **estimate** or **unmeasured** where they matter. Several load-bearing numbers have no primary measurement anywhere and must be measured on your bench. Those are called out as gates in the implementation sequence.

### What is on the bench

| Port | Board | Chip | Flash | PSRAM | Planned role |
|---|---|---|---|---|---|
| COM16 | Elegoo ESP32 dev board, CP2102 | ESP32-D0WD-V3 rev 3.1 | 4 MB | none | Infrastructure node (master candidate) |
| COM17 | Elegoo ESP32 dev board, CP2102 | ESP32-D0WD-V3 rev 3.1 | 4 MB | none | Infrastructure node |
| COM18 | Elegoo ESP32 dev board, CP2102 | ESP32-D0WD-V3 rev 3.1 | 4 MB | none | Infrastructure node |
| COM11 | Hosyond 3.2" ESP32 display, ST7789P3 240×320 SPI, resistive touch, CH340 (owner-confirmed) | ESP32-D0WD-V3 rev 3.1 | 4 MB | none | Classic client |
| COM9 | Freenove 2.8" ESP32-S3 display, native USB | ESP32-S3 rev 0.2 | 16 MB | 8 MB octal | S3 client |

**Board identity.** The owner's Amazon listing confirms the board is a Freenove **FNK0104** (2.8" IPS touch, microphone, speaker). Freenove's documentation defines FNK0104A as the non-touch 2.8" model and FNK0104B as the touch 2.8" model (ILI9341 plus FT6336U capacitive touch, I2C SDA 16 / SCL 15, address 0x38). The listing names the model "FNK0104A" while describing a touch screen, and it also contains obvious errors, such as an ARM processor and 384 KB of storage. This review therefore treats the board as the touch variant. Both variants share the ES8311 codec, MEMS microphone, speaker connector, SD, and battery connector. The client firmware detects touch at boot by probing I2C address 0x38, so the letter on the listing does not change any design decision.

The dev machine has PlatformIO with the frozen `espressif32 6.5.0` platform, which is Arduino 2.0.14 on ESP-IDF 4.4. That toolchain is unsuitable for this project for reasons in answer 38.

---

## Part A. Big picture

### 1. Understanding of LocalGrid

LocalGrid is a small private messaging network for a campsite with no outside connectivity. A handful of powered infrastructure nodes create radio coverage and carry traffic between each other. Handheld touchscreen devices attach to whichever node serves them best and exchange text: one-to-one, to a named group, or to everyone. One node, the master, holds the administrative truth: grid identity, the device registry, groups, configuration, and wall-clock time. It serves a local web page for the administrator.

Three properties define success. First, after one-time provisioning nobody touches network settings; power-on order does not matter. Second, the system degrades rather than fails: a lost node or a lost master removes coverage or administration, not the ability of reachable devices to talk. Third, it is honest and bounded: every queue has a limit, every timer has a value, and the security claims match what the code actually does.

The two firmware products share one protocol core. Nodes are infrastructure. Clients are endpoints and never forward anyone else's traffic.

### 2. Architecture diagram

```
                          LOCALGRID  (one fixed 2.4 GHz channel, e.g. ch 6)

   Admin phone / laptop
        |  Wi-Fi (WPA2, joins MASTER's SSID only)
        |  HTTP  http://192.168.4.1/
        v
 +---------------------------+      ESP-NOW backbone       +---------------------------+
 | NODE 0  "MAIN-CAMP"       |<===========================>| NODE 1  "TENT-NORTH"      |
 | role = MASTER             |   AEAD-sealed frames,       | role = NODE               |
 |  SoftAP 192.168.4.1/24    |   HELLO every 2 s,          |  SoftAP 192.168.5.1/24    |
 |  control TCP :7300        |   flood-with-dedup, TTL 3   |  control TCP :7300        |
 |  admin HTTP :80, DNS :53  |                             |                           |
 |  registry / config / time |                             |                           |
 +---------------------------+                             +---------------------------+
      ^            ^             \\                       //       ^              ^
      | TCP+Noise  | TCP+Noise    \\=====================//        | TCP+Noise    |
      |            |               \\                   //         |              |
   [CYD Dad]   [CYD Alex]         +---------------------------+   [CYD Emma]   [FNK0104 Ranger]
                                  | NODE 2  "TENT-SOUTH"      |
                                  |  SoftAP 192.168.6.1/24    |
                                  |  control TCP :7300        |
                                  +---------------------------+
                                        ^
                                        | TCP+Noise
                                    [CYD Bob]

 Firmware layering (both products)
 +--------------------------------------------------------------------------------+
 | UI (client only: LVGL screens)        | Admin web + DNS catch-all (master only) |
 +--------------------------------------------------------------------------------+
 | Services: messaging, presence, groups, store-and-forward, config, time, diag   |
 +--------------------------------------------------------------------------------+
 | LocalGrid protocol core  (pure C, no ESP-IDF headers, runs on host in tests)   |
 |   envelope codec · message IDs · dedup windows · bounded queues · routing      |
 +--------------------------------------------------------------------------------+
 | Security: Noise_NNpsk0 sessions · backbone AEAD · key derivation (lg_crypto)   |
 +--------------------------------------------------------------------------------+
 | Transports: TCP framed (client<->node)  ·  ESP-NOW framed (node<->node)        |
 +--------------------------------------------------------------------------------+
 | Connectivity: esp_wifi SoftAP / STA / scan · esp_now · lwIP · NVS             |
 +--------------------------------------------------------------------------------+
 | Board support: capability descriptor · esp_lcd panel · esp_lcd_touch · audio  |
 +--------------------------------------------------------------------------------+
 | ESP-IDF v6.1 · FreeRTOS · classic ESP32 or ESP32-S3                            |
 +--------------------------------------------------------------------------------+
```

### 3. Proposed infrastructure topology

**Recommendation: every node runs a SoftAP only, all nodes sit on one grid-wide channel, and nodes talk to each other over ESP-NOW.** There is no parent, no tree, and no station interface on a node during normal operation.

The backbone is a small neighbor graph, not a tree. Each node hears some set of other nodes. A message that must reach a node that is not a direct neighbor is flooded with a hop limit and suppressed by duplicate detection. With three to eight nodes, flooding costs almost nothing and needs no route computation.

Supported physical layouts, all without configuration:

```
 Full mesh (all in range)        Chain (A cannot hear C)         Star
      A ---- B                        A ---- B ---- C              B
       \    /                                                      |
        \  /                                                  A ---+--- C
         C                                                         |
                                                                   D
```

Why not the AP+STA tree the brief sketches is covered in answer 7. The short version: on a single-radio ESP32 a node's SoftAP is forced onto its uplink's channel, every uplink scan silences the node's own clients, and a parent loss cascades. ESP-NOW removes all of that.

Why ESP-NOW is a good fit here:

| Property | Value | Consequence |
|---|---|---|
| Payload per frame | 1470 B on ESP-IDF ≥ 5.4.2 (v2.0), 250 B on older (v1.0) | A whole text message plus headers fits one frame |
| Works on the SoftAP interface | Yes, peer `ifidx = WIFI_IF_AP` | Node stays AP-only |
| Needs association | No | No parent selection, nothing to heal |
| Default PHY rate | 1 Mbps, about 214 kbps measured in open air | Longest range; ample for text |
| Peer table | 20 total | Enough for eight nodes |
| Built-in encryption | Not used | It consumes SoftAP key slots and cannot encrypt broadcast |

**Unproven on this hardware:** Espressif documents ESP-NOW on the AP interface but ships no example of an AP-only classic ESP32 running ESP-NOW while serving a dozen stations. This is the first thing the one-week prototype tests.

### 4. How infrastructure nodes find one another

1. On boot a provisioned node loads its grid channel, node index, and backbone key from NVS.
2. It increments and commits its boot counter before any radio transmit. This counter is part of every backbone nonce.
3. It starts Wi-Fi in `WIFI_MODE_AP` on the grid channel, calls `esp_now_init`, and adds the broadcast peer on `WIFI_IF_AP`.
4. It sends `NODE_HELLO` as an ESP-NOW broadcast: five times at 200 ms with random jitter, then every 2 s.
5. Every node that receives a valid HELLO adds or refreshes a neighbor entry and adds a unicast ESP-NOW peer for that MAC on `WIFI_IF_AP`, channel 0, unencrypted at the ESP-NOW layer.
6. A neighbor is lost after three missed HELLOs, which is 6 s.

`NODE_HELLO` carries: node index, role, protocol version range, config generation, time quality, uptime, attached client count, free client slots, and a list of up to seven heard neighbors with their last-seen boot counter and link quality.

**Channel recovery.** A node that has zero neighbors for 60 s, while its config says other nodes exist, switches briefly to `WIFI_MODE_APSTA` and scans channels 1 to 11 for LocalGrid beacons with its grid discriminator and a higher config generation. If found, it adopts that channel. This handles a node that was powered off during a channel change. A SoftAP-only interface cannot scan, so this is the only place a node uses a station interface after setup.

Random power-on order is irrelevant because nothing waits for anything. The first node alive simply beacons; others join the graph when they appear.

### 5. How infrastructure nodes authenticate each other

**Frame authentication.** Every backbone frame is sealed with ChaCha20-Poly1305 under the backbone key. The 12-byte nonce is built from `sender node index (2 B) || zero (2 B) || boot counter (4 B) || sequence (4 B)`. Those same fields form a clear 12-byte outer header that is also the AEAD associated data. A receiver drops any frame whose tag fails, whose sender index is not in its node registry, or whose `(boot, seq)` falls outside that sender's replay window.

**Liveness proof.** Tags alone do not stop an attacker from replaying a recorded HELLO to a node that just rebooted and has an empty replay window. So a neighbor link becomes usable only after two-way confirmation: node A marks B as a working neighbor only when B's HELLO lists A with A's *current* boot counter. A replayed frame cannot contain a boot counter that did not exist when it was recorded. This is the same two-way check OSPF uses for adjacency.

**Membership.** The node registry, including revoked node indexes, is part of the master-issued configuration.

**Honest limit.** Every node holds the backbone key. A stolen node can impersonate the backbone until the grid key is rotated, which in the MVP means re-pairing every device. Answer 43 covers this.

### 6. Parent and upstream selection

With the recommended topology there is no parent or upstream. The questions a parent answers in a tree are answered differently:

| Tree concept | LocalGrid replacement |
|---|---|
| Which way to the master | Irrelevant for messaging; config and time flood across the graph |
| Which parent to join | None; every heard node is a neighbor |
| Loop prevention | No forwarding state exists; duplicates die in the dedup window and TTL |
| Healing after loss | A lost neighbor stops appearing; floods take the remaining paths |

**Next-hop choice for directed traffic.** If the destination node is a working neighbor, send ESP-NOW unicast to it. Otherwise flood: send unicast to every working neighbor except the one the frame came from, with TTL 3. Unicast rather than broadcast is used even for floods because unicast gets 802.11 MAC acknowledgements and retries; broadcast gets neither.

Link quality per neighbor is an exponentially weighted average of received RSSI from `esp_now_recv_info_t.rx_ctrl` plus HELLO loss over the last 30 s. In Phase 1 it only feeds diagnostics and the "backbone OK" flag that clients see. Phase 3 voice will need a real path, so the HELLO neighbor lists already carry enough to compute two-hop link-state later.

### 7. Is AP+STA appropriate?

**Not as the backbone.** AP+STA is appropriate only for two short moments: channel selection during master setup, and channel recovery described in answer 4.

What the ESP-IDF v6.1 documentation and source say about AP+STA on classic ESP32 and ESP32-S3:

- **One radio, one channel.** The SoftAP must use the station's home channel. If the station associates on another channel, the SoftAP moves by Channel Switch Announcement. Clients that ignore CSA must rescan.
- **Scans silence the SoftAP.** A background scan dwells 120 ms per channel off the home channel and returns for only 30 ms between channels. A full 11-channel scan leaves the node's own clients unserved for about 1.6 s in total. Espressif states the SoftAP's receive path is impacted while the station scans or connects.
- **Parent loss cascades.** A child station notices a vanished parent after the 6 s beacon timeout plus about 2.5 s of probe requests. It then scans and reconnects. If the new parent is on another channel, the child's clients get a forced channel switch too.
- **Loops are the application's problem.** No ESP-IDF primitive prevents A→B→C→A. Espressif's own ESP-Mesh-Lite had an open bug where the root joined its own child's SoftAP when no router existed.
- **Throughput halves per hop** on a single shared channel. Not an issue for text; relevant later for voice.
- **No measured SoftAP or AP+STA throughput exists** in Espressif's documentation. Only station iperf figures are published.

Ready-made alternatives were also rejected:

| Option | Reason rejected |
|---|---|
| ESP-WIFI-MESH (`esp_mesh`) | In router-less mode, ordinary stations cannot get IP service from a mesh node's SoftAP. Non-root nodes run no DHCP. About 60 KB RAM. Espressif steers new designs away. |
| ESP-Mesh-Lite | Each node NATs its own /24 and nodes on other branches are unreachable. It patches the IDF tree, does not build on IDF v6, and the maintained v2 is closed distribution. |
| AP+STA with your own TCP | Inherits every channel and cascade problem above, plus you write loop prevention. |

Classic ESP32 and ESP32-S3 behave identically for every item above.

### 8. One shared SSID or separate SSIDs

**Recommendation: separate SSIDs, same channel, same passphrase.** Example: `SMITH-MAIN`, `SMITH-NORTH`, `SMITH-SOUTH`. Normal users never see them.

Reasons:

- **The admin phone must reach the master.** Nodes do not route IP between each other. If every node shared one SSID, a phone would attach to whichever node is loudest and the admin page would be unreachable. A distinct master SSID makes "join `SMITH-MAIN`, open 192.168.4.1" a deterministic instruction.
- **Clients do not care.** A client selects by BSSID from a single-channel scan and reads grid identity from a beacon vendor element, not from the SSID. The SSID is just a string it copies into the connect call.
- **Shared SSIDs buy nothing here.** Their usual benefit is seamless roaming for third-party devices, which LocalGrid does not have to support, and ESP32 SoftAPs implement none of 802.11k/v/r anyway.
- **Logs are clearer.** A phone or laptop user reporting "I'm on SMITH-NORTH" tells you where they are.

SSID identity is never trusted. The grid is identified by an authenticated handshake after association.

The Wi-Fi passphrase is an admission control only, randomly generated at setup with at least 16 characters and shown to the admin once. It is not derived from any LocalGrid key, so giving it to an admin phone discloses nothing that lets the phone impersonate a device.

---

## Part B. Clients

### 9. How clients discover infrastructure

Clients know the grid channel from provisioning. Discovery is a Wi-Fi scan of that one channel, not BLE.

1. **Single-channel active scan.** `wifi_scan_config_t.channel` set to the grid channel, active dwell 30 to 70 ms, `show_hidden` false. Cost is roughly 70 to 150 ms.
2. **Identify LocalGrid nodes.** Each node SoftAP carries one vendor-specific information element in beacons and probe responses, set with `esp_wifi_set_vendor_ie`. The client reads it through `esp_wifi_set_vendor_ie_cb`, which delivers the element, source MAC, and RSSI during scans. Scan records themselves carry no raw elements.
3. **Fallback.** After three empty single-channel scans, scan channels 1 to 11 in case the grid channel changed while the client was off. A full scan takes about 2.4 s while connected.

Beacon element payload (about 24 B, unauthenticated hint only):

| Field | Size | Purpose |
|---|---|---|
| Format version | 1 B | Forward compatibility |
| Grid discriminator | 4 B | First 4 bytes of HMAC-SHA256(grid root, "lg-disc"); filters other grids |
| Node index | 2 B | Maps BSSID to node |
| Role | 1 B | MASTER or NODE |
| Protocol version range | 2 B | Skip incompatible nodes |
| Attached clients / free slots | 2 B | Load-aware selection |
| Flags | 1 B | Backbone OK, pairing open, setup mode |
| Config generation, low 16 bits | 2 B | Hint that fresher config exists |

Anyone can forge a beacon. A forged node only wastes a client's time, because the session handshake in answer 26 fails without the device key.

### 10. How clients connect automatically

**Selection score** for each candidate heard in the scan:

```
score = EWMA_RSSI_dBm
        + 3   if this is the node we were last attached to     (stickiness)
        - 3 × max(0, clients_attached - 8)                      (load)
        - 20  if "backbone OK" is clear and another candidate has it set
        excluded if free_slots == 0, or RSSI < -85 dBm, or blacklisted
```

**Connect sequence:**

1. `esp_wifi_set_config` with the candidate's SSID, the grid passphrase, `bssid_set = 1`, `channel = grid channel`, `scan_method = WIFI_FAST_SCAN`, and 802.11k/v/r disabled. Leaving BTM enabled would clear the fixed BSSID on the next reconnect.
2. `esp_wifi_connect`.
3. **Static IP, no DHCP.** Client address = `192.168.(4 + node index).(100 + device index)`, gateway `.1`. Espressif's lwIP DHCP client adds 1 to 3.5 s per new lease because of ARP checks. Static addressing removes that from every roam. Each node's DHCP pool is `.2` to `.99`, used only by phones and laptops.
4. TCP connect to port 7300 on `.1` with `TCP_NODELAY`.
5. Noise session handshake, answer 26.
6. `REGISTER` with device index, friendly name, firmware version, capability bits, cached config generation, and a summary of undelivered outbox messages.
7. `REGISTER_ACK` with node index, grid time and time quality, current config generation, and the first page of presence.
8. If config generations differ, `CONFIG_REQUEST`. Then normal operation.

**Expected time to CONNECTED:** scan 0.15 s, association 0.1 to 0.35 s, TCP plus Noise handshake about 0.1 to 0.2 s, register under 0.1 s. Total under 1 s after boot-up of the radio. **Estimate; community numbers are against commercial APs, never against an ESP32 SoftAP. Measured at milestone 1D.**

Retry policy: on failure, next-best candidate immediately; after all candidates fail, back off 1, 2, 4, 8, then every 10 s with a fresh scan each time. UI shows SEARCHING.

### 11. Roaming strategy

ESP-IDF roaming is always **break-before-make**. Espressif rejected make-before-break for single-radio chips. lwIP tears down sockets on disconnect. So the design goal is not a seamless roam; it is a short, bounded gap that the messaging layer hides.

**Triggers** (either one):

- EWMA RSSI of the current node below **-72 dBm** for 5 s. Samples come from `esp_wifi_sta_get_rssi` at 1 Hz, which returns a single-beacon reading that needs smoothing.
- Two of the last five session PINGs unanswered.

`esp_wifi_set_rssi_threshold` is armed at the degraded level to wake the roaming logic early. It is one-shot, so it is re-armed 5 dB lower after each event and reset when RSSI recovers, the same pattern Espressif's roaming app uses.

**While degraded:** single-channel scan every 5 s. While healthy: every 60 s, to keep load and health hints fresh.

**Roam only when all hold:**

| Condition | Default | Configurable |
|---|---|---|
| Candidate better by | ≥ 8 dB | yes |
| Better across | 2 consecutive scans, at least 5 s apart | yes |
| Candidate has free slots and backbone OK | required | no |
| Time since last roam | ≥ 20 s | yes |
| No voice session active (Phase 3) | required | no |
| Candidate blacklisted | no; 3 failed connects blacklist a BSSID for 5 min | yes |

These defaults sit between Apple's published client behavior (roam scan at -70 dBm, candidate 8 to 12 dB better) and Espressif's roaming app defaults (5 to 15 dB difference, 15 s backoff). They are far above the ESP32's stationary RSSI noise, which one measurement put at about 0.5 dB. Bodies and pockets cause much larger swings, which is why the persistence requirement exists.

**Emergency path:** if the station disconnects on its own, skip hysteresis: scan, pick the best candidate, connect.

**Roam sequence:** `ROAM_STARTED` event, `esp_wifi_disconnect` (sends a deauth so the old node frees the slot immediately), connect as in answer 10 with the new BSSID, register, re-offer outbox, `ROAM_COMPLETED` with duration. Status bar shows RECONNECTING only if the gap exceeds 2 s.

**Expected roam gap:** 0.5 to 1.5 s. **Unmeasured.** One open question is whether calling `esp_wifi_set_config` with an unchanged passphrase re-runs the WPA2 key derivation, which costs 450 to 850 ms on classic ESP32. Measured at milestone 1M.

### 12. BLE role

**Owner decision, 2026-09-15: BLE stays in Phase 1.** The original recommendation to drop BLE was rejected. The evidence behind it is kept below because it shapes how BLE is included and what the prototype must measure.

**Phase 1 BLE design:**

| Product | BLE role | Stack settings | Why this role |
|---|---|---|---|
| Node | Broadcaster only: non-connectable advertising, no scanning, no connections | NimBLE, broadcaster role only, controller BLE-only mode | Advertising is the lightest BLE activity; scanning and connections cost more airtime |
| Client | Observer only: passive scan for node adverts while SEARCHING or RECONNECTING, and for 1 s every 30 s while CONNECTED | NimBLE, observer role only | Coexistence with a connected Wi-Fi station is rated stable for BLE scanning on both chips |

**Advertisement payload** (31-byte legacy advert): 16-bit LocalGrid service UUID, then service data of about 20 bytes holding format version, 4-byte grid discriminator, node index, role, protocol version, free client slots, and a backbone-OK flag. Like the Wi-Fi beacon element, it is an unauthenticated hint. A forged advert only wastes a client's scan.

**How discovery uses both radios:** a client combines BLE adverts and the Wi-Fi single-channel scan into one candidate list keyed by node index. BLE gives an early "a LocalGrid node is nearby" signal and a second RSSI reading. The Wi-Fi scan still supplies the BSSID needed to connect. Either source alone is enough to find a node, so losing one radio never blocks connection.

**Advert interval:** 500 ms by default, configurable 100 ms to 2 s. A longer interval reduces airtime taken from Wi-Fi and ESP-NOW.

**Costs this decision carries, each measured in the prototype:**

1. **Backbone receive on nodes.** Espressif does not support ESP-NOW receive alongside active BLE when Wi-Fi is not in station mode. *Prototype test:* backbone frame loss on a node with advertising off, then at 500 ms and 100 ms intervals. *If loss is unacceptable:* advertise in short duty-cycled bursts, or move the node-to-node link off ESP-NOW (answer 7 lists the alternatives).
2. **Access point stability on nodes.** SoftAP with any BLE activity is rated "supported but unstable". *Prototype test:* 15 connected stations with advertising on, watching disconnects and round-trip time.
3. **Classic CYD memory.** Bluetooth reserves about 56 KB of static RAM at build time and NimBLE uses roughly 30 KB more at runtime. That pushes the CYD's estimated headroom from about 116 KB to about 26 KB, below the 40 KB floor. *Prototype test:* measured minimum free heap. *If short:* the LVGL and Wi-Fi shrink ladder in answer 40 applies first. The FNK0104B has 8 MB PSRAM and is not affected.
4. **Flash.** NimBLE adds an estimated 250–350 KB, which likely forces the classic CYD onto the single-app partition layout in answer 40.

Evidence from the original analysis:

- **Nodes.** Espressif's coexistence table rates SoftAP-connected plus any BLE activity as "supported but performance unstable" on both chips. It also marks ESP-NOW *receive* with BLE active as supported only in station mode. A node needs stable SoftAP and ESP-NOW receive. An open issue shows 100% ESP-NOW receive loss on classic ESP32 while BLE scans.
- **Classic CYD.** Enabling Bluetooth reserves about 56 KB of static DRAM at link time. NimBLE measured another 32.5 KB of heap. That is most of the headroom left after Wi-Fi and LVGL.
- **No benefit.** With all nodes on one channel, a Wi-Fi scan finds every node in about 100 ms and also yields the BSSID and RSSI needed to connect. BLE would add a second, weaker signal to reconcile.

With BLE kept, Wi-Fi scanning remains a full discovery path in its own right, as the brief requires.

### 13. Wi-Fi role

Wi-Fi carries everything in Phase 1:

| Link | Mechanism | Why |
|---|---|---|
| Client ↔ node | 802.11 association, WPA2-PSK, TCP | Reliable ordered delivery, standard tools |
| Node ↔ node | ESP-NOW action frames on the same channel | No association, no tree, whole-message frames |
| Admin phone ↔ master | 802.11 association, HTTP | Browser-native |
| Discovery | Beacon vendor element in a single-channel scan | Free with the SoftAP |

**Client power save:** keep `WIFI_PS_MIN_MODEM` with SoftAP DTIM period 1 and beacon interval 100 TU. Pushed messages wait at most one beacon interval, about 102 ms. `WIFI_PS_MAX_MODEM` is not used because it adds 307 ms and may lose broadcast frames. Voice in Phase 3 switches to `WIFI_PS_NONE` for the duration of a talk session.

**Node SoftAP settings:**

| Setting | Value | Reason |
|---|---|---|
| `max_connection` | 15 | ESP32 and S3 maximum; ESP-NOW encrypted peers set to 0 so no key slots are lost |
| `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM` | 0 | Application-layer AEAD replaces ESP-NOW encryption |
| `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` | 16 | Default 8 silently evicts the oldest lease |
| SoftAP inactivity (`esp_wifi_set_inactive_time`) | 30 s | Default 300 s; idle ESP32 stations send keepalives every 10 s |
| Client station beacon timeout | 3 s | Default 6 s; dead node noticed in about 5.5 s |
| Auth mode | WPA2-PSK, PMF capable | WPA3 adds SAE cost without benefit over a random passphrase; test PMF-required with the owner's phones |
| Country / channels | channel 1, 6, or 11 | Default country code only scans 1 to 11 |

### 14. Transport for text and control

**Client ↔ node: one persistent TCP connection per client.**

- Framing: 2-byte little-endian length, then one Noise transport message. Maximum frame 1024 B, below the 1440 B MSS.
- `TCP_NODELAY` on. Kernel keepalive as a backstop at idle 10 s, interval 5 s, count 3.
- **Application PING every 10 s.** A client is considered gone after 30 s of silence. Without this, lwIP would take about 19 minutes to abort a dead connection with its default retransmission table.
- **Node-side per-client output limit of 1024 B unacknowledged.** Excess waits in the bounded application queue. This keeps a stalled client from pinning kernel send buffers.
- `CONFIG_LWIP_MAX_SOCKETS` raised from 10 to 24. `CONFIG_LWIP_TCP_MSL` lowered from 60 s to 10 s so a client that reconnects repeatedly does not hold sockets in TIME_WAIT for two minutes.

Why TCP over UDP with custom retransmission: TCP already gives ordering and hop-level retransmission, which the Noise session requires. The heap risk of TCP buffers is handled by the output limit above.

**Node ↔ node: ESP-NOW frames.** Unicast gets MAC-layer acknowledgement and retries. A backbone `NODE_ACK` is returned for frames carrying messages, with retries at 300, 600, and 1200 ms before the node gives up on that neighbor. A single sender state machine keeps exactly one ESP-NOW frame in flight and advances on the send callback. That is the field-validated workaround for an open ESP-IDF bug where unpaced sends permanently wedge with `ESP_ERR_ESPNOW_NO_MEM`.

WebSockets and HTTP polling are not used anywhere in the device protocol.

### 15. Transport for future voice

- **Client ↔ node:** UDP, one datagram per 20 ms frame, 12-byte RTP-style header (16-bit sequence, 32-bit timestamp in samples, 32-bit stream ID). Datagrams sealed with a per-session voice key derived from the Noise session. No retransmission.
- **Codec starting point:** G.711 µ-law, 160 B per 20 ms, about 0.3% CPU on ESP32-S3 per Espressif's codec component. IMA ADPCM at 80 B per frame if airtime or fan-out demands it.
- **Jitter buffer:** 60 ms, three frames, growing to 120 ms under loss.
- **Node ↔ node:** ESP-NOW unicast along a computed path, not flooding. At 1 Mbps ESP-NOW measured about 214 kbps; one µ-law stream at 80 kbps with headers fits per hop, but two concurrent streams or a raised PHY rate need measurement.
- **Fan-out to listeners:** unicast per listener. The ESP32 SoftAP does not buffer multicast for sleeping stations.

**Capability reality:** classic CYDs have an 8-bit DAC and amplifier but no microphone, so they can listen but not talk. The FNK0104 has an ES8311 codec and a microphone and is the natural push-to-talk target.

---

## Part C. Protocol

### 16. Application protocol

Four layers, each independently versioned:

| Layer | Client ↔ node | Node ↔ node |
|---|---|---|
| Link framing | 2-byte length prefix on TCP | One ESP-NOW frame |
| Secure channel | Noise_NNpsk0_25519_ChaChaPoly_SHA256 session | 12-byte clear header + ChaCha20-Poly1305 |
| Envelope | 32-byte fixed binary header | same |
| Body | nanopb-encoded protobuf, bounded | same |

**Why a fixed binary envelope:** it is parsed only after the AEAD tag verifies, it has no variable-length fields to mis-parse, and routing code needs nothing else.

**Why nanopb bodies:** field numbers give forward and backward compatibility, `.options` files bound every string and repeated field to fixed arrays so decoding never allocates, and host-side Python tests can use standard protobuf. Code is 5 to 20 KB. Meshtastic uses exactly this pattern on ESP32. TinyCBOR (Espressif's `espressif/cbor` component) is the fallback if the protobuf toolchain causes friction on Windows. JSON is used only for the admin web API and diagnostics dumps.

**Versioning rules:**

- Envelope byte 0 holds protocol major (high nibble) and minor (low nibble).
- `NODE_HELLO` and `REGISTER` carry supported min and max. Peers use the highest common version.
- Unknown major: reply `ERROR(UNSUPPORTED_VERSION)`, log it, and drop.
- Unknown message type with a higher minor: ignore it. Unknown body fields are skipped by nanopb.
- A client firmware too old for every node shows "Update required" rather than looping.

**Initial message types:**

| ID | Type | Scope | Direction |
|---|---|---|---|
| 0x01 | NODE_HELLO | SYSTEM | node → neighbors (broadcast) |
| 0x02 | NODE_ACK | SYSTEM | node → node |
| 0x03 | NODE_STATUS | SYSTEM | node → all nodes (flood, every 30 s) |
| 0x10 | REGISTER | SYSTEM | client → node |
| 0x11 | REGISTER_ACK | SYSTEM | node → client |
| 0x12 | PING / 0x13 PONG | SYSTEM | either |
| 0x20 | PRESENCE_UPDATE | SYSTEM | node → nodes, node → clients |
| 0x21 | PRESENCE_REQUEST | SYSTEM | client → node, node → node |
| 0x22 | PRESENCE_DIGEST | SYSTEM | node → nodes (every 30 s) |
| 0x23 | NAME | SYSTEM | client → node, node → nodes (flood), node → clients. A handheld's chosen name, versioned, newest wins (D50). Body: u32 device, u32 version, u8 length, 1..23 bytes of UTF-8 |
| 0x30 | TEXT_MESSAGE | DIRECT / GROUP / BROADCAST | any |
| 0x31 | MESSAGE_ACK | DIRECT | any |
| 0x40 | CONFIG_ANNOUNCE | SYSTEM | master → nodes |
| 0x41 | CONFIG_REQUEST | SYSTEM | node → node, client → node |
| 0x42 | CONFIG_CHUNK | SYSTEM | node → node, node → client |
| 0x43 | CONFIG_ACK | SYSTEM | node → master |
| 0x50 | TIME_SYNC | SYSTEM | master → nodes, node → clients |
| 0x60 | PAIRING (SRP messages) | SYSTEM | unprovisioned device ↔ master |
| 0x7F | ERROR | SYSTEM | any |
| 0x80–0x8F | reserved: VOICE_START, VOICE_FRAME, VOICE_END, VOICE_CANCEL, TTS capability | | |

`GROUP_UPDATE` and `ROUTE_UPDATE` from the brief are folded into `CONFIG_*` and `NODE_STATUS` respectively. Groups are configuration; routes are not stored.

### 17. Message envelope

Little-endian, 32 bytes, inside the encrypted payload on both transports:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `version` | major << 4 \| minor |
| 1 | 1 | `type` | table above |
| 2 | 2 | `flags` | ACK_REQUESTED, URGENT, E2E_PAYLOAD, FRAGMENT, RELAYED |
| 4 | 1 | `scope` | 0 SYSTEM, 1 DIRECT, 2 GROUP, 3 BROADCAST |
| 5 | 1 | `ttl` | backbone hop limit, starts at 3 |
| 6 | 2 | `body_len` | ≤ 1400 |
| 8 | 4 | `origin_id` | device or node index of the author |
| 12 | 4 | `origin_boot` | author's boot counter |
| 16 | 4 | `origin_seq` | author's per-boot sequence |
| 20 | 4 | `target` | device index, group ID, or 0xFFFFFFFF |
| 24 | 4 | `grid_time` | seconds since Unix epoch, 0 if unknown |
| 28 | 2 | `origin_node` | node where the message entered the backbone |
| 30 | 2 | `reserved` | must be zero |

Backbone outer header, clear but authenticated as AEAD associated data:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | outer version |
| 1 | 1 | key epoch |
| 2 | 2 | sender node index |
| 4 | 4 | sender boot counter |
| 8 | 4 | sender frame sequence |

Per-frame overhead on the backbone: 12 B outer + 32 B envelope + 16 B tag = 60 B. A 240-byte text body produces a 300-byte frame, far under 1470 B. An outside sniffer sees node indexes and counters only; message type, author, and target are encrypted.

Text body limit: 240 bytes of UTF-8. No fragmentation is needed in Phase 1. The FRAGMENT flag exists for config transfer.

### 18. Addressing and identity

| Identifier | Size | Generated by | Scope | Changes when |
|---|---|---|---|---|
| Grid ID | 128-bit random | master at setup, after RF is on | global | factory reset of the grid |
| Grid discriminator | 32-bit | HMAC of grid root | beacon filter | grid key rotation |
| Device UUID | 128-bit random | device at first boot | global, stored in registry | device factory reset |
| Device index | 32-bit | master, monotonic, never reused | wire address within the grid | re-provisioning |
| Node index | 16-bit | master at adoption | wire address, IP subnet, nonce prefix | re-adoption |
| Group ID | 16-bit | master | wire target for GROUP scope | never; groups are renamed, not renumbered |
| Friendly name | ≤ 24 UTF-8 bytes | admin | display only | any time |

**One user per handheld (owner direction, 2026-09-15).** A user *is* a device: the device index is the user's address, the friendly name is the user's name, and there are no logins, user accounts, or shared handhelds. Handing a CYD to someone else means the admin renames or re-pairs it. This keeps the handheld free of any user-switching UI and credentials.

The MAC address is deliberately not an identity. Boards get swapped and MACs are visible to anyone.

**Message identity is `(origin_id, origin_boot, origin_seq)`.** The boot counter lives in NVS and is incremented and committed before the device transmits anything. The sequence starts at 1 each boot and is never reused within a boot; on the unlikely wrap past 2³² the device reboots.

Why this instead of random 128-bit IDs:

- **No wall clock involved**, so time corrections cannot collide IDs.
- **Duplicate detection becomes an IPsec-style sliding window** per author: a 128-bit bitmap plus the highest accepted `(boot, seq)`, 24 bytes per author. Random IDs would need a hash set of recent IDs with time-based eviction.
- **The same tuple forms the backbone nonce**, so ID uniqueness and nonce uniqueness are one mechanism with one test.
- **NVS wear is trivial:** one 32-byte entry per boot, one 4 KB page erase per 126 boots.

If NVS is unreadable, the boot counter, device index, and keys are all gone together, and the device returns to the unprovisioned state. A counter can therefore never silently restart while its old key remains.

### 19. Presence

**Where it lives:** every node keeps the full grid presence table, bounded at 32 devices. Clients receive the table from their node.

| Field | Size |
|---|---|
| device index | 4 B |
| attached node index | 2 B |
| attach epoch | 4 B |
| state | 1 B |
| last seen, node uptime seconds | 4 B |
| capability bits | 2 B |

Names come from configuration, not presence.

**Mechanism:**

1. A node learns about attach and detach first-hand: `REGISTER`, session PINGs, TCP close, and the 30 s silence timeout.
2. Changes are flooded as `PRESENCE_UPDATE` immediately.
3. Every 30 s each node floods a `PRESENCE_DIGEST` holding a hash of its table. A node whose hash differs requests the full table from that neighbor. This repairs missed updates without periodic full dumps.
4. **Conflicts** (two nodes claiming the same device after a roam) are resolved by the higher attach epoch. The client increments the epoch on each registration.
5. Clients get `PRESENCE_UPDATE` pushes for any change and a full snapshot at registration.

**States shown:**

| State | Meaning | Rule |
|---|---|---|
| ONLINE | attached somewhere reachable | session alive on a node that this node can reach |
| NEARBY | attached to the same node as the viewer | ONLINE and same node index |
| RECENTLY SEEN | not attached, seen within 15 min | configurable window |
| OFFLINE | not seen within the window, or unreachable partition | |

The UI labels NEARBY as "on your node", not "physically near". Being on the same node only means both devices hear the same tent.

### 20. Routing

Routing is location lookup plus the backbone forwarding rule from answer 6.

```
deliver(msg):
  if msg.scope == DIRECT:
      loc = presence[msg.target]
      if loc.node == self:           send to local session; await MESSAGE_ACK
      elif loc.node is reachable:    backbone_send(loc.node, msg)
      else:                          store_and_forward.enqueue(msg)   (answer 25)
  if msg.scope in (GROUP, BROADCAST):
      backbone_flood(msg)            (every node delivers to its local members)
      deliver locally to members attached here

backbone_send(dest_node, msg):
  if dest_node is a working neighbor:   ESP-NOW unicast to dest_node
  else:                                 backbone_flood(msg)

backbone_flood(msg):
  for n in working_neighbors except arrival neighbor:
      ESP-NOW unicast to n with ttl-1  (drop if ttl reaches 0)

on backbone receive:
  verify tag and replay window on outer header
  if (origin_id, origin_boot, origin_seq) already seen: drop
  handle locally, then continue flood if flooding and ttl > 0
```

Nodes deduplicate on the message tuple, independent of the per-frame replay window. That is what prevents loops: a message that circles back is recognized and dropped, and TTL bounds the worst case.

Dedup table on nodes: 48 authors × 24 B = 1.2 KB, least recently used eviction. An evicted author's old messages could reappear only if replayed after eviction and within the TTL, which the per-message `grid_time` age check (reject if more than 2× retention old when time is known) further bounds.

### 21. Direct messaging flow

Alice on node A sends to Bob on node C, chain A–B–C.

```
Alice(CYD)          Node A              Node B              Node C            Bob(CYD)
   | TEXT id=(7,42,19) |                   |                   |                  |
   |------------------>|                   |                   |                  |
   |  MESSAGE_ACK      |                   |                   |                  |
   |  status=ACCEPTED  |  presence: Bob@C, C not a neighbor -> flood             |
   |<------------------|--ESPNOW---------->|--ESPNOW---------->|                  |
   |                   |<--NODE_ACK--------|<--NODE_ACK--------|  TEXT            |
   |                   |                   |                   |----------------->|
   |                   |                   |                   |  MESSAGE_ACK     |
   |                   |                   |                   |  DELIVERED       |
   |                   |<---------------flood back-------------|<-----------------|
   |  MESSAGE_ACK      |                   |                   |                  |
   |  DELIVERED        |                   |                   |                  |
   |<------------------|                   |                   |                  |
```

UI states on Alice's screen: `sending` → `sent` (node accepted custody) → `delivered` (Bob's device acknowledged). `queued` if Bob is offline. `failed` when retention expires.

Bob's device deduplicates on the message tuple before displaying, so a re-offered copy after a roam shows once.

### 22. Group messaging flow

Alice sends "Dinner at 7." to FAMILY (group 3: Dad, Alex, Emma, Alice).

1. Alice's node checks that Alice is a member of group 3 in its cached config. Non-members get `ERROR(NOT_A_MEMBER)`.
2. Node A floods the message once across the backbone.
3. Every node delivers to locally attached members of group 3 and never to non-members. Membership enforcement happens at every delivering node, not only at the origin.
4. Each receiving member returns `MESSAGE_ACK DELIVERED`, flooded back to the origin node, which forwards it to Alice.
5. For members who are offline, the origin node queues a per-member copy. Only the origin node queues, so a member reconnecting anywhere gets exactly one copy.
6. Alice's UI shows "delivered 2 of 3" and updates as ACKs arrive.

**MVP honesty:** in Phase 1 nodes can read group messages. "Non-members do not receive it" is enforced by node code, not cryptography. Answer 26 describes the future group-key path.

### 23. Broadcast messaging flow

Same as group with an implicit member list of every provisioned, non-revoked client.

- Broadcast has a per-device rate limit: one per 10 s by default, adjustable in config. Admin-issued broadcasts from the web UI are exempt.
- Offline recipients are queued at the origin node only if the message carries the URGENT flag. Routine broadcasts ("Dinner is ready") are live-only to protect the store-and-forward pool. "Storm coming" should be sent as URGENT.
- The sender sees a delivered count, not per-person ticks.

This is an application-layer broadcast. No Wi-Fi broadcast or IP multicast frames are used anywhere, because the ESP32 SoftAP does not buffer group-addressed frames reliably for power-saving clients.

### 24. Acknowledgement and retry strategy

Reliability is **end-to-end**, with custody hand-offs to keep the common case fast.

| Hop | Mechanism | Timers | Give up |
|---|---|---|---|
| Client → node | TCP, then `MESSAGE_ACK ACCEPTED` | ACCEPTED expected within 3 s | Session reset; message stays in client outbox |
| Node → node | ESP-NOW MAC ACK, then `NODE_ACK` | 300, 600, 1200 ms | Neighbor marked degraded; flood via others if any |
| Node → client | TCP, then `MESSAGE_ACK DELIVERED` from recipient | DELIVERED expected within 5 s | Copy moves to store-and-forward |
| End to end | Origin client keeps the message until DELIVERED or retention expiry | re-offer on every new session | `failed` state in UI |

**Client outbox:** 16 messages, persisted in NVS as a small ring, so a sending device can reboot or roam without losing an accepted-but-undelivered message. On each new session the client re-offers undelivered messages with their original IDs. Deduplication at nodes and recipients ensures one display.

**Why the sender retains a copy:** node queues live in RAM. A node that reboots loses its queue. The sender's outbox makes that survivable without writing every queued message to node flash.

**Recipient deduplication after a recipient reboot:** the recipient persists the highest delivered `(boot, seq)` per author alongside its message history, so a re-offer after the recipient rebooted still shows once.

### 25. Bounded store-and-forward

Held at the **origin node** only.

| Limit | Default | Range |
|---|---|---|
| Pool size per node | 64 messages | 16–128 |
| Per recipient | 8 | 1–16 |
| Per author | 16 | 4–32 |
| Retention | 15 min | 5, 15, 30, 60 min |
| Memory | 64 × 300 B ≈ 19 KB, preallocated at boot | fixed |
| Broadcast copies | URGENT only | |

**When full:**

1. Expired entries are purged first.
2. A new DIRECT or URGENT message is rejected with `MESSAGE_ACK REJECTED(QUEUE_FULL)`. The sender's UI shows "not delivered: mailbox full". Nothing already queued is silently evicted.
3. A per-recipient or per-author cap produces the same rejection.

**Flush:** when a `PRESENCE_UPDATE` shows a queued recipient ONLINE, the origin node sends queued copies in order with normal ACK handling, paced at two per second so a reconnecting device is not flooded.

**Persistence:** RAM only in Phase 1, by design. The sender outbox is the durable copy.

---

## Part D. Security

### 26. Security model

**Threats in scope:** other campers with phones, laptops, Wi-Fi and BLE scanners, and their own ESP32s; replay of captured frames; forged beacons; a buggy or malicious client flooding its node; a lost or stolen handheld.

**Out of scope for Phase 1, stated plainly:** physical extraction of keys from a stolen node, fault-injection attacks on classic ESP32 silicon, and radio jamming.

| Property | Phase 1 (MVP) | Future (E2E) |
|---|---|---|
| Admission to Wi-Fi | WPA2-PSK, random 16+ char passphrase | same |
| Client ↔ node | Noise_NNpsk0 session keyed by per-device key; mutual authentication; forward secrecy for session traffic | same |
| Node ↔ node | ChaCha20-Poly1305 under backbone key, replay window, two-way liveness | same |
| Direct message privacy | **End-to-end (owner requirement).** Only sender and recipient can read it; nodes forward ciphertext | Add forward secrecy and a master-signed key directory |
| Group privacy | **Enforced by node code. Nodes see plaintext.** | Per-group key wrapped for each member; rotated on membership change |
| Broadcast privacy | Readable only by admitted devices and nodes | same |
| Metadata | Nodes see author, target, type, timing | Unavoidable for routing |
| Stolen client | Admin revokes device index; nodes refuse its handshake | same |
| Stolen node | **Backbone and device keys compromised until full re-pair** | Per-node keys and signed config reduce blast radius |

The envelope already has an `E2E_PAYLOAD` flag and treats the body as opaque to routing, so end-to-end encryption can be added without changing the envelope.

**Direct messages are end-to-end encrypted (owner requirement, 2026-09-15).** Nobody except the two people in a 1:1 conversation can read it: not other campers, not other LocalGrid users, and not the infrastructure nodes.

- **Keys:** each handheld generates an X25519 key pair once and keeps the private key in NVS. Its public key travels in `REGISTER` and in `PRESENCE_UPDATE`.
- **Pairwise key:** `K_pair = HKDF-SHA256(X25519(my_private, peer_public), "lg direct v1" || lower device index || higher device index)`, computed once per peer and cached. About 15 ms per new peer, then nothing.
- **Per message:** ChaCha20-Poly1305 under `K_pair`. Nonce = `origin_id || origin_boot || origin_seq`, which never repeats because it includes the author and a boot counter. Associated data is the envelope fields nodes never change: type, scope, stable flags, author, boot, sequence, target, grid time. Overhead is the 16-byte tag.
- **Enforcement:** nodes reject any DIRECT text without the `E2E_PAYLOAD` flag, so an unencrypted 1:1 message cannot be sent by mistake.
- **Trust in public keys:** in the prototype, a handheld pins the first public key it sees for each person and warns loudly if it ever changes. That stops passive eavesdroppers and other users, but a malicious node present at first contact could substitute keys. In Phase 1 the master signs the key directory at pairing, which closes that gap.
- **Not provided:** forward secrecy. A stolen handheld's private key decrypts that user's past 1:1 messages if an attacker also recorded them. Rotating a user's key pair is an admin action.

Group and broadcast text remain encrypted in transit and readable by nodes, as stated in the table above.

**Primitive choices and why:**

| Primitive | Use | Reason |
|---|---|---|
| ChaCha20-Poly1305 | all AEAD | Neither classic ESP32 nor ESP32-S3 has hardware GCM. Software ChaCha20-Poly1305 measured about 3.1–3.3 MB/s versus 1.4–1.8 MB/s for AES-GCM, in constant time. A 200-byte message costs about 0.1 ms either way. |
| HKDF-SHA256 | key derivation | Standard, cheap |
| HMAC-SHA256 | grid discriminator, config authentication | Standard |
| X25519 | Noise ephemeral keys | About 14–17 ms per operation on both chips (libsodium measurements); used only at connect and roam |
| PBKDF2-HMAC-SHA256 | admin password | The only practical password hash; Argon2's memory floor is unreachable on classic ESP32 |
| Not used | AES-GCM, ECDSA P-256, RSA, TLS | P-256 verify measured 300–420 ms in mbedTLS; TLS costs about 40 KB per connection |

**Why Noise_NNpsk0 for sessions rather than a custom handshake:** Noise is a published, analyzed protocol framework. ESPHome runs Noise_NNpsk0_25519_ChaChaPoly_SHA256 in production on classic ESP32, with a full handshake measured at 63–64 ms. The pre-shared key is the device key. The initiator sends its device index in the Noise prologue so the node can select the key. Transport messages use Noise's own 64-bit nonce counters, which TCP ordering satisfies.

**Implementation gate:** the Noise library (ESPHome's `noise-c` fork with libsodium, or noise-c on mbedTLS PSA) must build on ESP-IDF v6.1 at milestone 1A. If it cannot, the fallback is a documented PSK challenge-response using HMAC-SHA256 and HKDF modeled on the TLS 1.3 PSK key schedule. That fallback loses forward secrecy, and the review will say so if it is used.

**Online attack limits:**

- Session handshake failures: 5 per source MAC in 60 s block that MAC for 60 s, doubling to 15 min.
- Pairing attempts: 5 wrong codes close the pairing window.
- Admin login: exponential backoff per client IP, 1 s doubling to 60 s after 5 failures, plus a global limit.
- Per-client rate limit on the node: 2 messages per second, burst 5. Excess gets `ERROR(RATE_LIMITED)`.

Device keys are 256-bit random derivatives, so online guessing is not the threat; the limits exist to bound CPU and log noise.

### 27. Key and provisioning model

**Key hierarchy:**

```
grid_root (32 B, random, generated on master after Wi-Fi start)
 ├─ K_backbone[epoch]   = HKDF(grid_root, "lg backbone" || grid_id || epoch)      → all nodes
 ├─ K_device[i, epoch]  = HKDF(grid_root, "lg device" || grid_id || i || epoch)   → device i only
 ├─ K_config            = HKDF(grid_root, "lg config" || grid_id)                → all nodes
 └─ discriminator       = HMAC(grid_root, "lg-disc")[0..3]                         → beacons

wifi_passphrase         independent random string, shown to admin
admin_credential        PBKDF2-HMAC-SHA256(password, 16 B salt, calibrated iterations)
setup_code[node]        8 random digits generated at first boot of each node, printed on serial
```

Nodes hold `grid_root`, which lets any node verify any device without a key table. A client holds only its own `K_device`, so a stolen client exposes one key that revocation neutralizes.

Randomness is drawn only after `esp_wifi_start`. Espressif documents that `esp_random` is only pseudo-random before RF is enabled.

**Provisioning paths:**

| What | How | Needs |
|---|---|---|
| First master | Unprovisioned node exposes `LocalGrid-Setup-XXXX`, WPA2 with the node's 8-digit setup code. Admin runs the setup wizard; this node becomes the master. | Phone, setup code from node serial output or label |
| Other nodes | Admin clicks "Add node" and enters that node's setup code. Master and node run SRP6a over ESP-NOW using the setup code as password, then the master sends the node bundle through the resulting AES-GCM channel. | Setup code |
| Clients, field path | Admin clicks "Add device", master shows a one-time 8-digit code valid 5 min. On the client: "Join LocalGrid", pick the grid advertising "pairing open", type the code. SRP6a over ESP-NOW, then the client bundle. | Code typed on the touchscreen |
| Clients, bench path | Serial command `grid provision` with a bundle generated from the master CLI | USB cable |
| Revocation | Admin revokes a device; the next config generation lists it; nodes refuse its handshake and drop its session | Master reachable |

SRP6a with AES-GCM is Espressif's own "Security 2" provisioning scheme, the vetted choice for pairing with a short code. Carrying it over ESP-NOW means an unprovisioned client needs neither the Wi-Fi passphrase nor a laptop. The SRP public values are 384 bytes, which fits an ESP-NOW v2 frame.

**Client bundle contents:** grid ID, grid name, device index, `K_device`, key epoch, Wi-Fi passphrase, grid channel, discriminator, node BSSID list, protocol version, and an initial config snapshot.

**Storage:** plain NVS in Phase 1. On classic ESP32, NVS encryption requires flash encryption, which permanently limits reflashing. On ESP32-S3, NVS encryption can use the HMAC eFuse key without flash encryption. That is a post-MVP hardening step for S3 clients, tested first with virtual eFuses.

**Rotation:** backbone epoch rotation is a config push. Rotating `grid_root` invalidates every device key and requires re-pairing every device. That is the documented response to a stolen node in Phase 1.

---

## Part E. Master, administration, time, configuration

### 28. Master node architecture

The master runs the same infrastructure firmware as every node, with `role = MASTER` in NVS. Everything a node does, the master also does, including serving clients.

Master-only services:

| Service | Detail |
|---|---|
| Setup mode | Unprovisioned: setup SoftAP, DNS catch-all, setup wizard |
| Admin web | `esp_http_server` on port 80, answer 29 |
| Captive DNS | Answers A queries for any name with 192.168.4.1 on the master's SoftAP |
| mDNS | `localgrid.local` advertised as a convenience |
| Registry authority | Allocates device and node indexes; stores names, capabilities, revocation |
| Config authority | Only the master increments the config generation |
| Time authority | Source of `AUTHORITATIVE` time |
| Pairing | SRP6a responder for nodes and clients |

**Deterministic addressing:** the master is always node index 0, SoftAP `192.168.4.1`. Other nodes are `192.168.(4 + index).1`.

**Which Elegoo board:** any. The one you configure first becomes the master. Suggest COM16 for bench consistency.

### 29. Master HTML administration architecture

- **Assets:** vanilla HTML, CSS, and JavaScript, no framework, no remote fonts or scripts. Built and gzipped at compile time, embedded with `EMBED_FILES`, served with `Content-Encoding: gzip`. Budget 40 KB gzipped; WLED's much larger UI is 59 KB gzipped for comparison.
- **API:** JSON over HTTP under `/api/`. JSON is fine here: browser-native, admin-only, low rate. The dashboard refreshes every 5 s while open, which is admin-page polling, not device protocol.
- **Server settings:** `max_open_sockets` 7, `lru_purge_enable` on so captive-probe storms cannot pin sockets, `max_uri_handlers` 32, stack 6 KB.

**Endpoints (initial):**

| Section | Endpoints |
|---|---|
| Setup | `GET /setup`, `POST /api/setup` |
| Session | `POST /api/login`, `POST /api/logout` |
| Dashboard | `GET /api/status` |
| Infrastructure | `GET /api/nodes`, `POST /api/nodes/{i}/rename`, `POST /api/nodes/adopt` |
| Devices | `GET /api/devices`, `POST /api/devices/{i}/rename`, `/revoke`, `POST /api/pairing/open` |
| Groups | `GET/POST /api/groups`, `POST /api/groups/{id}/members` |
| Time | `GET /api/time`, `POST /api/time` |
| Security | `POST /api/admin/password`, `POST /api/security/rotate-backbone` |
| Diagnostics | `GET /api/diag`, `GET /api/logs?since=` |
| System | `POST /api/system/reboot`, `GET /api/system/firmware` |

**Session security:**

- Password stored as `salt (16 B) || iterations || PBKDF2 hash (32 B)`. Iterations calibrated at setup to about 0.5 s, floor 4,000. Expect roughly 6,000–8,000 on classic ESP32.
- Session token 128-bit random in a cookie with `HttpOnly; SameSite=Strict; Path=/`. Idle expiry 30 min, absolute 8 h, at most 2 concurrent sessions.
- CSRF: per-session token sent in an `X-CSRF` header on every non-GET request, plus an Origin and Host equality check.
- No secrets in URLs. Keys, hashes, and the grid root are never returned by any endpoint.

**HTTP, not HTTPS, and what that means:** HTTPS costs about 40 KB per TLS socket and roughly 2 s per handshake on a no-PSRAM master, and a self-signed certificate triggers interstitials on every browser. Plain HTTP runs inside the WPA2-protected master SoftAP. **Residual risk:** anyone who knows the Wi-Fi passphrase and is on the master's SoftAP can sniff an admin session cookie. `Secure` cookies and Web Crypto are unavailable on plain HTTP. HTTPS becomes realistic if a future master is an ESP32-S3 with PSRAM.

**Phone realities:**

- Android always labels the network "No internet" because its HTTPS probe cannot succeed offline, and may route browser traffic over mobile data. The setup landing page says: turn on airplane mode, then enable Wi-Fi, or accept "use this network without internet".
- iOS shows the captive assistant sheet. It is used only as a landing page with the literal address, because it discards cookies on close.
- `localgrid.local` resolves on iOS, macOS, Windows 10+, and Android 12+, but not Android 11 and older. `http://192.168.4.1/` is the canonical address, printed on the master label.
- DHCP option 114 is **disabled**. ESP-IDF's example advertises a plain-HTTP IP URI, which RFC 8908 and RFC 8910 forbid and which modern iOS and Android fetch as an API.

### 30. Browser-based offline time configuration

The setup and Time pages run:

```js
const payload = {
  utc_ms: Date.now(),
  tz: Intl.DateTimeFormat().resolvedOptions().timeZone,   // e.g. "America/Chicago"
  offset_min: new Date().getTimezoneOffset()
};
```

On the master:

1. Look up the IANA zone in an embedded table derived from the `posix_tz_db` project: 461 zones, about 15 KB raw. Store the POSIX string, for example `CST6CDT,M3.2.0,M11.1.0`.
2. If the zone is unknown, build a fixed-offset string from `offset_min` and warn that DST will not apply.
3. `settimeofday` with the UTC value, `setenv("TZ", posix)`, `tzset`. Newlib in both current ESP-IDF toolchains applies DST rules without a timezone database.
4. Mark time quality `AUTHORITATIVE`, persist UTC to NVS, bump config generation because the timezone is configuration.

Manual entry uses the same endpoint with values typed into date and time fields.

Accuracy is bounded by request latency, tens to hundreds of milliseconds, which is fine for a clock display.

### 31. Configuration propagation from master to nodes

**Configuration object** (nanopb, ≤ 4 KB encoded):

```
version (schema), generation (u32), grid_id, grid_name, timezone_posix,
channel, wifi_passphrase_ref, node_registry[≤8], device_registry[≤32 names+caps],
revoked_devices[], groups[≤8 × ≤16 members], messaging_policy, roaming_policy,
retention_policy, backbone_key_epoch
```

The Wi-Fi passphrase and keys never travel in config; nodes already hold them from adoption.

**Distribution:**

1. Admin change → master validates → writes new config to the inactive NVS slot → verifies read-back → flips the active slot pointer → increments generation. A power cut at any point leaves the previous valid config active.
2. Master includes `(generation, hash)` in every HELLO and sends `CONFIG_ANNOUNCE` immediately.
3. A node seeing a higher generation from any neighbor sends `CONFIG_REQUEST`. The neighbor answers with `CONFIG_CHUNK` frames of ≤ 1200 B. Any node can serve config, so a node out of range of the master still receives it through a neighbor.
4. The node reassembles, verifies an HMAC under `K_config`, verifies schema version, writes its own A/B slot, applies, and floods `CONFIG_ACK(generation)`.
5. The admin page shows each node's applied generation.

**Rules:** only the master increments generation. Nodes never create config. A node rejects any config whose grid ID differs or whose generation is lower than its own. Partial transfers time out after 10 s and restart.

**Honest limit:** HMAC under a key every node holds means a compromised node could forge config. Ed25519-signed config, with the signing key only on the master, is the post-MVP fix.

**Prototype, groups (D52):** groups do not wait for this config object. The group table travels on its own as `GROUPS` (0x52, 9 + 22 bytes per group, at most 185), versioned by (seq, author AP) with the newest winning, flooded on link up and every 30 s, and saved in NVS on every AP. Any AP may make a new version, from the admin page or from a handheld's `GROUP_EDIT` (0x53, 23 bytes: op, id, members, name).

### 32. How clients obtain configuration

Clients never contact the master.

- `REGISTER` carries the client's cached generation. If the node's generation differs, the node sends a **client view** of the config: grid name, timezone, the client's own name, the device directory (names and indexes), groups the client belongs to with member names, and roaming and retention policy. Registry internals and revocation lists stay on nodes.
- When a node applies a new generation, it pushes the client view to every attached client.
- Clients store the last client view in NVS and use it while offline, for example to show names in history.
- **Prototype, groups (D52):** a node sends `GROUPS` after `REGISTER_ACK` and whenever its table changes; a handheld keeps it only if newer, saves it in NVS, and deletes the messages of any group the new table no longer has.

---

## Part F. Failure behavior

### 33. Non-master node fails

Example: TENT-NORTH loses power.

| Time after loss | What happens |
|---|---|
| 3–5.5 s | Its clients hit beacon timeout, show RECONNECTING, scan, and connect to the best remaining node if in range |
| 6 s | Neighbors mark it lost after three missed HELLOs |
| ~6–10 s | Roamed clients re-register; their presence reappears with a higher attach epoch; outboxes re-offer |
| 30 s | Clients that found no other node are marked unreachable, then RECENTLY SEEN |
| Ongoing | If it was the only link between two halves of a chain, the grid partitions (answer 35) |

On restore, the node boots, HELLOs, gets two-way confirmation within about 4 s, pulls newer config if any, and resumes serving. Clients do not move back unless the roaming rules say so.

### 34. Master fails

**Continues:** backbone among remaining nodes, sessions on other nodes, direct, group, and broadcast messaging, store-and-forward, cached config, groups, revocation list, and each node's running clock.

**Stops:** admin web, new pairing, config changes, time corrections, and client service in the master's own coverage area. Clients attached to the master roam if another node covers them; otherwise they wait.

**Time without the master:** every node keeps its own offset and advertises quality `CARRIED`. Nodes do not adjust each other, so there is no authority fight. Crystal drift is at most about 1 s per day.

**Master restore:** the master boots, recovers time from a neighbor's `CARRIED` time if its own is unset, keeps its config generation from NVS, and resumes. If time was recovered this way, it stays `CARRIED` until an admin confirms or re-sets it.

**Clarifying the brief:** "existing messaging continues where topology permits" is exactly right, and the gap is spatial. The area only the master covers goes dark.

### 35. Infrastructure partitions

Chain A–B–C with B lost:

- **After 6 s:** A and C each lose B as a neighbor. If A and C can hear each other directly, which ESP-NOW at 1 Mbps makes more likely than for a client link, nothing partitions.
- **If they cannot:** each side keeps full local service. Devices on the far side become unreachable after 10 s without flooded `NODE_STATUS` from their node, then RECENTLY SEEN in the UI.
- **Messages to the far side** queue at the origin node under the normal limits, and senders see `queued`.
- **Config:** each side keeps its current generation. Only the side containing the master can create a new one.
- **Time:** each side runs on its own clocks.

### 36. Partition merges

1. HELLOs cross again. Two-way confirmation completes within about 4 s.
2. Nodes exchange `PRESENCE_DIGEST`. Mismatched hashes trigger full table transfers. Conflicting device entries resolve by the higher attach epoch.
3. Store-and-forward queues flush to newly reachable recipients at 2 messages per second.
4. Message deduplication absorbs copies that took both paths during the transition.
5. Config: the higher generation wins. The master is the only author, so generations never conflict.
6. Time: the side with the higher quality (`AUTHORITATIVE` > `CARRIED` > `UNSET`) wins. Nodes correct their offsets gradually if the difference is under 2 s and step if larger. Steps are logged.

---

## Part G. Software platform

### 37. Hardware abstraction strategy

**Principle:** ESP-IDF's `esp_lcd` panel API and `esp_lcd_touch` API already are the display and touch abstraction, and LVGL sits on top. LocalGrid does not wrap them again. It adds only what they lack: a board descriptor and a few board functions.

```c
typedef struct {
    const char *board_id;              // "cyd-2432s028r", "freenove-fnk0104b", "elegoo-devkit"
    uint16_t    display_w, display_h;  // 0 if headless
    uint32_t    caps;                  // LG_CAP_DISPLAY | LG_CAP_TOUCH_CAP | LG_CAP_MIC | ...
    uint32_t    psram_bytes, flash_bytes;
} lg_board_desc_t;

typedef struct {
    const lg_board_desc_t *desc;
    esp_err_t (*init_display)(esp_lcd_panel_handle_t *out);    // NULL if headless
    esp_err_t (*init_touch)(esp_lcd_touch_handle_t *out);      // NULL if no touch
    esp_err_t (*init_audio_out)(lg_audio_out_t *out);          // NULL if none
    esp_err_t (*init_audio_in)(lg_audio_in_t *out);            // NULL if none
    int       (*battery_mv)(void);                             // -1 if unknown
    void      (*set_status_led)(lg_led_state_t s);             // no-op if none
} lg_board_t;
```

- **Selection at build time** through a Kconfig choice, "LocalGrid board", with a `sdkconfig.defaults.<board>` file per board. One binary per board keeps unused drivers out of 4 MB flash.
- **Verification at boot:** each board file probes something cheap, for example FT6336U at I2C 0x38 on the FNK0104B, or panel ID bytes on the CYD. A mismatch logs a clear error and continues in a degraded "unknown board" mode.
- **Runtime variants within a board:** the ESP32-2432S028R ships with ILI9341 or ST7789 panels under the same silkscreen. The board file reads the panel ID and selects the driver and color inversion, so one binary covers both.
- **Application code** reads `caps` and never includes a pin or controller name.

**Resolution-independent UI (owner direction, 2026-09-15).** Pixel numbers live only in the hardware profile. Everything else fits itself to the screen it finds.

- **Where the size comes from:** SPI panels such as the ILI9341 and ST7789P3 cannot report their resolution, so "discovery" means the UI asks at runtime rather than assuming. The board profile declares the native panel size, the rotation that gives portrait, and the physical size or pixel density. The UI reads the resulting width and height from the LVGL display (`lv_display_get_horizontal_resolution` and `lv_display_get_vertical_resolution`) after rotation is applied.
- **Layout:** every screen uses LVGL flex and grid layouts with sizes in percent of the parent or content-sized. The keyboard is `LV_PCT(50)` of the screen height; message lists and headers grow or shrink to fill the rest. No `lv_obj_set_pos` or `lv_obj_set_size` call takes a literal pixel value outside the board profile.
- **Scaling rules derived at runtime from the profile:** a size class (small, medium, large) chosen from the shorter screen side, which picks the font set; minimum touch-target size in millimetres converted to pixels using the profile's density; and spacing as a fraction of the shorter side.
- **Orientation:** the profile maps the panel's native orientation to portrait, so a panel that is natively landscape still presents a portrait UI.
- **Enforcement:** a CI check rejects numeric pixel arguments to LVGL position and size calls in `firmware/client/ui/`, and the UI is exercised at two resolutions (the FNK0104B and the Hosyond 3.2") before any UI milestone is accepted.

**Themes (owner direction, 2026-09-15).** Phase 1 ships one theme, the terminal look: black background and green text. More themes come later, so theming is designed in from the start.

- **One theme table** holds every visual choice: colors by role (background, surface, text, muted text, accent, warning, danger), font set per size class, spacing and radius scale factors, and keyboard key styling.
- **Screens use roles, not values.** Screen code asks the theme for "accent color" or "body font", built once into shared LVGL styles. Color literals appear only in theme tables.
- **Switching** a theme rebuilds the shared styles and invalidates the screen; no screen code changes. Theme choice is a per-handheld preference stored in NVS.
- **Constraint:** themes must stay readable on the classic CYD's panel and fit its flash, so a theme may only reference fonts already in the build.

Initial board files:

| Board | Display | Touch | Audio out | Mic | SD | Battery | PSRAM |
|---|---|---|---|---|---|---|---|
| elegoo-devkit | none | none | none | no | no | no | 0 |
| hosyond-3.2 (bench COM11) | 240×320 ST7789P3, SPI | resistive (controller and pins to be confirmed at bring-up) | to be confirmed | no | to be confirmed | no | 0 |
| cyd-2432s028r (other 2.8" CYDs) | 320×240 ILI9341 or ST7789, SPI | XPT2046 resistive | DAC + 1 W amp | no | yes, optional | no | 0 |
| freenove-fnk0104b | 320×240 ILI9341, SPI | FT6336U capacitive | ES8311 I2S | yes | yes | ADC | 8 MB |

CYD specifics the board file handles: display, touch, and SD are on three separate pin sets on a two-bus chip, so touch is bit-banged if SD is used; touch interrupt pins GPIO36 and GPIO39 glitch when the ADC powers up (ESP32 errata), so touch is polled with debounce. FNK0104 specifics: GPIO33 to GPIO37 belong to octal PSRAM and are never assigned.

### 38. Framework recommendation

**Pure ESP-IDF v6.1 for both firmware products. No Arduino. No PlatformIO.**

| Option | Verdict | Deciding facts |
|---|---|---|
| **ESP-IDF v6.1** | **Recommended** | Current in-service line (service to about 2027-08, end of life about 2029-02). Direct control of Wi-Fi events, ESP-NOW, NVS, lwIP Kconfig, esp_console, esp_http_server. ESP-NOW v2, captive-portal support, and scan fixes are all present. |
| ESP-IDF v5.5.5 | Fallback | Most-exercised line, maintenance until about 2028-01. Legacy mbedTLS 3.6 API. Forces a v6 migration mid-project. |
| Arduino-ESP32 3.3.11 | Rejected | Built on IDF 5.5.5 with a frozen sdkconfig: Wi-Fi buffers, lwIP sockets, and FreeRTOS tick cannot be tuned without rebuilding the core. Adds about 180 KB. Its Wi-Fi class owns event handling. Nothing LocalGrid needs comes from Arduino. |
| PlatformIO official platform | Rejected | Still ships Arduino 2.0.17 on IDF 4.4. Its ESP-IDF mode is a second build system that lags IDF releases. The installed `espressif32 6.5.0` predates ESP-NOW v2 and the scan-while-connected fixes roaming depends on. |
| pioarduino fork | Rejected | Volunteer-maintained wrapper, same second-build-system risk. |
| Different frameworks per product | Rejected | Nothing in the client needs a different framework; LVGL and esp_lcd are native IDF components. |

**Strongest counterargument:** v6.1 is six months old in its major line. It builds with warnings as errors, uses GCC 15, and moved to mbedTLS 4 with PSA as the only crypto API. Third-party components may break: the XPT2046 touch driver's last commit was in 2025, and whether HKDF and PBKDF2 are enabled in IDF 6.1's PSA configuration was not verifiable from source. **Milestone 1A gate:** build a throwaway app on v6.1 containing LVGL 9.5, `esp_lvgl_port`, `esp_lcd_ili9341`, the XPT2046 and FT5x06 touch drivers, `espressif/mdns`, nanopb, the chosen Noise library, and PSA calls for ChaCha20-Poly1305, HKDF, PBKDF2, and X25519, for both targets. If any blocker cannot be fixed in a day, pin v5.5.5 and record the decision.

**Windows setup:** install ESP-IDF v6.1 with Espressif's ESP-IDF Installation Manager (EIM), targets esp32 and esp32s3 only, into a short path without spaces. Add the ESP-IDF VS Code extension on top if wanted. Host unit tests of the pure-C core run natively with CMake; ESP-IDF's `linux` target tests run in WSL2 or the `espressif/idf` Docker image, because that target does not support Windows hosts. Budget 4–6 GB disk.

**CI:** GitHub Actions with `espressif/esp-idf-ci-action`, pinned to v6.1, building node and client for esp32 and client for esp32s3, plus a host job for unit tests and the simulator. Each build records `idf.py size-components --format json2` and fails if an app exceeds 85% of its partition.

**Concurrency model** (brief sections 58–59):

| Product | Task | Priority | Stack | Blocks on | Watchdog |
|---|---|---|---|---|---|
| Node | `lg_core`: Wi-Fi events, ESP-NOW RX queue, TCP `select`, protocol, timers | 5 | 6144 B | one queue + `select` with 100 ms timeout | subscribed, 5 s |
| Node | `httpd` (master only, IDF-owned) | 5 | 6144 B | sockets | IDF |
| Node | `dns` (master setup and runtime) | 4 | 3072 B | UDP recv | subscribed |
| Node | `console` (IDF REPL) | 2 | 4096 B | UART | none |
| Client | `lg_net`: Wi-Fi, scan, roaming, TCP, protocol, NVS writes | 5 | 6144 B | queue + `select` | subscribed, 5 s |
| Client | `lvgl` (esp_lvgl_port) | 4 | 7168 B | LVGL timer | subscribed |
| Client | `console` | 2 | 4096 B | UART or USB-JTAG | none |

- **Message passing:** the UI and network tasks exchange fixed-size structs over two FreeRTOS queues, `ui_cmd_q` (8 entries) and `ui_evt_q` (16). Message text travels by slot index into a preallocated pool, not by pointer to heap.
- **Wi-Fi and ESP-NOW callbacks** only copy into a queue and return, because they run in the high-priority Wi-Fi task.
- **No mutexes in the protocol core.** It is single-threaded by construction. The only shared structure is the read-mostly presence snapshot the UI copies under a short critical section.
- **Internal events:** `NETWORK_CONNECTED`, `NETWORK_DISCONNECTED`, `ROAM_STARTED`, `ROAM_COMPLETED`, `MESSAGE_RECEIVED`, `MESSAGE_STATE_CHANGED`, `PRESENCE_CHANGED`, `TIME_UPDATED`, `CONFIG_CHANGED`.

### 39. Repository organization

ESP-IDF convention is one project directory per firmware with shared components pulled in through `EXTRA_COMPONENT_DIRS`. The pure-C core must compile without ESP-IDF, so it gets its own CMake entry point for host builds.

```
LocalGrid/
├── firmware/
│   ├── node/                 ESP-IDF project: infrastructure + master
│   │   ├── main/
│   │   ├── partitions/       classic-4mb.csv
│   │   └── sdkconfig.defaults(.esp32)
│   └── client/               ESP-IDF project: handheld
│       ├── main/  ui/
│       ├── partitions/       classic-4mb.csv, s3-16mb.csv
│       └── sdkconfig.defaults.{cyd-2432s028r,freenove-fnk0104b}
├── components/
│   ├── lg_proto/             envelope, nanopb bodies, IDs, dedup windows   (pure C)
│   ├── lg_core/              routing, presence, queues, S&F, config model  (pure C)
│   ├── lg_crypto/            AEAD, HKDF, PBKDF2, Noise adapter             (PSA backend + host backend)
│   ├── lg_port_idf/          NVS, time, esp_wifi, esp_now, sockets glue
│   ├── lg_board/             board descriptors and board files
│   ├── lg_diag/              logging, counters, heap and stack reports
│   ├── lg_console/           serial CLI commands
│   └── lg_admin_web/         HTTP handlers + embedded gzip assets
├── web/admin/                admin UI source (vanilla), build script → gzip
├── proto/                    .proto and .options files
├── host/
│   ├── CMakeLists.txt        builds lg_proto + lg_core + lg_crypto(host) natively
│   ├── tests/                Unity unit tests
│   └── sim/                  N-node / M-client simulator with loss, delay, duplication, partitions
├── tools/
│   ├── provision.py          bench provisioning over serial
│   ├── logdecode.py          decodes binary diagnostic dumps
│   └── tz/                   IANA → POSIX table generator
└── docs/
    ├── DESIGN_REVIEW.md  ARCHITECTURE.md  PROTOCOL.md  SECURITY.md  NETWORKING.md
    ├── BOARD_PORTING.md  BUILD.md  TESTING.md  RECOVERY.md  ADMIN_WEB.md
    └── adr/                  0001-esp-idf-v6.1.md, 0002-espnow-backbone.md, ...
```

The simulator links the exact core code the firmware uses, with a fake transport. It covers the brief's simulation list: loss, delay, duplication, reordering, disconnect, partition, merge, master loss, and config updates. All testing runs on ESP32 boards (decision D25); load beyond the two bench handhelds needs more ESP32 boards.

### 40. RAM and flash on classic ESP32

Measured anchors from Espressif: 520 KB SRAM; about 299 KB free heap in hello_world on v6.1; 128 KB usable IRAM; Wi-Fi "memory saving" buffer rank leaves 170 KB free during iperf versus 144.5 KB at defaults. **No primary source measures ESP-IDF 5.x or 6.x Wi-Fi heap for SoftAP or station on classic ESP32.** Everything below the anchors is an **estimate** until milestone 1B and 1D measurements replace it.

**Node RAM budget (estimate):**

| Item | KB |
|---|---|
| Free heap after boot | 299 |
| Wi-Fi SoftAP, memory-saving rank, up to 15 stations | −80 |
| lwIP, 24 sockets, capped TCP buffers | −30 |
| ESP-NOW | −5 |
| Core task stack + tables (presence 2, dedup 1.2, config 8, frag 3) | −20 |
| Store-and-forward pool, preallocated | −19 |
| Console | −8 |
| Master only: httpd, DNS, mDNS | −20 |
| **Headroom, non-master / master** | **≈ 137 / ≈ 117** |

**Client RAM budget, CYD (estimate):**

| Item | KB |
|---|---|
| Free heap after boot | 299 |
| Wi-Fi station, memory-saving rank | −60 |
| lwIP | −10 |
| LVGL: two 320×24 DMA draw buffers (30.7), LV_MEM 40, task 7 | −78 |
| Noise session + crypto | −6 |
| Message pool 32 × 300 B, queues, outbox mirror | −15 |
| Net task and console stacks | −14 |
| **Headroom** | **≈ 116** |

Required floors, enforced by diagnostics warnings: minimum free heap ≥ 40 KB and largest internal free block ≥ 16 KB after a 1 h run.

If the CYD measures below the floor: first shrink LVGL (LV_MEM 32 KB, a single draw buffer), then drop to the Wi-Fi "minimum" rank. If still short, replace LVGL on the classic CYD with a small text renderer using a 10 KB strip buffer. Adding BLE (about 90 KB DRAM plus 30–47 KB IRAM) does not fit.

**Flash, 4 MB:**

```
Offset     Size       Name      Notes
0x009000   0x008000   nvs       32 KB
0x011000   0x002000   otadata
0x013000   0x001000   phy_init
0x020000   0x1C0000   ota_0     1.75 MB
0x1E0000   0x1C0000   ota_1     1.75 MB
0x3A0000   0x060000   storage   384 KB  (client: message history; node: logs)
```

Estimated binaries: node 1.0–1.3 MB, CYD client 1.3–1.7 MB. The node fits A/B comfortably. The CYD client may not. **Decision at milestone 1D:** if the client image exceeds 1.5 MB with size optimization, IPv6 disabled (saves about 39 KB), TLS disabled, and newlib nano formatting, the classic client profile switches to a single 3.5 MB app partition with USB-only updates. The brief already permits this.

IRAM is the other ceiling: Wi-Fi IRAM optimizations use about 31 KB of the 128 KB, and a real 25-component ESPHome build overflowed it by 164 bytes. CI tracks IRAM and fails above 110 KB.

### 41. RAM and flash on ESP32-S3 (FNK0104)

- **Internal SRAM:** 512 KB, about 390 KB free at hello_world. **PSRAM:** 8 MB octal, requires `CONFIG_SPIRAM_MODE_OCT` (the default is quad) and 80 MHz, not the experimental 120 MHz.
- **What moves to PSRAM:** message history (500 messages), presence, store of rendered text, larger LVGL memory pool (128 KB), fonts with more glyphs.
- **What stays internal:** task stacks, Wi-Fi and lwIP buffers, and LVGL draw buffers for the SPI ILI9341 unless measured fine with a bounce buffer, because PSRAM copy speed is about 57 MB/s versus 366 MB/s internal.
- **Headroom:** internal RAM is comfortable; BLE fits here if ever wanted.
- **GPIO:** 33–37 are consumed by octal PSRAM.

**Flash, 16 MB with robust A/B:**

```
Offset     Size       Name      Notes
0x009000   0x010000   nvs       64 KB
0x019000   0x002000   otadata
0x01B000   0x001000   phy_init
0x020000   0x400000   ota_0     4 MB
0x420000   0x400000   ota_1     4 MB
0x820000   0x010000   coredump  64 KB
0x830000   0x7D0000   storage   ~7.8 MB (history, logs, future TTS voice data)
```

Rollback via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a new image must reach `REGISTERED` within 60 s of boot before it is marked valid.

---

## Part H. Risks and challenges

### 42. Five biggest networking risks

1. **ESP-NOW on an AP-only classic ESP32 serving clients is unproven.** Open ESP-IDF issues describe permanent `NO_MEM` wedges under bursty sends and ESP-NOW receive breaking after a channel change. *Mitigation:* the one-week prototype tests exactly this under load; single in-flight sender; dead-man timer that restarts Wi-Fi; channel never changes at runtime outside recovery. *Fallback if it fails:* AP+STA chain with fixed master channel and explicit root distance, accepting its costs.
2. **Station capacity per node.** 15 stations per SoftAP is a hard limit. Twenty handhelds around one campfire, with other nodes out of range, leaves five unconnected. *Mitigation:* load-aware selection, free-slot flag in beacons, a clear "node full" state in the client UI, and placement guidance in field documentation.
3. **Fixed channel in a noisy campground.** Other campers' hotspots on the same channel reduce throughput and range, and nothing moves the grid automatically. *Mitigation:* master scans and recommends 1, 6, or 11 at setup; admin-initiated coordinated channel change; node channel-recovery scan. Automatic channel migration is deliberately not in Phase 1.
4. **Roam gap and reconnect cost on real RF.** Break-before-make plus a possible WPA2 key re-derivation could make roams take 1.5 s or more, and pockets and bodies cause RSSI swings that invite ping-pong. *Mitigation:* custody and outbox hide gaps; hysteresis and backoff; measurement gate at 1M.
5. **Coverage is set by the handheld links, and partitions happen.** CYD PCB antennas in pockets are the weakest links; a chain can split when its middle node dies. *Mitigation:* ESP-NOW at 1 Mbps likely outranges client links, making backbone partitions rarer than coverage holes; field test at 1R; external node antennas remain an option without software changes.

Honorable mention: admin phones' "no internet" handling, which is a usability risk rather than a network risk.

### 43. Five biggest security risks

1. **Stolen node compromises the grid.** Classic ESP32 has no NVS encryption without irreversible flash encryption, and every node holds the grid root. *Mitigation now:* documented re-pair procedure. *Later:* per-node backbone keys, Ed25519-signed config, flash encryption as an optional hardening step.
2. **Admin session over plain HTTP.** Anyone with the Wi-Fi passphrase on the master's SoftAP can sniff the session cookie. *Mitigation:* strong random passphrase shared only with family, short session expiry, re-authentication for key rotation and factory reset. *Later:* HTTPS on a PSRAM master.
3. **Nonce reuse through a boot-counter bug.** ChaCha20-Poly1305 fails catastrophically on nonce reuse. *Mitigation:* counter committed before any transmit; NVS loss equals key loss; host unit tests for reboot, power-cut during commit, and restore; a runtime assertion that refuses to transmit if the counter did not advance.
4. **Denial of service.** Deauthentication floods against WPA2 without PMF, forged ESP-NOW frames that cost AEAD verifications, and clients flooding messages. *Mitigation:* PMF enabled and tested for required mode; cheap pre-checks before AEAD (known sender index, sane counters); per-sender rate limits; bounded queues everywhere.
5. **Trust-on-first-use key pinning for 1:1 messages.** A malicious node present when two people first exchange keys could substitute its own. Nodes can also still read group and broadcast text. *Mitigation:* loud key-change warnings on handhelds, a master-signed key directory in Phase 1, and UI wording that says group messages are readable by LocalGrid nodes.

Also noted: forged beacons attracting clients (handled by authentication failure and blacklisting), pairing-code guessing (5 attempts, 5-minute window), and a 6,000–8,000 iteration PBKDF2 hash that is weak against offline cracking if flash is dumped, which is why a 12-character minimum admin password is enforced.

### 44. Five biggest embedded-software risks

1. **Classic CYD memory.** Wi-Fi, lwIP, crypto, and LVGL may not leave 40 KB of stable headroom. *Mitigation:* measurement at 1D; LVGL shrink ladder and text-renderer fallback; no BLE.
2. **ESP-IDF v6.1 component compatibility.** Warnings-as-errors, GCC 15, and PSA-only crypto may break drivers and the Noise library. *Mitigation:* 1A build gate with v5.5.5 fallback.
3. **Long-run heap fragmentation and leaks.** A device that works for an hour can fail after a day. *Mitigation:* no per-message heap allocation; preallocated pools; heap and stack high-water reporting every 60 s; 24 h soak at 1Q with a pass criterion of no downward trend in minimum free heap after the first hour.
4. **Flash and power faults.** Power pulled mid-write, NVS corruption, and CYD brown-outs when backlight and Wi-Fi transmit peak on a weak USB supply. *Mitigation:* A/B config slots; commit ordering; brown-out detector enabled; power-pull test loop at 1O; recommend 2 A supplies for nodes.
5. **Concurrency in callbacks and IRAM limits.** Work done inside Wi-Fi or ESP-NOW callbacks stalls the radio; IRAM overflow appears late as components are added. *Mitigation:* callbacks only enqueue; single-threaded core; task watchdog on core tasks; CI size gate for IRAM.

### 45. Assumptions in the brief that are technically wrong or need changing

Each item states the problem, the reason, and the recommendation. None has been silently applied; each needs your acceptance.

1. **"Nodes connect AP-to-AP using AP+STA."** Single-radio channel coupling, scan outages, and parent cascades make it the fragile option. *Recommend:* SoftAP-only nodes on one channel with an ESP-NOW backbone.
2. **"BLE is the primary discovery method with Wi-Fi scanning as fallback."** BLE degrades SoftAP and ESP-NOW receive on nodes and costs about 90 KB DRAM on a classic CYD, while a one-channel Wi-Fi scan already finds every node in about 100 ms. *Original recommendation:* BLE off in Phase 1. **Owner decision:** BLE stays. Nodes advertise, clients scan, both radios feed one candidate list, and the prototype measures the costs listed in answer 12.
3. **Parent and upstream selection, loop prevention via root distance.** With the recommended backbone there is no tree. Loops are prevented by message deduplication and TTL. *Recommend:* accept a neighbor graph.
4. **"Roaming should be invisible."** ESP32 SoftAPs support none of 802.11k/v/r and ESP-IDF roaming is break-before-make. A gap of roughly 0.5–1.5 s is physics plus driver design. *Recommend:* accept a short RECONNECTING state; messaging hides it with custody and outbox.
5. **Time after acceptance steps 45–50.** No board on the bench has a battery-backed clock. After everything powers off, no device knows the time. *Options:* (a) accept "time not set" until an admin opens the Time page, with message ordering still correct; (b) add a DS3231 module, about US$3–10 and accurate to about ±1 minute per year, to the master's I2C pins; (c) allow a battery-powered handheld to offer a clearly-labeled time hint when the grid has none. *Recommend:* (a) for Phase 1, (b) as a cheap upgrade. (c) conflicts with the brief's rule that clients are never time sources, so it is your call. **Owner decision, 2026-09-15:** after a full power-off the time must be entered again, and a device whose time does not match its connected node is told it will not work until the time is corrected. *Concern raised:* a hard block also stops an URGENT broadcast such as "Storm coming" while nobody has re-entered the time. **Scope confirmed:** the admin re-enters the time on the master web page, and handhelds sync from their node automatically. A handheld whose clock differs from its node by more than 2 minutes shows a warning and can only receive messages and send URGENT broadcasts until the time is fixed. While the grid time is unset, every handheld is in that restricted state.
6. **"GROUP messages only decryptable by group members" and DIRECT privacy between endpoints.** *Original recommendation:* hop-by-hop only in Phase 1. **Owner decision:** 1:1 messages are end-to-end encrypted from the prototype onward (answer 26). Group and broadcast remain node-readable until group keys are added.
7. **HTTPS-grade admin security.** Not achievable on a 4 MB no-PSRAM master. *Recommend:* HTTP inside WPA2 with the documented residual risk.
8. **`http://localgrid.local/` as the predictable address.** Android 11 and older cannot resolve it, and Android flags the network as having no internet. *Recommend:* `http://192.168.4.1/` as canonical, mDNS as a convenience, setup page instructions for phones.
9. **One SSID implicitly.** *Recommend:* separate SSIDs per node so the admin phone can target the master; clients do not care.
10. **"5–20 CYDs connect to the best node."** At most 15 per node, and the default DHCP pool of 8 silently evicts. *Recommend:* static client addressing, 15-station cap, load-aware selection.
11. **"Bounded authentication attempts" as a security control.** Device keys are 256-bit random; guessing is not possible. Throttling protects CPU. The admin password and pairing codes are the guessable secrets. *Recommend:* focus throttling there.
12. **Store-and-forward stored on infrastructure.** RAM queues on nodes are lost on node reboot, and flash-backed queues wear flash. *Recommend:* node RAM queue plus sender-side persistent outbox.
13. **PTT on classic CYDs.** They have no microphone. *Recommend:* classic CYDs are receive-only for voice; FNK0104 boards talk.
14. **A/B partitions on 4 MB boards.** Feasible for nodes; possibly not for the client with LVGL. *Recommend:* decide after measuring at 1D.
15. **Board part number "FNK0106".** That is Freenove's NVMe SSD. The owner's listing confirms FNK0104. *Recommend:* treat it as the touch variant and confirm touch with an I2C probe at boot.
16. **Phase ordering.** The brief adds the second node at 1H, after direct messaging on one node. The backbone is the highest technical risk. *Recommend:* build two-node backbone at 1C, as sequenced in answer 47.

### 46. Smallest useful prototype in about one week

**Revised 2026-09-15 at the owner's direction:** the prototype delivers **1:1, group, and broadcast messaging as early as possible**. The three architecture risks (ESP-NOW under client load, reconnect timing, classic CYD memory) are measured along the way on the same code instead of in a separate risk-only week.

**Scope:** direct, group, and broadcast text across two or three nodes, with delivery acknowledgements and a touch UI. **1:1 messages are end-to-end encrypted** as described in answer 26, and backbone frames are sealed with a prototype key. **Not in the prototype:** pairing, admin web, store-and-forward, roaming hysteresis. The channel, node indexes, device indexes, names, and groups are hardcoded in a shared header. Prototype keys live in a gitignored header so they never reach the repository.

**Handheld UI rules (owner direction):** portrait orientation only. The on-screen keyboard may take the lower half of the screen and has letter, number, and symbol pages plus an emoji page with the most common emoji. **No screen coordinates or sizes are hard-coded anywhere except the hardware profile**; the UI reads the resolution at runtime and lays itself out to fit (answer 37). The emoji are drawn from a small embedded image set, because the fonts that fit in 4 MB flash have no emoji glyphs.

**Testing without a PC compiler:** WSL on this machine has no C compiler, and installing one needs a sudo password. The protocol core's unit tests and multi-node simulation therefore run as an ESP-IDF test app on one Elegoo board, reporting over serial.

**Each handheld is exactly one user.** There are only two handhelds, and "non-members do not receive a group message" needs at least three users. Nothing is tested from a laptop (decision D25). On hardware, the two handhelds cover group membership in two passes: a group containing both, then a group containing only one. The four-user cases run in the on-board simulator (`tests/target`).

Prototype roster (hardcoded):

| User | Device | Groups |
|---|---|---|
| Dad | Hosyond 3.2" CYD (COM11) | FAMILY, LEADERS |
| Emma | Freenove FNK0104B (COM9) | FAMILY, KIDS |
| Alex | simulated in `tests/target` only | FAMILY, KIDS |
| Ranger | simulated in `tests/target` only | LEADERS |

| Day | Work | Exit criterion |
|---|---|---|
| 1 | Back up the current flash of all five boards. Install ESP-IDF v6.1. Build and flash hello_world everywhere; confirm the CYD panel ID; record free heap. Write the pure-C protocol core (envelope, message IDs, dedup, group and broadcast fan-out) with host tests in WSL. | Five boards flash from the command line; host tests pass |
| 2 | Node firmware: SoftAP on channel 6, TCP server for clients, ESP-NOW backbone with HELLO, flood and dedup, serial CLI. | Two nodes see each other within 4 s in either power-on order |
| 3 | Client connection on the Hosyond and FNK0104B: scan, static IP, TCP, REGISTER, presence list across nodes. **Direct messages** with `sent` and `delivered` states. | Acceptance steps 19–23: "Where are you?" and "At the fire." across two nodes |
| 4 | **Group and broadcast**: hardcoded groups, membership enforcement at every delivering node, delivered counts, broadcast rate limit. | Acceptance steps 24–29: FAMILY members get "Dinner at 7." and Ranger does not; everyone gets "Storm coming. Return to camp." |
| 5 | Touch UI on both handhelds: status, people, groups, everyone, conversation, on-screen keyboard (LVGL). | All three message modes usable from the screens without the serial console |
| 6 | Third node in a chain (reduced TX power to force A–B–C). Failure drills: power off a node with clients attached. Load: needs extra ESP32 boards (D25). | Messages cross two hops exactly once; reconnect time measured; no crash with 15 stations |
| 7 | Measurements and write-up: CYD minimum free heap, reconnect time, ESP-NOW behavior under load. Update this review's estimates. | Backbone and classic-client UI stack confirmed or revised with data |

**Pass / fail meaning:** if ESP-NOW on AP-only nodes misbehaves under client load, the messaging code above does not change. Only the node-to-node transport underneath it is swapped.

### 47. Phase 1 implementation sequence

Each milestone ends with: code compiles for all targets, exact build and flash commands, expected serial logs, a manual test procedure, known limitations, automated tests run, and documentation updated. A broken milestone blocks the next.

| Milestone | Scope | Key gate or measurement |
|---|---|---|
| **1A Scaffold** | Repository layout, CI, host build, Unity tests for envelope codec, message IDs, dedup windows, bounded queues. Board probe app. Toolchain compatibility gate app. | v6.1 vs v5.5.5 decided; FNK0104 variant confirmed |
| **1B Single node** | SoftAP fixed channel, TCP server with framing, serial CLI (`grid status`, `grid network`, `grid diagnostics`, `grid reboot`), diagnostic logging, heap and stack reports | Measured SoftAP heap with 1 and 10 stations |
| **1C Two-node backbone** | ESP-NOW HELLO, two-way liveness, neighbor table, flood with dedup, single in-flight sender, `NODE_STATUS` | 24 h two-node HELLO soak without wedge |
| **1D Client connect** | Scan, vendor element, selection, static IP, TCP session, status UI on CYD and FNK0104, LVGL budget | CYD minimum free heap ≥ 40 KB; client binary size decides CYD partition profile |
| **1E Identity and secure sessions** | Device UUID and index, boot counter, Noise_NNpsk0 sessions, backbone AEAD, bench provisioning via serial, REGISTER / REGISTER_ACK | Handshake time measured; nonce tests including power cut |
| **1F Presence** | Presence table, updates, digest reconciliation, attach epochs, online list UI | Presence correct across two nodes after roams |
| **1G Direct text** | TEXT_MESSAGE, MESSAGE_ACK states, custody, client outbox, recipient dedup, conversation UI, on-screen keyboard | Acceptance steps 19–23 on two nodes |
| **1H Master setup and admin web** | Setup mode, setup codes, captive DNS, setup wizard, browser time, password hashing, sessions, dashboard, nodes and devices pages | Setup on Android, iOS, and Windows without internet |
| **1I Configuration and groups** | Config object, A/B slots, generation distribution, client view, group management UI, group text with membership enforcement | Acceptance steps 24–27 |
| **1J Broadcast** | Broadcast scope, URGENT flag, rate limits | Acceptance steps 28–29 |
| **1K Pairing and revocation** | SRP6a pairing over ESP-NOW for nodes and clients, revocation list, handshake refusal | Pairing a client with no laptop; revoked client locked out |
| **1L Third node** | Chain and full-mesh layouts, config via neighbor relay, partition and merge | Chain A–B–C messaging; config reaches C through B |
| **1M Roaming** | RSSI filtering, triggers, hysteresis, backoff, blacklist, load awareness | Roam gap measured; no ping-pong over a 30-minute walk loop |
| **1N Store-and-forward** | Origin queues, limits, full-queue rejection, flush pacing, URGENT broadcast queueing | Acceptance steps 33–36 |
| **1O Failure handling** | Master loss, time quality levels, node restore, full power cycle, power-pull loop | Acceptance steps 37–50 |
| **1P Stress** | ESP32 load boards (D25) with many client sessions, message floods, malformed-frame fuzzing on host and over the air, reconnect storms | No crash, bounded memory, correct rejections |
| **1Q Stability** | Several-hour then 24 h soak with continuous traffic across all boards | No downward heap trend after hour one |
| **1R Field test** | Real campsite: trees, tents, bodies, pockets, distances; full 53-step acceptance test with internet absent | Acceptance test passes; coverage map recorded |

Phase 2 (offline text-to-speech) and Phase 3 (push-to-talk) start only after 1R passes.

---

## Decisions needed from you

1. **Backbone:** **accepted by the owner:** SoftAP-only nodes with an ESP-NOW backbone, subject to the prototype's measurements.
2. **BLE:** **decided by the owner: BLE stays in Phase 1.** Nodes advertise and clients scan, as designed in answer 12.
3. **Time after total power loss:** **decided by the owner:** the admin re-enters time on the master. A handheld whose time differs from its node by more than 2 minutes, or while grid time is unset, warns its user and can only receive and send URGENT broadcasts.
4. **Security labeling:** **decided by the owner:** 1:1 messages must be unreadable by others, so they are end-to-end encrypted. Group and broadcast are encrypted in transit and readable by nodes.
5. **Admin transport:** ~~accept plain HTTP inside WPA2 on the classic master.~~ **Accepted by the owner on 2026-09-15.**
6. **Board:** **resolved.** The owner confirmed the FNK0104B with capacitive touch.
7. **Start:** **revised by the owner.** The prototype must deliver 1:1, group, and broadcast messaging as early as possible; answer 46 now reflects that.

## Sources consulted (primary unless noted)

- ESP-IDF v6.1 programming guide: Wi-Fi driver overview, station scenarios, performance and power save, coexistence, ESP-NOW, esp_wifi API, lwIP, memory types, RAM usage, partition tables, NVS, NVS encryption, flash encryption, random numbers, system time, esp_http_server, esp_https_server, host apps, and the v6.0 migration guide.
- ESP-IDF source and Kconfig on GitHub: `esp_wifi`, `lwip`, `mbedtls`, `soc_caps.h` for esp32 and esp32s3, `esp_now.h`, roaming app, captive portal example.
- ESP-IDF issues #18682, #18661, #17874, #11936, #12317, #9064, #3583, #3784, #13111; ESP-Mesh-Lite issues #106, #199, #200, #201.
- RFC 4303, RFC 6347, RFC 5116, RFC 8439, RFC 8908, RFC 8910, RFC 3550, RFC 3551; Noise Protocol Framework specification.
- ESPHome Noise implementation measurements (esphome/esphome PR 19000, esphome-libs/noise-c PR 35).
- Android NetworkMonitor source, Apple captive network and roaming support articles, Microsoft NCSI documentation, Chromium DNS documentation.
- Freenove FNK0104 documentation and product pages; witnessmenow ESP32-Cheap-Yellow-Display documentation; Freenove FNK0106 repository (NVMe SSD).
- Secondary: CycloneCRYPTO benchmarks, Meshtastic firmware source, painlessMesh wiki, community roaming and DHCP timing measurements in arduino-esp32 issues.
