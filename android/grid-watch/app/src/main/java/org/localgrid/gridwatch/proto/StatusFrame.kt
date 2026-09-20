package org.localgrid.gridwatch.proto

import org.localgrid.gridwatch.crypto.ChaCha20
import org.localgrid.gridwatch.crypto.ChaCha20Poly1305
import java.security.MessageDigest

/** The sealed status frame of docs/ble-status.md, mirrored from tools/grid_watch.py. */
object StatusFrame {
    const val MAGIC = 0x53
    const val HEADER = 9
    const val TAG = 4
    const val MAX = 27
    const val MAX_CT = 14

    const val T_HEALTH = 1
    const val T_HANDHELDS = 2
    const val T_ALERT = 3
    const val T_BATTERY = 4
    const val T_NAME = 5

    const val HANDHELD_SLOTS = 20

    private val TYPE_LEN = mapOf(
        T_HEALTH to (10..10), T_HANDHELDS to (14..14), T_ALERT to (10..10),
        T_BATTERY to (0..14), T_NAME to (1..14),
    )

    fun nonce(ap: Int, boot: Long, counter: Int): ByteArray = byteArrayOf(
        ap.toByte(), 0,
        boot.toByte(), (boot ushr 8).toByte(), (boot ushr 16).toByte(), (boot ushr 24).toByte(),
        counter.toByte(), (counter ushr 8).toByte(), (counter ushr 16).toByte(),
        0, 0, 0,
    )

    private fun u8(b: ByteArray, i: Int) = b[i].toInt() and 0xFF
    private fun u16(b: ByteArray, i: Int) = u8(b, i) or (u8(b, i + 1) shl 8)
    private fun u24(b: ByteArray, i: Int) = u16(b, i) or (u8(b, i + 2) shl 16)
    private fun u32(b: ByteArray, i: Int): Long = (u16(b, i).toLong()) or (u16(b, i + 2).toLong() shl 16)

    sealed interface Opened
    data class Frame(val ap: Int, val type: Int, val boot: Long, val counter: Int, val plaintext: ByteArray) : Opened
    data class Rejected(val reason: String) : Opened

    /**
     * Checks and decrypts one status frame. The AEAD's own decrypt cannot check a 4-byte tag,
     * so: decrypt with raw ChaCha20 at block counter 1, seal the recovered plaintext again with
     * the same nonce and AAD, and compare the first 4 bytes of that tag in constant time.
     */
    fun open(k: ByteArray, frame: ByteArray): Opened {
        if (frame.size < HEADER + TAG || frame.size > MAX) return Rejected("length")
        if (u8(frame, 0) != MAGIC) return Rejected("magic")
        val ap = u8(frame, 1) and 0x0F
        val type = u8(frame, 1) shr 4
        val boot = u32(frame, 2)
        val counter = u24(frame, 6)
        val ct = frame.copyOfRange(HEADER, frame.size - TAG)
        val tag = frame.copyOfRange(frame.size - TAG, frame.size)
        if (ct.size > MAX_CT) return Rejected("length")
        val nonce = nonce(ap, boot, counter)
        val aad = frame.copyOfRange(0, HEADER)
        val pt = ChaCha20.xor(k, 1, nonce, ct)
        val sealed = ChaCha20Poly1305.seal(k, nonce, pt, aad)
        val expect = sealed.copyOfRange(pt.size, pt.size + TAG)
        if (!MessageDigest.isEqual(expect, tag)) return Rejected("tag")
        return Frame(ap, type, boot, counter, pt)
    }

    /** The sender's side, as the AP does it. Used by the tests only. */
    fun seal(k: ByteArray, ap: Int, type: Int, boot: Long, counter: Int, pt: ByteArray): ByteArray {
        val header = byteArrayOf(
            MAGIC.toByte(), ((type shl 4) or (ap and 0x0F)).toByte(),
            boot.toByte(), (boot ushr 8).toByte(), (boot ushr 16).toByte(), (boot ushr 24).toByte(),
            counter.toByte(), (counter ushr 8).toByte(), (counter ushr 16).toByte(),
        )
        val sealed = ChaCha20Poly1305.seal(k, nonce(ap, boot, counter), pt, header)
        return header + sealed.copyOfRange(0, pt.size) + sealed.copyOfRange(pt.size, pt.size + TAG)
    }

    sealed interface Body
    data class Health(
        val uptimeMin: Int, val links: Int, val handheldsHere: Int,
        val timeQuality: Int, val timeGps: Boolean, val stratum: Int,
        val heapKb: Int, val resetReason: Int, val restarts: Int, val brownouts: Int,
        val gpsFitted: Boolean, val gpsFix: Boolean, val gpsSats: Int,
    ) : Body
    data class Handhelds(val online: Set<Int>, val where: Map<Int, Int?>) : Body
    data class Alert(
        val active: Boolean, val allClear: Boolean, val author: Int, val ageS: Int,
        val reads: Int, val gridTime: Long,
    ) : Body
    data class Batteries(val batteries: Map<Int, Int>) : Body
    data class Name(val device: Int, val name: String) : Body

    /** Plaintext to a body, or null if the length is wrong for the type. */
    fun decode(type: Int, pt: ByteArray): Body? {
        val range = TYPE_LEN[type] ?: return null
        if (pt.size !in range) return null
        return when (type) {
            T_HEALTH -> {
                val t = u8(pt, 4)
                val g = u8(pt, 9)
                Health(
                    uptimeMin = u16(pt, 0), links = u8(pt, 2), handheldsHere = u8(pt, 3),
                    timeQuality = t and 0x03, timeGps = t and 0x04 != 0, stratum = t shr 3,
                    heapKb = u8(pt, 5), resetReason = u8(pt, 6), restarts = u8(pt, 7),
                    brownouts = u8(pt, 8), gpsFitted = g and 1 != 0, gpsFix = g and 2 != 0,
                    gpsSats = g shr 2,
                )
            }
            T_HANDHELDS -> {
                val online = u32(pt, 0)
                val where = LinkedHashMap<Int, Int?>()
                for (i in 0 until HANDHELD_SLOTS) {
                    val nib = (u8(pt, 4 + i / 2) shr (4 * (i % 2))) and 0x0F
                    where[i + 1] = if (nib == 0x0F) null else nib
                }
                Handhelds((1..32).filter { online and (1L shl (it - 1)) != 0L }.toSet(), where)
            }
            T_ALERT -> Alert(
                active = u8(pt, 0) and 1 != 0, allClear = u8(pt, 0) and 2 != 0, author = u8(pt, 1),
                ageS = u16(pt, 2), reads = u8(pt, 4), gridTime = u32(pt, 6),
            )
            T_BATTERY -> {
                if (pt.size % 2 != 0) return null
                val m = LinkedHashMap<Int, Int>()
                for (i in pt.indices step 2) if (u8(pt, i) != 0) m[u8(pt, i)] = u8(pt, i + 1)
                Batteries(m)
            }
            T_NAME -> Name(u8(pt, 0), String(pt, 1, pt.size - 1, Charsets.UTF_8))
            else -> null
        }
    }
}

/** The discovery payload (lg_proto_config.h) an AP advertises as manufacturer data. */
data class Discovery(
    val version: Int, val disc: ByteArray, val ap: Int, val freeSlots: Int,
    val flags: Int, val attached: Int, val name: String?,
) {
    companion object {
        const val LEN = 12
        const val FLAG_BACKBONE = 0x01
        const val FLAG_TIME = 0x02

        fun parse(p: ByteArray): Discovery? {
            if (p.size < LEN || p[0] != 'L'.code.toByte() || p[1] != 'G'.code.toByte()) return null
            var name: String? = null
            if (p.size > LEN) {
                val n = p[LEN].toInt() and 0xFF
                if (n > 0 && n <= p.size - LEN - 1) name = String(p, LEN + 1, n, Charsets.UTF_8)
            }
            fun u8(i: Int) = p[i].toInt() and 0xFF
            return Discovery(u8(2), p.copyOfRange(3, 7), u8(7), u8(9), u8(10), u8(11), name)
        }
    }
}

object AdParser {
    /**
     * Every manufacturer-specific data entry (AD type 0xFF) under [companyId] in a raw scan
     * record, payload only (after the 2-byte company ID). Android merges the advert and the scan
     * response into one record; both carry this company ID, so ScanRecord's own lookup returns
     * only one of them. Zero length bytes (padding between the two parts) are skipped.
     */
    fun manufacturerPayloads(record: ByteArray?, companyId: Int): List<ByteArray> {
        if (record == null) return emptyList()
        val out = ArrayList<ByteArray>(2)
        var i = 0
        while (i < record.size) {
            val len = record[i].toInt() and 0xFF
            if (len == 0) { i++; continue }
            if (i + 1 + len > record.size) break
            val type = record[i + 1].toInt() and 0xFF
            if (type == 0xFF && len >= 3) {
                val id = (record[i + 2].toInt() and 0xFF) or ((record[i + 3].toInt() and 0xFF) shl 8)
                if (id == companyId) out.add(record.copyOfRange(i + 4, i + 1 + len))
            }
            i += 1 + len
        }
        return out
    }
}
