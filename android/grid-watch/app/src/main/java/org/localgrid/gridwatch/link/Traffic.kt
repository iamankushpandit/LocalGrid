package org.localgrid.gridwatch.link

import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The `TRAFFIC` reply (opcode 0x85), the packed record firmware/node/main/traffic.c builds
 * (docs/ble-link.md, layout 1). Little-endian; the phone does the arithmetic and the wording.
 *
 * Counts and sizes only: no message body, no audio, no sender-recipient pair beyond the device
 * numbers the status reply already carries, and no hardware address (D21).
 */
object Traffic {
    const val LAYOUT = 1
    const val HEADER = 24
    const val CLASS = 12
    const val SESS = 28
    const val PERF = 40
    const val LINK = 28
    const val BUCKET = 4

    /**
     * D71's LoRa section: 64 bytes appended once after the last link entry. It is *appended*, not
     * numbered in — the layout byte stays 1 — so an AP built before D71, or a record cut short on
     * the way here, simply has no section and every LoRa field stays null. Never guessed.
     */
    const val LORA = 64

    /** lora.h: the module's fixed settings, the same on every AP of a grid. */
    object Radio {
        const val BAND_MHZ = "868.5 MHz"
        const val SF = 9
        const val BW_KHZ = 125
        const val CR = "4/5"
        const val PREAMBLE = 12
        const val POWER_DBM = 22
        val words: String get() = "SF$SF, BW $BW_KHZ kHz, CR $CR, $BAND_MHZ, $POWER_DBM dBm"
    }

    /** lg_traffic_class_t (components/lg_core/include/lg_node.h), in its own order. */
    val CLASSES = listOf(
        "direct", "group", "broadcast", "voice", "ack", "presence", "announce", "position",
        "time", "other",
    )
    val FAULTS = listOf(
        "duplicate", "table_full", "unknown_recipient", "decrypt_failed", "ttl_expired",
        "queue_full", "send_timeout", "voice_dropped", "malformed", "rejected",
    )

    val CLASS_WORDS = mapOf(
        "direct" to "1:1 text", "group" to "group text", "broadcast" to "broadcast and urgent",
        "voice" to "voice (push to talk)", "ack" to "delivered and read reports",
        "presence" to "presence", "announce" to "groups, grid state, names",
        "position" to "positions", "time" to "time", "other" to "hellos, pings, diagnostics",
    )
    val FAULT_WORDS = mapOf(
        "duplicate" to "duplicates suppressed", "table_full" to "table full",
        "unknown_recipient" to "unknown recipient", "decrypt_failed" to "failed to decrypt",
        "ttl_expired" to "TTL expired", "queue_full" to "send queue full",
        "send_timeout" to "send timed out", "voice_dropped" to "voice frames dropped",
        "malformed" to "malformed messages", "rejected" to "rejected messages",
    )
    val FAULTS_THAT_MATTER = setOf("table_full", "decrypt_failed", "queue_full", "send_timeout", "voice_dropped")

    data class Counts(val inCount: Long, val out: Long, val relayed: Long)
    data class Bucket(val inCount: Int, val out: Int) {
        val messages: Int get() = inCount + out
    }

    data class Link(
        val ap: Int, val up: Boolean, val rssi: Int, val sent: Long, val bytesOut: Long,
        val received: Long, val bytesIn: Long, val failures: Long, val heardS: Long?,
    )

    data class Handhelds(
        val sessions: Int, val registered: Int, val opened: Long, val registrations: Long,
        val disconnects: Long, val bytesIn: Long, val bytesOut: Long, val slowestSendMs: Long,
    )

    data class Perf(
        val heapFree: Long, val heapMin: Long, val heapLargest: Long, val queueDepth: Int,
        val queueHigh: Int, val stackCore: Long, val stackLink: Long, val loopMaxMs: Long,
        /** Offset 28 is microseconds (traffic.c: loop_total_us / loop_passes); offset 24 is ms. */
        val loopAvgUs: Long, val espnowErrors: Long, val wifiErrors: Long, val cpuBusy: Int?,
    )

    /**
     * The second backbone (D71), as the AP packs it. Facts only; the wording is the phone's.
     *
     * The record carries one last RSSI and SNR for the radio as a whole and a bitmask of which
     * APs' heartbeats are current — not a line per peer. The per-peer signal the admin page shows
     * comes from `/api/status` ([Status.Lora]), and only for the AP this phone actually asked.
     */
    data class Lora(
        val fitted: Boolean, val configured: Boolean, val broadcast: Boolean, val off: Boolean,
        val big: Boolean, val address: Int, val network: Int, val rssi: Int, val snr: Int,
        val framesOut: Long, val framesIn: Long, val framesFirst: Long,
        val partsOut: Long, val partsIn: Long, val partsDropped: Long,
        val reasmTimeouts: Long, val sealFail: Long, val refusedBig: Long,
        val queueDepth: Int, val queueHigh: Int, val airtimeMs: Long, val retries: Long,
        val queueDropped: Long, val heardS: Long?, val peerBits: Int, val restarts: Int,
    ) {
        /** The AP indexes whose heartbeat is current (bit n is AP n). */
        val peersUp: List<Int> get() = (0..7).filter { (peerBits shr it) and 1 == 1 }

        /** Everything that went wrong, in one number. */
        val trouble: Long get() = partsDropped + reasmTimeouts + sealFail + queueDropped + refusedBig

        /** A module is there, answered at boot, and has never heard a thing. */
        val silent: Boolean get() = fitted && heardS == null

        val airtimeS: Double get() = airtimeMs / 1000.0
    }

    data class Record(
        val ap: Int, val cpuBusy: Int?, val bucketS: Double, val uptimeS: Long, val gridTime: Long,
        val inTotal: Long, val outTotal: Long, val messages: Map<String, Counts>,
        val faults: Map<String, Long>, val handhelds: Handhelds, val perf: Perf,
        val buckets: List<Bucket>, val links: List<Link>,
        val rate1m: Double?, val rate5m: Double?,
        /** Null when the AP sent no LoRa section at all: an AP older than D71, never a guess. */
        val lora: Lora? = null,
    ) {
        val totalMessages: Long get() = inTotal + outTotal
        fun fault(name: String): Long = faults[name] ?: 0L
    }

    fun decode(blob: ByteArray): Record {
        if (blob.size < HEADER || blob[0].toInt() != LAYOUT) throw LinkException("traffic bytes are not layout 1")
        fun u8(o: Int) = blob[o].toInt() and 0xFF
        fun u16(o: Int) = u8(o) or (u8(o + 1) shl 8)
        fun u32(o: Int): Long = u16(o).toLong() or (u16(o + 2).toLong() shl 16)

        val nLinks = u8(2)
        val nClasses = u8(3)
        val nBuckets = u8(4)
        val cpu = u8(5)
        if (nClasses != CLASSES.size) {
            throw LinkException("this AP counts $nClasses message classes, this app knows ${CLASSES.size}")
        }
        var o = HEADER
        val need = nClasses * CLASS + FAULTS.size * 4 + SESS + PERF + nBuckets * BUCKET
        if (o + need > blob.size) throw LinkException("traffic bytes stop before the fixed sections end")

        val messages = LinkedHashMap<String, Counts>()
        for (cls in CLASSES) {
            messages[cls] = Counts(u32(o), u32(o + 4), u32(o + 8))
            o += CLASS
        }
        val faults = LinkedHashMap<String, Long>()
        for (name in FAULTS) {
            faults[name] = u32(o)
            o += 4
        }
        val handhelds = Handhelds(
            sessions = u8(o), registered = u8(o + 1), opened = u32(o + 4), registrations = u32(o + 8),
            disconnects = u32(o + 12), bytesIn = u32(o + 16), bytesOut = u32(o + 20),
            slowestSendMs = u32(o + 24),
        )
        o += SESS
        val perf = Perf(
            heapFree = u32(o), heapMin = u32(o + 4), heapLargest = u32(o + 8), queueDepth = u16(o + 12),
            queueHigh = u16(o + 14), stackCore = u32(o + 16), stackLink = u32(o + 20),
            loopMaxMs = u32(o + 24), loopAvgUs = u32(o + 28), espnowErrors = u32(o + 32),
            wifiErrors = u32(o + 36), cpuBusy = if (cpu == 255) null else cpu,
        )
        o += PERF
        val buckets = ArrayList<Bucket>(nBuckets)
        repeat(nBuckets) {
            buckets.add(Bucket(u16(o), u16(o + 2)))
            o += BUCKET
        }
        val links = ArrayList<Link>(nLinks)
        repeat(nLinks) {
            if (o + LINK > blob.size) return@repeat
            val heard = u32(o + 24)
            links.add(
                Link(
                    ap = u8(o), up = u8(o + 1) != 0, rssi = blob[o + 2].toInt(), sent = u32(o + 4),
                    bytesOut = u32(o + 8), received = u32(o + 12), bytesIn = u32(o + 16),
                    failures = u32(o + 20),
                    heardS = if (heard == 0xFFFFFFFFL) null else (heard / 1000.0).roundToInt().toLong(),
                ),
            )
            o += LINK
        }

        // D71, appended after the links. Absent on an AP built before it, and absent (rather than
        // half-read) if the record stops inside it: an incomplete radio report is worse than none.
        val lora = if (o + LORA <= blob.size) {
            val flags = u8(o)
            val heard = u32(o + 48)
            Lora(
                fitted = flags and 0x01 != 0, configured = flags and 0x02 != 0,
                broadcast = flags and 0x04 != 0, off = flags and 0x08 != 0,
                big = flags and 0x10 != 0,
                address = u8(o + 1), network = u8(o + 54),
                rssi = blob[o + 2].toInt(), snr = blob[o + 3].toInt(),
                framesOut = u32(o + 4), framesIn = u32(o + 8), framesFirst = u32(o + 56),
                partsOut = u32(o + 12), partsIn = u32(o + 16), partsDropped = u32(o + 20),
                reasmTimeouts = u32(o + 24), sealFail = u32(o + 28), refusedBig = u32(o + 60),
                queueDepth = u16(o + 32), queueHigh = u16(o + 34), airtimeMs = u32(o + 36),
                retries = u32(o + 40), queueDropped = u32(o + 44),
                heardS = if (heard == 0xFFFFFFFFL) null else (heard / 1000.0).roundToInt().toLong(),
                peerBits = u8(o + 52), restarts = u8(o + 53),
            )
        } else {
            null
        }
        val bs = if (u16(6) > 0) u16(6) / 1000.0 else 30.0

        fun rate(seconds: Double): Double? {
            if (buckets.isEmpty()) return null
            val want = max(1, (seconds / bs).roundToInt())
            val take = buckets.takeLast(want)
            return ((60.0 * take.sumOf { it.messages } / (take.size * bs)) * 10).roundToInt() / 10.0
        }

        return Record(
            ap = u8(1), cpuBusy = if (cpu == 255) null else cpu, bucketS = bs, uptimeS = u32(8),
            gridTime = u32(12), inTotal = u32(16), outTotal = u32(20), messages = messages,
            faults = faults, handhelds = handhelds, perf = perf, buckets = buckets, links = links,
            rate1m = rate(60.0), rate5m = rate(300.0), lora = lora,
        )
    }

    /**
     * Limits worth saying out loud, in plain English and in red. The same numbers the laptop
     * dashboard uses, so a watcher on either sees the same grid the same way.
     */
    object Limit {
        const val HEAP_WARN_KB = 40
        const val HEAP_BAD_KB = 20
        const val QUEUE_WARN = 8
        const val QUEUE_BAD = 16
        const val LOOP_WARN_MS = 100
        const val LOOP_BAD_MS = 500
        const val SEND_WAIT_WARN_MS = 2000
        const val CPU_WARN = 85
        const val LINK_FAIL_PCT = 5
        const val STACK_LOW_B = 1024

        // LoRa (D71). The signal steps are the admin page's own (firmware/node/main/web/admin.html);
        // the queue is four small slots (lora.c SMALL_TXQ_SLOTS), so 4 waiting means full.
        const val LORA_RSSI_WARN = -100
        const val LORA_RSSI_BAD = -115
        const val LORA_QUEUE_WARN = 3
        const val LORA_QUEUE_BAD = 4

        /** A module that answered at boot and has heard nothing for this long is not talking. */
        const val LORA_SILENT_S = 300L

        /** Airtime as a share of the time the AP has been up: the radio is busy past this. */
        const val LORA_AIRTIME_PCT_WARN = 5.0
        const val LORA_AIRTIME_PCT_BAD = 10.0
    }

    /** How loud a LoRa signal is, in the admin page's three steps. */
    fun loraSignalIsBad(rssi: Int): Boolean = rssi < Limit.LORA_RSSI_BAD
    fun loraSignalIsWeak(rssi: Int): Boolean = rssi < Limit.LORA_RSSI_WARN

    /**
     * The average pass through the AP's main loop, in words. The AP counts it in microseconds
     * (traffic.c writes loop_total_us / loop_passes at offset 28, while offset 24 is
     * milliseconds), so a healthy AP's average is a handful of microseconds and "0.01 ms" would
     * hide that.
     */
    fun avgPass(us: Long): String =
        if (us < 1000) "$us µs" else String.format(java.util.Locale.US, "%.2f ms", us / 1000.0)

    /** A note is (text, red): red means it crossed a limit, amber means keep an eye on it. */
    data class Note(val text: String, val red: Boolean)

    fun notes(
        t: Record,
        name: String,
        loraPeers: List<Status.LoraPeer> = emptyList(),
        apName: (Int) -> String,
    ): List<Note> {
        val out = ArrayList<Note>()
        val p = t.perf
        fun add(text: String, red: Boolean) = out.add(Note(text, red))
        if (t.fault("voice_dropped") > 0) {
            add(
                "$name has dropped ${t.fault("voice_dropped")} voice frames since it started: " +
                    "push-to-talk sounds broken when it does. The AP drops voice rather than block " +
                    "the rest of the grid.",
                true,
            )
        }
        if (t.fault("queue_full") > 0) {
            val n = t.fault("queue_full")
            add("$name's send queue filled $n ${if (n == 1L) "time" else "times"}: it could not keep up with what it was asked to send.", true)
        }
        if (t.fault("table_full") > 0) add("$name ran out of table room ${t.fault("table_full")} times: messages were refused.", true)
        if (t.fault("send_timeout") > 0) add("$name timed out sending ${t.fault("send_timeout")} times.", true)
        if (t.fault("decrypt_failed") > 0) {
            add("$name could not decrypt ${t.fault("decrypt_failed")} messages: an AP or handheld may be running different secrets.", true)
        }
        if (t.fault("ttl_expired") > 0) {
            add("${t.fault("ttl_expired")} messages died of old age (TTL) at $name: the path was too long or the grid was split.", false)
        }
        val junk = t.fault("malformed") + t.fault("rejected")
        if (junk > 0) add("$name threw away $junk ${if (junk == 1L) "message" else "messages"} it could not make sense of.", false)
        val heapKb = (p.heapMin / 1024.0).roundToInt()
        if (heapKb < Limit.HEAP_BAD_KB) add("$name has been down to $heapKb KB of free memory: it is close to restarting itself.", true)
        else if (heapKb < Limit.HEAP_WARN_KB) add("$name's lowest free memory was $heapKb KB; watch it.", false)
        if (p.queueHigh >= Limit.QUEUE_BAD) add("$name's core queue reached ${p.queueHigh} waiting messages: work is piling up.", true)
        else if (p.queueHigh >= Limit.QUEUE_WARN) add("$name's core queue reached ${p.queueHigh} waiting messages.", false)
        if (p.loopMaxMs >= Limit.LOOP_BAD_MS) add("$name had a ${p.loopMaxMs} ms pass through its main loop: something blocked it.", true)
        else if (p.loopMaxMs >= Limit.LOOP_WARN_MS) add("$name's longest main-loop pass was ${p.loopMaxMs} ms.", false)
        val stack = minOf(p.stackCore, p.stackLink)
        if (stack < Limit.STACK_LOW_B) add("$name is down to $stack bytes of spare stack: a crash waiting to happen.", true)
        if (p.cpuBusy != null && p.cpuBusy >= Limit.CPU_WARN) add("$name is ${p.cpuBusy}% busy.", true)
        if (p.espnowErrors > 0) add("$name had ${p.espnowErrors} ESP-NOW send errors.", false)
        if (t.handhelds.slowestSendMs >= Limit.SEND_WAIT_WARN_MS) {
            add("$name kept a handheld waiting ${t.handhelds.slowestSendMs} ms to send.", false)
        }
        for (l in t.links) {
            val pct = if (l.sent > 0) 100.0 * l.failures / l.sent else 0.0
            if (!l.up) add("The link from $name to ${apName(l.ap)} is down.", true)
            else if (pct >= Limit.LINK_FAIL_PCT) {
                add("The link from $name to ${apName(l.ap)} is failing ${pct.roundToInt()}% of its sends.", true)
            }
        }
        out += loraNotes(t, name, loraPeers, apName)
        return out
    }

    /**
     * What the second backbone (D71) is doing wrong, in plain English.
     *
     * "No module fitted" is never a fault: an AP is never expected to have one, and a grid may
     * mix APs with and without. A module that is fitted and has heard nothing is a fault, and the
     * two are told apart here rather than left to the reader.
     */
    fun loraNotes(
        t: Record,
        name: String,
        peers: List<Status.LoraPeer> = emptyList(),
        apName: (Int) -> String,
    ): List<Note> {
        val out = ArrayList<Note>()
        val l = t.lora ?: return out            // an AP older than D71 says nothing, so nor do we
        if (!l.fitted) return out               // no module here: by design, not a fault
        fun add(text: String, red: Boolean) = out.add(Note(text, red))

        if (!l.configured) {
            add("$name's LoRa module has not been configured: it answered, but the settings did not take.", true)
        }
        if (l.off) add("$name's LoRa is switched off for a test and will come back by itself.", false)
        if (l.silent) {
            add("$name has a LoRa module and has never heard another one: check the antenna, the network ID and that another AP has a module.", true)
        } else if (l.heardS != null && l.heardS >= Limit.LORA_SILENT_S) {
            add("$name has heard nothing over LoRa for ${l.heardS / 60} minutes, although a module is fitted.", true)
        }
        for (p in peers) {
            if (!p.up) add("$name's LoRa link to ${apName(p.ap)} is down; it was last heard ${if (p.ageS == null) "never" else "${p.ageS} s ago"}.", true)
            else if (loraSignalIsBad(p.rssi)) {
                add("$name hears ${apName(p.ap)} over LoRa at only ${p.rssi} dBm: near the edge of range.", true)
            }
        }
        if (peers.isEmpty() && l.peersUp.isEmpty() && !l.silent) {
            add("$name has a LoRa module but no peer's heartbeat is current: the LoRa backbone is down here.", true)
        }
        if (l.sealFail > 0) {
            add("$name could not authenticate ${l.sealFail} LoRa ${if (l.sealFail == 1L) "frame" else "frames"}: another AP may be running different secrets, or another network is on this channel.", true)
        }
        if (l.queueDropped > 0) {
            add("$name's LoRa send queue overflowed ${l.queueDropped} times: the oldest low-priority frames were dropped (never an alert).", true)
        }
        if (l.queueDepth >= Limit.LORA_QUEUE_BAD) {
            add("$name's LoRa send queue is full right now (${l.queueDepth} of ${Limit.LORA_QUEUE_BAD} slots): the radio cannot keep up with what it is asked to carry.", true)
        } else if (l.queueHigh >= Limit.LORA_QUEUE_WARN) {
            // A full queue on its own loses nothing - the AP waits. Losing a frame is queueDropped.
            add("$name's LoRa send queue has reached ${l.queueHigh} of ${Limit.LORA_QUEUE_BAD} waiting frames; a LoRa frame is seconds of airtime, so a short queue is normal.", false)
        }
        if (l.refusedBig > 0) {
            add("$name refused ${l.refusedBig} LoRa payloads for being larger than it can hold; nothing was truncated.", true)
        }
        if (l.partsDropped > 0) {
            add("$name dropped ${l.partsDropped} LoRa ${if (l.partsDropped == 1L) "part" else "parts"} (malformed, duplicate, or no room to reassemble).", false)
        }
        if (l.reasmTimeouts > 0) {
            add("${l.reasmTimeouts} LoRa ${if (l.reasmTimeouts == 1L) "frame" else "frames"} never arrived complete at $name and were given up.", false)
        }
        if (l.restarts > 0) add("$name has had to reset a wedged LoRa module ${l.restarts} times.", false)
        val pct = if (t.uptimeS > 0) 100.0 * l.airtimeS / t.uptimeS else 0.0
        if (pct >= Limit.LORA_AIRTIME_PCT_BAD) {
            add("$name's LoRa radio has been transmitting ${pct.roundToInt()}% of the time: the air is crowded and frames will start waiting.", true)
        } else if (pct >= Limit.LORA_AIRTIME_PCT_WARN) {
            add("$name's LoRa radio has been transmitting ${pct.roundToInt()}% of the time.", false)
        }
        return out
    }

    /**
     * The one line that says whether the radio earns its keep, in the admin page's words: frames
     * that arrived over LoRa and that Wi-Fi had *not* already delivered.
     */
    fun loraWorth(l: Lora): String =
        if (l.framesFirst > 0) {
            "${l.framesFirst} of the ${l.framesIn} frames that arrived by LoRa got here first, " +
                "before Wi-Fi had them."
        } else {
            "Nothing has needed LoRa yet: every frame it carried had already arrived over Wi-Fi."
        }
}
