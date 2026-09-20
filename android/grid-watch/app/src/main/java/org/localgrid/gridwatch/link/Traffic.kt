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

    data class Record(
        val ap: Int, val cpuBusy: Int?, val bucketS: Double, val uptimeS: Long, val gridTime: Long,
        val inTotal: Long, val outTotal: Long, val messages: Map<String, Counts>,
        val faults: Map<String, Long>, val handhelds: Handhelds, val perf: Perf,
        val buckets: List<Bucket>, val links: List<Link>,
        val rate1m: Double?, val rate5m: Double?,
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
            rate1m = rate(60.0), rate5m = rate(300.0),
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
    }

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

    fun notes(t: Record, name: String, apName: (Int) -> String): List<Note> {
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
        return out
    }
}
