package org.localgrid.gridwatch.link

import org.localgrid.gridwatch.grid.RESET_WORDS

/**
 * `GET /api/history`, layout 1 (h_history in firmware/node/main/web_admin.c), decoded as
 * tools/grid_watch.py decodes it. Packed binary on the wire, words made here (D49).
 */
object History {
    const val HEADER = 16
    const val AP = 76
    const val AVAIL_MINUTES = 120
    const val INCIDENT = 16
    val KINDS = listOf("down", "restarted", "link", "unexplained")

    data class TimeSet(val unix: Long, val on: Int, val from: Int, val gps: Boolean)

    data class ApRecord(
        val ap: Int, val self: Boolean, val timeQuality: Int, val stratum: Int,
        val syncFrom: Int, val resetReason: Int, val reset: String, val syncDriftMs: Int,
        val uptimeS: Long, val heardS: Long, val syncAgeS: Long, val boot: Long,
        val prevRunS: Long, val name: String, val avail: IntArray,
    ) {
        override fun equals(other: Any?) = this === other
        override fun hashCode() = System.identityHashCode(this)
    }

    data class Incident(
        val downGridTime: Long, val durationS: Int, val prevRunS: Long, val ap: Int,
        val kind: String, val reset: String, val seenBy: Int,
    )

    data class Record(
        val self: Int, val timeSet: TimeSet, val newestMin: Long,
        val aps: List<ApRecord>, val incidents: List<Incident>,
    )

    fun decode(blob: ByteArray): Record {
        if (blob.size < HEADER || blob[0].toInt() != 1) throw LinkException("history bytes are not layout 1")
        fun u8(o: Int) = blob[o].toInt() and 0xFF
        fun u16(o: Int) = u8(o) or (u8(o + 1) shl 8)
        fun i16(o: Int) = u16(o).toShort().toInt()
        fun u32(o: Int): Long = u16(o).toLong() or (u16(o + 2).toLong() shl 16)

        val nAps = u8(2)
        val nInc = u8(3)
        val timeSet = TimeSet(
            unix = u32(4), on = if (u8(8) == 255) -1 else u8(8),
            from = if (u8(9) == 255) -1 else u8(9), gps = u8(10) == 1,
        )
        var o = HEADER
        val aps = ArrayList<ApRecord>(nAps)
        repeat(nAps) {
            if (o + AP > blob.size) return@repeat
            val raw = blob.copyOfRange(o + 28, o + 44)
            val end = raw.indexOfFirst { it.toInt() == 0 }.let { if (it < 0) raw.size else it }
            val syncAge = u32(o + 16)
            val prev = u32(o + 24)
            val avail = IntArray(AVAIL_MINUTES) { m -> (u8(o + 44 + (m shr 2)) shr ((m % 4) * 2)) and 3 }
            aps.add(
                ApRecord(
                    ap = u8(o), self = u8(o + 1) and 1 != 0, timeQuality = u8(o + 2), stratum = u8(o + 3),
                    syncFrom = if (u8(o + 4) == 255) -1 else u8(o + 4), resetReason = u8(o + 5),
                    reset = RESET_WORDS[u8(o + 5)] ?: "reason ${u8(o + 5)}",
                    syncDriftMs = i16(o + 6), uptimeS = u32(o + 8), heardS = u32(o + 12),
                    syncAgeS = if (syncAge == 0xFFFFFFFFL) -1 else syncAge, boot = u32(o + 20),
                    prevRunS = if (prev == 0xFFFFFFFFL) -1 else prev,
                    name = String(raw, 0, end, Charsets.UTF_8), avail = avail,
                ),
            )
            o += AP
        }
        val incidents = ArrayList<Incident>(nInc)
        repeat(nInc) {
            if (o + INCIDENT > blob.size) return@repeat
            val kr = u8(o + 11)
            val prevMin = u16(o + 6)
            incidents.add(
                Incident(
                    downGridTime = u32(o), durationS = u16(o + 4),
                    prevRunS = if (prevMin == 0xFFFF) -1 else prevMin.toLong() * 60,
                    ap = u8(o + 10), kind = KINDS[kr and 3],
                    reset = RESET_WORDS[kr shr 2] ?: "unknown", seenBy = u8(o + 12),
                ),
            )
            o += INCIDENT
        }
        return Record(u8(1), timeSet, u32(12), aps, incidents)
    }
}
