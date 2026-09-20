package org.localgrid.gridwatch.link

import org.localgrid.gridwatch.crypto.ChaCha20Poly1305
import org.localgrid.gridwatch.crypto.Kdf
import java.security.MessageDigest
import java.util.UUID

/**
 * The sealed admin link of docs/ble-link.md (D70), mirrored from tools/grid_watch.py.
 *
 * The watcher connects to an AP it already hears beaconing, says HELLO, proves it knows the
 * admin password without sending it, and asks for the admin page's own status and history bytes
 * plus the traffic counters. Every message is sealed with ChaCha20-Poly1305 and a full 16-byte
 * tag under K_link; the 4-byte header travels in the clear as the AAD.
 *
 * Read-only by construction: the opcodes below are every opcode there is, and not one of them
 * writes anything, carries message text, or carries audio. The watcher monitors and takes no
 * part in the grid (owner, 2026-09-19).
 */
object LinkProtocol {
    val KDF_SALT = "LG-BLE-LINK-1".toByteArray(Charsets.US_ASCII)
    val KDF_INFO = "admin link".toByteArray(Charsets.US_ASCII)
    val PROOF_CONTEXT = "lg-ble-admin".toByteArray(Charsets.US_ASCII)

    const val VERSION = 1
    const val HEADER = 4                  // opcode u8, flags u8, length u16 LE
    const val TAG = 16
    const val FLAG_MORE = 0x01
    const val CHUNK_BODY = 32             // body bytes per request chunk; requests are tiny
    const val MAX_REPLY = 64 * 1024       // longer than this is a fault, not a message

    const val DIR_TO_AP = 0
    const val DIR_TO_CLIENT = 1

    const val OP_HELLO = 0x01
    const val OP_LOGIN = 0x02
    const val OP_GET_STATUS = 0x03
    const val OP_GET_HISTORY = 0x04
    const val OP_GET_TRAFFIC = 0x05
    const val OP_SESSION = 0x80           // the one message that is not sealed
    const val OP_HELLO_OK = 0x81
    const val OP_LOGIN_OK = 0x82
    const val OP_STATUS = 0x83
    const val OP_HISTORY = 0x84
    const val OP_TRAFFIC = 0x85
    const val OP_ERROR = 0xC0
    const val OP_LOGIN_FAIL = 0xC2

    const val HELLO_OK_LEN = 1 + 1 + 4 + 1 + 16 + 4 + 32
    const val CHALLENGE_LEN = 32
    const val PROOF_LEN = 32

    /** The AP refuses an ATT MTU under this, so a chunk always has room for a body. */
    const val MTU_MIN = 64
    const val MTU_WANTED = 247

    val SERVICE: UUID = UUID.fromString("4c470001-6c67-4772-6964-42544c453031")
    val REQUEST: UUID = UUID.fromString("4c470002-6c67-4772-6964-42544c453031")
    val REPLY: UUID = UUID.fromString("4c470003-6c67-4772-6964-42544c453031")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    fun opName(opcode: Int): String = when (opcode) {
        OP_HELLO -> "HELLO"; OP_LOGIN -> "LOGIN"; OP_GET_STATUS -> "GET_STATUS"
        OP_GET_HISTORY -> "GET_HISTORY"; OP_GET_TRAFFIC -> "GET_TRAFFIC"; OP_SESSION -> "SESSION"
        OP_HELLO_OK -> "HELLO_OK"; OP_LOGIN_OK -> "LOGIN_OK"; OP_STATUS -> "STATUS"
        OP_HISTORY -> "HISTORY"; OP_TRAFFIC -> "TRAFFIC"; OP_ERROR -> "ERROR"
        OP_LOGIN_FAIL -> "LOGIN_FAIL"
        else -> "0x%02X".format(opcode)
    }

    /** K_link = HKDF-SHA256(salt "LG-BLE-LINK-1", ikm = K, info = "admin link", 32). */
    fun linkKey(k: ByteArray): ByteArray = Kdf.hkdfSha256(KDF_SALT, k, KDF_INFO)

    /** dir (u8), session (u16 LE), 0x00, counter (u32 LE), four zero bytes. */
    fun nonce(direction: Int, session: Int, counter: Long): ByteArray = byteArrayOf(
        (direction and 1).toByte(), session.toByte(), (session ushr 8).toByte(), 0,
        counter.toByte(), (counter ushr 8).toByte(), (counter ushr 16).toByte(),
        (counter ushr 24).toByte(), 0, 0, 0, 0,
    )

    fun header(opcode: Int, flags: Int, length: Int) =
        byteArrayOf(opcode.toByte(), flags.toByte(), length.toByte(), (length ushr 8).toByte())

    /** One sealed message: header in the clear, body and tag sealed under it. */
    fun seal(key: ByteArray, direction: Int, session: Int, counter: Long,
             opcode: Int, flags: Int, body: ByteArray): ByteArray {
        val h = header(opcode, flags, body.size)
        return h + ChaCha20Poly1305.seal(key, nonce(direction, session, counter), body, h)
    }

    data class Message(val opcode: Int, val flags: Int, val body: ByteArray) {
        val more: Boolean get() = flags and FLAG_MORE != 0
        override fun equals(other: Any?) = other is Message && opcode == other.opcode &&
            flags == other.flags && body.contentEquals(other.body)
        override fun hashCode() = (opcode * 31 + flags) * 31 + body.contentHashCode()
    }

    /**
     * The other side of [seal]. Anything that does not add up raises: a short message, a length
     * that disagrees with the header, or a tag that fails because the key, the session, the
     * direction or the counter is not the one that sealed it (docs/ble-link.md).
     */
    fun open(key: ByteArray, direction: Int, session: Int, counter: Long, data: ByteArray): Message {
        if (data.size < HEADER + TAG) throw LinkException("a link message shorter than a header and a tag")
        val h = data.copyOfRange(0, HEADER)
        val sealed = data.copyOfRange(HEADER, data.size)
        val length = (h[2].toInt() and 0xFF) or ((h[3].toInt() and 0xFF) shl 8)
        if (length != sealed.size - TAG) throw LinkException("the length in the header does not match the message")
        val body = ChaCha20Poly1305.open(key, nonce(direction, session, counter), sealed, h)
            ?: throw LinkException("a link message failed its tag: wrong key, session, or counter")
        return Message(h[0].toInt() and 0xFF, h[1].toInt() and 0xFF, body)
    }

    /** The sender's side of the chunking: an empty body still sends one final chunk. */
    fun splitChunks(body: ByteArray, chunk: Int = CHUNK_BODY): List<Pair<Int, ByteArray>> {
        if (body.isEmpty()) return listOf(0 to ByteArray(0))
        val parts = ArrayList<Pair<Int, ByteArray>>()
        var i = 0
        while (i < body.size) {
            val end = minOf(i + chunk, body.size)
            parts.add((if (end < body.size) FLAG_MORE else 0) to body.copyOfRange(i, end))
            i = end
        }
        return parts
    }

    // -- logging in (docs/ble-link.md "Logging in")

    /** What the AP stores: PBKDF2-HMAC-SHA256(password, salt, iterations, 32). */
    fun loginHash(password: CharArray, salt: ByteArray, iterations: Int): ByteArray =
        Kdf.pbkdf2Sha256(password, salt, iterations)

    /** HMAC-SHA256(key = the stored hash, message = challenge || "lg-ble-admin"). */
    fun loginProof(hash32: ByteArray, challenge: ByteArray): ByteArray =
        Kdf.hmacSha256(hash32, challenge + PROOF_CONTEXT)

    data class Hello(
        val version: Int, val ap: Int, val boot: Long, val passwordSet: Boolean,
        val salt: ByteArray, val iterations: Int, val challenge: ByteArray,
    ) {
        override fun equals(other: Any?) = this === other
        override fun hashCode() = System.identityHashCode(this)
    }

    /**
     * version u8, AP u8, boot u32, password set u8, salt 16, iterations u32, challenge 32.
     * The challenge is the last field: the doc's table stops at the iterations, but logging in
     * needs it, and ble_link.c builds exactly these 59 bytes.
     */
    fun parseHelloOk(body: ByteArray): Hello {
        if (body.size < HELLO_OK_LEN) throw LinkException("HELLO_OK is too short")
        fun u8(i: Int) = body[i].toInt() and 0xFF
        fun u32(i: Int): Long = (u8(i).toLong()) or (u8(i + 1).toLong() shl 8) or
            (u8(i + 2).toLong() shl 16) or (u8(i + 3).toLong() shl 24)
        return Hello(
            version = u8(0), ap = u8(1), boot = u32(2), passwordSet = u8(6) != 0,
            salt = body.copyOfRange(7, 23), iterations = u32(23).toInt(),
            challenge = body.copyOfRange(27, 59),
        )
    }

    fun constantTimeEquals(a: ByteArray, b: ByteArray) = MessageDigest.isEqual(a, b)
}

/** The link failed its own rules: a bad tag, a counter out of order, or a refused request. */
class LinkException(message: String) : Exception(message)
