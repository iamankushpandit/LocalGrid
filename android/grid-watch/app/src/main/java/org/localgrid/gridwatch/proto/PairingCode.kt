package org.localgrid.gridwatch.proto

import java.util.Base64

/**
 * The D69 pairing code: `LGW1:` + base64url (no padding) of company ID (u16 LE),
 * discriminator (4 bytes) and the derived status key K (32 bytes). K is already derived, so the
 * phone never sees the backbone key and never runs HKDF.
 */
class Pairing(val companyId: Int, val discriminator: ByteArray, val key: ByteArray) {
    /** The grid ID as shown to people: the discriminator in hex. It is public (discovery). */
    val gridId: String get() = discriminator.joinToString("") { "%02X".format(it.toInt() and 0xFF) }

    fun toBytes(): ByteArray =
        byteArrayOf(companyId.toByte(), (companyId ushr 8).toByte()) + discriminator + key

    fun toCode(): String = PairingCode.PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(toBytes())

    companion object {
        fun fromBytes(b: ByteArray): Pairing? {
            if (b.size != PairingCode.BYTES) return null
            val id = (b[0].toInt() and 0xFF) or ((b[1].toInt() and 0xFF) shl 8)
            return Pairing(id, b.copyOfRange(2, 6), b.copyOfRange(6, 38))
        }
    }
}

object PairingCode {
    const val PREFIX = "LGW1:"
    const val BYTES = 38
    const val CHARS = 51                               // base64url of 38 bytes, no padding

    sealed interface Result
    data class Ok(val pairing: Pairing) : Result
    data class Bad(val message: String) : Result

    private val ALPHABET = Regex("^[A-Za-z0-9_-]+$")

    fun parse(text: String): Result {
        val t = text.trim()
        if (t.isEmpty()) return Bad("The code is empty.")
        if (!t.startsWith(PREFIX)) {
            return Bad("This is not a LocalGrid Watch pairing code: it must start with \"$PREFIX\". " +
                "Show the code with python tools/grid_watch.py --pair on the laptop.")
        }
        val body = t.substring(PREFIX.length)
        if (!ALPHABET.matches(body)) return Bad("The code has characters that do not belong in it.")
        if (body.length != CHARS) {
            return Bad("The code has the wrong length (${body.length} characters after \"$PREFIX\", " +
                "expected $CHARS). It may be cut short or from another version.")
        }
        val bytes = try {
            Base64.getUrlDecoder().decode(body)
        } catch (e: IllegalArgumentException) {
            return Bad("The code cannot be decoded.")
        }
        val p = Pairing.fromBytes(bytes) ?: return Bad("The code holds ${bytes.size} bytes, expected $BYTES.")
        return Ok(p)
    }
}
