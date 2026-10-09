# Making LoRa first-class

## The honest baseline: where the 4–5x goes

The air link carries ~1.6 kbit/s (~200 B/s) at SF9/BW125. We deliver ~40 B/s one way. The loss, in order of size (all from the 2026-10-08 bench measurements):

| Cost | Share of a part's 2.15 s channel slot | Source |
|---|---|---|
| Receiver quiet gap, `PART_GAP_MIN_MS = 1500` | **70%** | Measured: receivers lose the next packet without ~1.5 s of quiet |
| Part airtime, 120 chars | 30% (0.65 s) | Measured |
| base64 (3 bytes → 4 chars) | 25% of the payload chars carry nothing | Wire format |
| Seal overhead: 12 outer + 16 tag + 32 envelope = 60 B per frame | 70% of a one-part frame is overhead on a 20-char text | Envelope layout |
| Every ack is a full transmission | 1.85 s of channel for 6 bytes of meaning | Measured |
| Hellos/housekeeping | 10.7% steady-state, 64% during discovery | Measured |
| Handheld voice sent twice, unacked | 2x on that hop | Current policy |

Today's two benchmarks: a 20-char text AP→AP costs **~4.0 s of channel** (2.7 s measured until the sender knows); a 4 s voice note arrives assembled **26–37 s after end of talk** — the designs disagree on this baseline, see "Where the designs disagree" below.

The gap is the single biggest lever. Everything below is ordered by both judges' consensus, and nothing gets written before the experiments.

---

## The staged plan

### Stage 1 — Radio, conservative: SF8 + Z85 + wire v2 (module design, dials 1–2)

SF9→SF8 (1.78x air rate), base64→Z85 (+6.7% density), 3-byte short header for single-part frames, magic 0x4C→0x4D, and one radio-profile struct replacing the `PART_GAP_MIN_MS`/`AIR_PART_GAP_MS` duplication across lora.c and hh_lora.c. No change to acks, policy, or scheduling.

- **Text AP→AP:** 3.98 → **3.51 s** channel; sender knows ~0.7 s ideal (vs ~1.2 s)
- **4 s voice note:** 37 → **~32 s** assembled (module's accounting)
- Goodput 39.5 → 48.7 B/s (1.23x); airtime per part halves, so collisions drop ~1.9x
- **Range:** **−2.5 dB** sensitivity (Semtech per-SF step; the RYLR998 datasheet gives no table — medium confidence). ~0.75x range open-field, ~0.83x in terrain. Only fringe links with <2.5 dB margin are affected; revert is one profile constant.
- **Effort:** the whole module design is ~4 days including the bench campaign and fleet reflash.
- **Risks:** SF change makes old/new firmware mutually deaf — one whole-fleet flash (existing rule covers it, magic bump silences stragglers). A Z85 character eaten by module firmware — E3 catches it in 30 min; ~9 spare printable characters exist.

### Stage 2 — Radio, expected: 200-char parts + 1000 ms gap (module dials 3–4), exactly as measured

Each dial is turned only to the value E1/E2 measure, and each reverts alone.

- **Text AP→AP:** **2.51 s** channel
- **4 s voice note:** **~19 s** assembled; voice packets drop 3 parts → 2; `C2_SPACING_MS` can halve 9000 → ~4500
- Goodput 98.6 B/s (2.5x). Note: 2.5x is flat-out goodput; the benchmarks themselves improve 1.59x (text channel) and 1.95x (voice) — the module design's headline overstates its own tables.
- **Range:** no further cost beyond Stage 1.
- **Effort:** included in the ~4 days above; gated entirely on E1/E2.
- **Risks:** the gap may scale with byte count rather than airtime (then dial 4 dies, Stage 1 stands); the ~130-char cliff may be a character count rather than airtime (then dial 3 dies alone). Bigger parts add **~0.4 s to worst-case SOS preemption** (≤1.48 s vs ≤1.06 s at 120-char SF8; today is ≤1.95 s) — an owner decision, below.

### Stage 3 — Application Stages A+B: LGC1 compact seal + bundles, grid tick, queue aging

Envelope 32 → ~16 B on the LoRa path only (bijective, core never sees it, policy/rank computed pre-compression so SOS can't be misclassified); many queued frames share one seal with no added delay; AP housekeeping becomes one bundle per minute, hard-capped at ≤2 parts/AP/min; stale routine records are overwritten in queue or dropped at peek.

These **multiply** with Stages 1–2: the radio enlarges the container, this shrinks the contents. A 1:1 text (2 parts, 5.66 s today) becomes one SF8 part — roughly a 0.3 s part in a ~2.4 s slot (judge recomputation).

- **Text:** group 20-char text −3% alone (the gap dominates one-part frames — that lever is Stage 2's); **1:1 text 5.66 → 3.94 s (−30%)** at today's radio; max one-part text 25 → 41 chars
- **Voice note:** unchanged by A+B directly; B3 bounds a text's wait behind a note to one segment (~6.5 s) instead of the whole note
- k=4 bundle: 8.2 → **~4.2 s** (the design's 3.92 s understates by ~8% per judge recomputation; conclusion holds)
- **Range:** 0 dB by construction.
- **Effort:** A 2–3 days, B 3 days; each ships alone.
- **Risks:** new codec on the crypto path — mitigated by bijection fuzzing in tests/target, a verbatim-fallback tag, and the AEAD tag; a lost tick is no worse than a lost hello today (60 s refresh inside 190 s timeouts).

### Stage 4 — Application Stage C: codec2 700C voice segments

700C is already in the vendored source, compiled out. 87.5 B/s of speech vs 150 (1.7x), shipped as 2.24 s segments of ≤3 parts instead of 1.12 s packets, AP repacks handheld uplink.

- **4 s voice note:** at today's radio, 26 → **11.5 s** assembled per the application design (but see baseline dispute); combined with Stage 2's 155-byte slices, **~8–10 s assembled — roughly 4x** (judge 2's composition). Judge 1 projects the full stack at 4–6x on voice, "well under 10 s".
- **Text:** unchanged.
- **Range:** 0 dB.
- **Effort:** 3–4 days.
- **Risks:** 700C is audibly more synthetic and degrades in wind — owner listen test decides the default, 1200 stays a per-note toggle; encoder CPU and stack on the classic ESP32 are unmeasured (experiment 4); +40–80 KB flash for the VQ tables; multi-part frames amplify loss — the segment cap comes from experiment 5, and the whole-note option stays config-gated.

### Stage 5 — Opportunistic: the protocol design's two detachable pieces

Both judges score the full TDMA/beacon/grab design lowest and say defer it: smallest benchmark gains (its voice "−25%" evaporates under the corrected baseline), highest risk (it rewrites the CSMA area that was this week's recurring bug source; a desynced transmitter is worse than CSMA), and 14 days is optimistic — judge 1 says 3–4 weeks. Its real contribution is boundedness: SOS ≤22.5 s hard bound vs today's unbounded tail, at the price of regressing idle-channel text latency 2.7 → 6–7 s.

Cherry-pick, in order:

1. **XOR parity parts** — one parity part per multi-part frame heals a single lost part with no resend round trip (3-part frame completion 0.73 → 0.95 at p=0.9/part). Deployed firmware provably drops `part >= of` (lg_lora_wire.c:128), so parity is backward compatible and **can ship alone, first, today**. Caveat: the p≈0.9 loss figure is inferred from contended-channel data; the quiet-channel bench measured near-perfect delivery, so parity pays on contended or marginal links — validate p before claiming the resend savings.
2. **Piggybacked bitmap acks / ack bursts** — deletes the standalone ack transmission (~40% of all TX in a two-way conversation); fits the Stage 3 bundle container without any TDMA.

Revisit full TDMA only if the fleet grows past ~3 APs or a chaos run shows the tuned CSMA collapsing — and redo its 2500 ms slot arithmetic with E1's measured SF8 gap first, because it assumes the 1.5 s figure.

### Combined projections

| Benchmark | Today | Stage 1 | +Stage 2 | +Stages 3–4 |
|---|---|---|---|---|
| 20-char text AP→AP, channel | 3.98 s | 3.51 s | 2.51 s | ~2.4 s (group) / one-part always (1:1, 41-char) |
| 12-char 1:1 text, channel | 5.66 s (2 parts) | — | — | ~2.4 s (1 part) |
| 4 s voice note, assembled after talk | 26–37 s (disputed) | ~32 s | ~19 s | **~8–10 s** |
| SOS fully on air, worst case during voice | ≤1.95 s | ≤1.06 s | ≤1.48 s | unchanged by 3–4 |

---

## Bench experiments to run FIRST (cheapest and most informative at the top)

Both judges independently name E1 as the first thing to do: the 1.5 s gap is 70% of every channel slot, and its curve gates three designs at once.

| # | Experiment | Cost | Green-lights | Kills |
|---|---|---|---|---|
| 1 | **E1 — gap-vs-airtime sweep.** Two idle boards, raw AT. {SF9, SF8} × {60, 120, 200 ch}, quiet swept 400→1500 ms in 100 ms steps, 16 pairs/point, ≥15/16 passes. Check `AT+MODE?` (smart power saving would fake the gap) and record `AT+VER?` first. | ~2 h scripted | Gap ≈1000 ms at SF8 → **Stage 2's gap dial**, and sets Stage 4's segment pacing and any future TDMA slot math | Gap stays 1500 or scales with byte count → Stage 2's dial 4 dead; land Stage 1 and keep 1500 everywhere |
| 2 | **E2 — SF8 payload cliff.** `AT+PARAMETER=8,7,1,12` both boards; sweep AT+SEND 120→240 chars in steps of 8, 24 sends each. Part max = one step below the cliff, exactly how 120 was chosen under 130. | ~1 h | Cliff ≥208 chars → **200-char parts (Stage 2 dial 3)** | Cliff still ~130 at SF8 → parts stay 120 chars; dial 3 dead alone |
| 3 | **E3 — Z85 alphabet survival.** Full alphabet both directions, plus a payload containing literal `+RCV=` and the near-misses. Pass = byte-identical, no parser desync. | ~30 min | **Stage 1's Z85** | A character is eaten → substitute from ~9 spares, or stay base64 (lose 6.7%, keep the rest of Stage 1) |
| 4 | **700C on-target proof.** tests/target build with `CODEC2_MODE_700C_EN`; encode 4 s canned PCM on a handheld, print ms/frame and stack high-water, play back on the device speaker, results on screen (D23). Owner listens 700C vs 1200. | ½ day | **Stage 4**, if real-time ×2 holds and the owner accepts the sound | Too slow or judged ugly → Stage 4 dead; Stages 1–3 unaffected |
| 5 | **Multi-part delivery sweep.** 2/3/5/8 consecutive parts at today's exact link discipline, 50 reps, bench RSSI and across-the-house at power LOW. | ½ day | Sets **Stage 4's segment cap** from a measured curve; p≥0.95/part validates 3-part segments | p≈0.85 at range → smaller segments, whole-note option dead; also calibrates the parity payoff (Stage 5) |
| 6 | **LGC1 bijection + on-air A/B.** A day of captured sealed traffic through compress→seal→unseal→decompress on tests/target, byte-exact envelopes and identical policy/rank/ack answers; then 20 reps of a 12-char 1:1 text on air confirming 1 part and ~3.9 s. | 1 day | **Stage 3 ships** | Any non-bijective case or policy divergence → fix or fall back to the verbatim tag before shipping |

(The protocol design's grab-audibility experiment is deferred with the design; run it only if TDMA is revisited.)

---

## Where the designs disagree

- **Today's 4 s voice baseline: 26 s vs 37 s vs 48 s.** The application design says ~26 s (its last-packet accounting is the most careful, per judge 2). The module design says ~37 s (counts the measured 9 s `C2_SPACING_MS`, per judge 1, who calls 26 s inconsistent with the measured spacing). The protocol design's 48 s assumes a double-copy AP→handheld hop, which judge 2 shows is wrong — that hop is acked single-copy. The two judges themselves disagree between 26 and 37. **Measure the real wall-clock baseline on the bench before quoting any multiplier to anyone.** The proposed end states (~19 s at Stage 2, ~8–10 s at Stage 4) are less sensitive to this dispute than the "Nx" headlines are.
- **Module's "2.5x expected"** is goodput, not the benchmarks: 1.59x text channel, 1.95x voice by its own tables.
- **Application's k=4 bundle** figure is ~8% optimistic (3.92 s stated, ~4.23 s recomputed); conclusion unchanged.
- **Protocol's P2 "−25%"** does not survive the corrected baseline — against ~33–37 s it is a wash. Its genuine wins are repairability (≥99.7% complete vs 89% with permanent holes) and a bounded SOS, not speed.

---

## Decisions only the owner can make

1. **Range vs speed (Stage 1 gate).** SF8 costs −2.5 dB: ~0.75–0.83x range at the absolute fringe, nothing on links with margin. TX is already at 22 dBm; nothing buys it back. The revert is one profile constant, and D25 applies — a field walk decides, not the datasheet. Everything past Stage 1's Z85/header wins hangs on a yes.
2. **SOS preempt latency vs throughput (Stage 2).** 200-char parts: worst-case SOS on air ≤1.48 s instead of ≤1.06 s at 120-char SF8 — both better than today's ≤1.95 s — in exchange for 1.8x voice throughput. Record the answer in docs/DECISIONS.md.
3. **Voice quality vs delivery time (Stage 4).** 700C at ~8–10 s delivery sounds noticeably more synthetic and suffers in wind; 1200 at ~2x the time sounds better. The codec byte is per-note, so this is a default, not a lock-in — but the default needs the owner's ears (experiment 4) and a DECISIONS.md entry.
4. **Whole-note frames on proven-quiet links (Stage 4 option).** One 5-part frame gets a 4 s note to ~9.3 s assembled, but one lost part costs a ~9 s resend. Config-gated; experiment 5's curve informs it, the owner picks.
5. **Boundedness vs idle latency (only if TDMA is ever revisited).** The schedule bounds SOS at ≤22.5 s worst case and deletes collisions, at the price of a single idle-channel text going from 2.7 s to 6–7 s mean. Not proposed now; the number is recorded so the future decision is honest.

**Total effort for Stages 1–4:** roughly 12–14 working days across four independently shippable steps, plus the ~2 bench days up front that decide which of them exist.

---

## What the literature says

Literature check against the staged plan, 2026-10-09. Sources favor 2020–2026; numbers are attributed inline. Where the literature is silent or paywalled, it says so.

### Stage 1 — SF8 + Z85 + wire v2

- **Confirms the −2.5 dB figure.** The plan's per-SF-step cost (flagged medium confidence, no RYLR998 datasheet table) matches the independently published **2.5–2.7 dB per LoRa data-rate step** ([dudmuck/lora_codec2](https://github.com/dudmuck/lora_codec2)). Upgrade the confidence note.
- **Confirms airtime-halving-halves-collisions** — the classic 2T vulnerability window (pure ALOHA peaks at 18.4% capacity, slotted at 36.8%), restated as the LoRa baseline in [CurveALOHA, INFOCOM 2022](https://par.nsf.gov/servlets/purl/10379802) and [Beltramelli et al. 2020](https://arxiv.org/abs/2002.10732). LoRa's capture effect keeps real throughput above those curves at high load (Beltramelli, Fig. 7).
- **Adds a field-walk check:** measured carrier-sense (CAD) range is shorter than decode range and shrinks faster at lower SF — SX1261/868 MHz/BW125: SF9 detects 100/96/63/37/20% at 2.5/4.3/5.5/6.2/7.5 km, SF8 only 100/79/47/17/0/0 over the same steps ([SFMAC, IEEE MASS 2025](https://repository.tudelft.nl/file/File_383450cc-467d-4e60-9f69-aacaf930ed71)). Hidden terminals, not load, are what break CSMA-on-LoRa — so the SF8 walk must verify each AP still trips the other two APs' CAD, not just that packets decode.
- Prior art for the 3-byte short header: VLoRa used exactly a 3-byte header (8-bit size + 16-bit seq) for streaming ([ICOIN 2024](https://d2j16w31g89z0j.cloudfront.net/site/icoin2024/abs/C-6-3.pdf)). The literature is silent on Z85-vs-base64 over AT modules; E3 stands alone.

### Stage 2 — 200-char parts + 1000 ms gap

- **The 1.5 s receiver gap exists nowhere in the literature.** LMAC ([MobiCom 2020](https://dl.acm.org/doi/10.1145/3372224.3419200)), CurveALOHA and SFMAC all assume back-to-back reception on SX12xx silicon, and every shipped codec2-voice build drives the radio over SPI at **90–96% duty with no recovery gap** (dudmuck: 56-byte packets, 576 ms airtime, ~96% duty). The gap is almost certainly REYAX AT-firmware behavior, not physics — which strengthens E1's premise that it can shrink, and means its floor is unknowable from papers. The plan's E1-first ordering is right.
- **Corroborates the gap-dominance diagnosis independently:** a 2022 SCHC-over-LoRaWAN study measured channel efficiency halving (**32% → 15% at SF12**) from receiver-side processing time alone ([Sensors 22(4):1531](https://pmc.ncbi.nlm.nih.gov/articles/PMC8874547/)). Receiver dead time, not airtime, dominates LPWAN channel cost.
- **Corrects the cliff expectation:** VLoRa measured **>124-byte payloads unreliable** on different SX1276-class shields ([ICOIN 2024](https://d2j16w31g89z0j.cloudfront.net/site/icoin2024/abs/C-6-3.pdf)), and dudmuck recommends SX126x because it "receives large LoRa packets with fewer errors." The ~130-char cliff may be SX1276 silicon, not SF9 — E2 should expect dial 3 may die alone, as the plan already allows.
- Minor datum for owner decision 2: Beartooth's commercial codec2-over-LoRa kept each channel's transmissions **<500 ms** for the US dwell rule (Mekiker et al., ICCCN 2021, known via the ICOIN paper's summary — itself paywalled). Worth one line when parts grow to ~1.48 s airtime.

### Stage 3 — LGC1 compact seal + bundles + housekeeping caps

- **Confirms the whole approach, including not adopting a standard.** Static-context header elision is standardized ([RFC 8724, April 2020](https://datatracker.ietf.org/doc/html/rfc8724); LoRaWAN profile [RFC 9011, 2021](https://www.rfc-editor.org/rfc/rfc9011.html)) and measured on a real testbed: a 111-byte IPv6/UDP/CoAP packet → **51 B (46%)**, with 48 B of fully predictable headers → a **1-byte rule ID** ([Sensors 20(1):280, Jan 2020](https://pmc.ncbi.nlm.nih.gov/articles/PMC6982818/)). The 32→16 B envelope target is conservative next to that. The rule-ID-in-fPort trick in that paper validates packing LGC1's tag into the existing magic/flags.
- **Confirms skipping libschc:** measured **13.62 kB ROM / 3.60 kB RAM** on a Cortex-M4F inside RIOT ([arXiv:2307.03649, Jul 2023](https://arxiv.org/abs/2307.03649)), and its rule grammars target IPv6/UDP/CoAP fields a custom 32-B envelope doesn't have. A purpose-built bijective codec is smaller than the integration.
- **Adds one guardrail from SCHC's design rationale:** RFC 8724 deliberately has no runtime context sync because resync is the most bandwidth-expensive operation over LPWAN, and rule mismatch silently mis-decompresses. **LGC1 must stay stateless per frame — never delta against a previous frame.** The plan's mitigations (magic 0x4D bump = version agreed before use, verbatim fallback, E6 bijection fuzzing, AEAD tag) are exactly the defenses the literature implies.
- **Confirms skipping generic payload coders:** on a 2026 STM32U575 benchmark, heatshrink needed **58.7 kB RAM and 113 ms** on a kB-scale log ([Frontiers in Computer Science, 2026](https://www.frontiersin.org/journals/computer-science/articles/10.3389/fcomp.2026.1925314/full)); no study shows any dictionary coder paying off on ≤85-byte payloads. The winning pattern (schema-aware packing) is what D49 + LGC1 already do.
- **Confirms bundling, with the plan on the conservative side:** multi-hop LoRa relays aggregating payloads into one frame cut relay energy **up to 61%, up to 7x per reading** ([Sensors 23(11):4994, May 2023](https://www.mdpi.com/1424-8220/23/11/4994)) — and those gains required an aggregation timer; LocalGrid bundles only already-queued frames at zero added delay. The k=4 ~2x sits inside the measured 2–7x range.
- **Confirms the housekeeping hard-cap against the only deployed precedent:** Meshtastic gates background traffic at **25% channel utilization and everything at 40%** ([airtime.h, meshtastic/firmware](https://github.com/meshtastic/firmware/blob/master/src/airtime.h)). No peer-reviewed measurement of Meshtastic utilization exists — this is deployed practice, not a paper. Logging LoRa channel-utilization % against 25/40 would make the ≤2 parts/AP/min cap self-auditing.
- **Adds:** the k=4 bundle is the same erasure shape as Stage 5's frames — one XOR parity part lifts completion **0.656 → 0.919 at p=0.9/part** for 25% more airtime (binomial arithmetic, ours, flagged as such; systematic layout per [LoRaWAN TS004 v1.0.0](https://lora-alliance.org/wp-content/uploads/2020/11/fragmented_data_block_transport_v1.0.0.pdf) means parity-ignorant receivers are unaffected). Wire the bundle container so a parity part can be appended when Stage 5 lands.

### Stage 4 — codec2 700C voice segments

- **Confirms the codec choice:** 700 bps codec2 is what the literature converged on for single-channel LoRa voice — VLoRa chose it as the only codec that fits ([ICOIN 2024](https://d2j16w31g89z0j.cloudfront.net/site/icoin2024/abs/C-6-3.pdf)); FreeDV 700D (2018) and 700E (2020) ship the same 700C vocoder ([codec2 README_freedv](https://github.com/drowe67/codec2/blob/main/README_freedv.md)). The 87.5 B/s figure is exact (28 bits/40 ms frame, confirmed by dudmuck's table). Nobody with bandwidth to spare picked 700C (M17 ships 3200/1600) — which matches "owner listens, 1200 stays a toggle."
- **Confirms the segment envelope:** 2.24 s / ≤3-part segments sit inside both shipped designs — dudmuck's 56-byte/640 ms 700C packets and VLoRa's reliable window (**≥32 bytes for stable latency, ≤124 bytes before loss; <16-byte packets near-zero loss; bit errors ~2/300 packets**). Voice streams are universally unacked (VLoRa, Beartooth, [esp32_loradv](https://github.com/sh123/esp32_loradv)).
- **Adds: experiment 4 is load-bearing and the literature cannot replace it.** The ezDV project chose the **ESP32-S3 specifically for SIMD**, found C3/S2 unsuitable and RP2040 slower than real time, and lists classic-ESP32 testing as future work ([TAPR DCC, Aug 2022](https://files.tapr.org/tech_docs/DCCpapers/2022%2007.pdf)). The only classic-ESP32 datum is forum-reported and unverified: 16.8 ms encode / 15.2 ms decode per 40 ms frame at 1200 bps, 24 kB RAM ([esp32.com](https://esp32.com/viewtopic.php?p=122893)). No published 700C-on-classic-ESP32 figure exists.
- **Adds parity math for the options:** ≤3-part segments + 1 XOR parity ≈ **0.95 completion at p=0.9/part**; the config-gated 5-part whole-note option gets only **0.886** from one parity and needs two RS parities (**0.974**) to be honest about its resend risk (arithmetic ours; see Stage 5).
- **Adds a burstiness requirement on E5:** DaRe's real-world LoRaWAN data (~23,000 frames) measured **frame loss up to 70% at 7.5 km** and found bursty (Gilbert-Elliott) loss in roughly half its datasets ([IoTDI 2017](https://dl.acm.org/doi/10.1145/3054977.3054978), extended [IEEE TMC 2020](https://ieeexplore.ieee.org/document/9167471/)). Double losses inside one frame defeat a single parity, and P(2 losses) ≫ binomial on bursty links — **E5 must record loss run-lengths, not just per-part p.** Multi-part amplification is measured elsewhere too: per-chunk stop-and-wait delivery fell 96.9% → **89.0% over 9 hops** ([Solé et al., Computer Communications preprint, Dec 2025](https://research.ac.upc.edu/Asset-project/files/2025_CC_Elsevier_Large_and_reliable_Payloads.pdf)).
- If range tuning is ever revisited: **SF12 breaks voice** — VLoRa measured ~7.5–14 s latency there; SF7–SF11 stayed ≤1–2 s.

### Stage 5 — XOR parity parts + piggybacked bitmap acks

- **The plan's headline number is verified exactly:** 3-part frame completion 0.73 → 0.95 at p=0.9/part is 0.729 → 0.948 (binomial, ours).
- **Confirms the systematic-XOR wire shape** — it is deployed LoRaWAN FUOTA practice ([TS004 v1.0.0, 2018](https://lora-alliance.org/wp-content/uploads/2020/11/fragmented_data_block_transport_v1.0.0.pdf)): first M coded fragments are the data; receivers that predate parity ignore the extras — the same back-compat argument as `part >= of`.
- **Corrects the construction at this size:** TS004's own appendix shows its random-subset parity rows fail **70% of the time with exactly M fragments received, need M+2 on average, M+7 for 99%**, and says the scheme "should not be used for less than 20 fragments." At k=2–9, never use random/rateless rows (LT codes carry ~14% overhead even at K=2,000, worse as K shrinks; no measured LT/RaptorQ-on-LoRa study below k=10 exists). **One systematic XOR parity is MDS for single loss and optimal at 2–4 parts; at k≥5 it under-delivers (0.886 at k=5, 0.736 at k=9 at p=0.9).** Two binary XOR rows cannot fix that (not MDS); the standard second parity is Reed-Solomon (k+2,k) over GF(256) ([RFC 5510](https://www.rfc-editor.org/rfc/rfc5510)) — two 256-byte tables at k≤9. Supporting measurement: RLNC with dense coefficients gained **38–88% decoding success under bursty loss** vs the TS004 code ([TU Delft thesis, Jun 2022](https://repository.tudelft.nl/record/uuid:60364987-404e-490c-857f-e5958df2c325)); QMesh measured FEC lifting packet receive rate **93% → 99%** in a two-relay worst case ([project log, 2020](https://hackaday.io/project/161491-qmesh-a-lora-based-voice-mesh-network)).
- **The "parity pays on contended/marginal links" caveat is supported:** DaRe (up to 70% loss at range) and ReDCoS (**≥25% of received transmissions corrupted** in the field, [arXiv 2107.08868, Jul 2021](https://arxiv.org/abs/2107.08868)) both show real links sit far below the quiet bench's p≈1. (ReDCoS itself targets bit-corrupted frames; the RYLR998 delivers whole-or-nothing, so erasure coding is the right model here.)
- **Bitmap acks: the direction is precedented, the mechanism is not.** SCHC ACK-on-Error reports only errored windows via bitmap and measurably beats ACK-Always on goodput at equal delay ([Sensors 22(6):2120, 2022](https://doi.org/10.3390/s22062120); [RFC 9442, 2023](https://www.rfc-editor.org/rfc/rfc9442.html)); the [compound-ACK draft](https://datatracker.ietf.org/doc/html/draft-ietf-lpwan-schc-compound-ack) batches several bitmaps into one transmission; Meshtastic ships implicit acks with explicit broadcast acks suppressed (≤3 TX broadcast, ≤5 DM, [mesh-algo docs](https://meshtastic.org/docs/overview/mesh-algo/)); and the measured alternative — per-chunk stop-and-wait — costs 11% message loss over 9 hops from timeout exhaustion (Solé et al. 2025). But **no measured piggybacked-bitmap-ack study on LoRa mesh exists.** The bench is the only evidence source for the bitmap mechanics; ship it as planned and validate there.

### Deferred TDMA

**The deferral is right; the revisit trigger is wrong.** Every measured TDMA/slotting win is at 20–25+ saturated nodes: packet loss 25.8% → 5.02% at 20 nodes ([arXiv 2605.23588, May 2026](https://arxiv.org/abs/2605.23588)); >99% PDR at 25 nodes ([TS-LoRa, Comput. Commun. 2020](https://doi.org/10.1016/j.comcom.2020.01.056)); 5.8x throughput at 24 nodes ([arXiv 1902.09468, 2019](https://arxiv.org/abs/1902.09468)). No study shows a TDMA gain at 3 nodes, and sync overhead is pure cost at light load (Beltramelli 2020). Meanwhile CSMA-with-CAD measurably buys nothing below normalized offered load ~0.2 and plateaus at 1.7–2.2x ALOHA under saturation (SFMAC 2025; LMAC 2020: up to 2.2x goodput, 2.4x less energy per frame). What actually breaks CSMA is sustained load or a hidden pair — so the trigger should be **sustained channel utilization above ~25–40%, or an observed hidden AP pair**, not fleet size: three saturated APs can cross it while ten quiet ones never do.

### Changes to the plan

1. **Stage 5:** cap single-XOR-parity protection at 4-part frames, or add a second parity as RS (k+2,k) over GF(256) for k≥5 — one parity gives only 0.886 at k=5, 0.736 at k=9 (TS004 appendix; RFC 5510; arithmetic ours). Never random-subset parity rows below 20 fragments.
2. **E5:** record loss run-lengths (burst vs independent), not just per-part p — burstiness decides whether one parity suffices at k=4–5 and calibrates the whole-note option (DaRe 2017/2020).
3. **Stage 1 field walk:** verify each AP still trips the other APs' CAD at SF8, not just that packets decode — CAD range shrinks faster than link margin (SFMAC 2025 table).
4. **TDMA revisit trigger:** replace "fleet grows past ~3 APs" with "sustained channel utilization >~25–40% or an observed hidden AP pair" (SFMAC; Meshtastic thresholds).
5. **Stage 3:** write into the LGC1 spec that the codec is stateless per frame — never delta against a previous frame (RFC 8724's no-resync rationale); and shape the bundle container so a parity part can be appended when Stage 5 lands.
6. **Stage 4 whole-note option:** present it to the owner with two RS parities (0.974), not one XOR (0.886).
7. **Stage 1:** upgrade the −2.5 dB note from medium to high confidence (dudmuck: 2.5–2.7 dB/step).
8. **Owner decision 2:** add the Beartooth datum — commercial practice kept per-channel transmissions <500 ms for the US dwell rule — when weighing 1.48 s parts.
9. **Stage 3 (optional):** log channel-utilization % against the 25/40 Meshtastic thresholds so the housekeeping cap is self-auditing.

Everything else the evidence confirms as planned: the Stage 3 approach and its mitigations, the Stage 4 codec and segment sizes, unacked voice, deleting standalone acks, and E1-first ordering.

### Where the literature is silent or unreachable

- No study models an RYLR998/AT-module receiver recovery gap, streams voice through an AT module, or measures piggybacked bitmap acks on LoRa mesh. No paper compresses a custom non-IP envelope on point-to-point LoRa. No peer-reviewed measurement of Meshtastic channel utilization exists. Only the bench answers these.
- Paywalled, abstract-only: the VLoRa follow-up (Computer Networks 2025, claims sub-0.8 s latency — settings unverifiable), the IEEE ACK-on-Error tuning paper (2020), Beartooth ICCCN 2021, and LoRaFFEC's full tables (EWSN 2020; a 2022 survey credits it >98% delivery).

### Sources most worth the owner's time

1. [LoRaWAN TS004 v1.0.0 fragmentation spec](https://lora-alliance.org/wp-content/uploads/2020/11/fragmented_data_block_transport_v1.0.0.pdf) — the deployed XOR-parity standard, and its own appendix's case against random codes at small k.
2. [SFMAC, IEEE MASS 2025](https://repository.tudelft.nl/file/File_383450cc-467d-4e60-9f69-aacaf930ed71) — the CAD-range-vs-SF table and the load<0.2 result; the sharpest picture of when CSMA-on-LoRa works and breaks.
3. [VLoRa, ICOIN 2024](https://d2j16w31g89z0j.cloudfront.net/site/icoin2024/abs/C-6-3.pdf) — the only full-text measured codec2-700-over-LoRa paper: payload cliff, packet-size sweet spot, SF latency curves.
4. [dudmuck/lora_codec2](https://github.com/dudmuck/lora_codec2) — the only published codec2-mode-vs-LoRa-settings table; 96% duty with no gap is the existence proof behind E1.
5. [RFC 8724](https://datatracker.ietf.org/doc/html/rfc8724) + [Sensors 2020 measurement](https://pmc.ncbi.nlm.nih.gov/articles/PMC6982818/) — the design LGC1 independently reinvented, and why it chose stateless context.
6. [DaRe, IoTDI 2017](https://dl.acm.org/doi/10.1145/3054977.3054978) — what real LoRa links lose (70% at range) and how burstily; the numbers that make parity worth shipping.
