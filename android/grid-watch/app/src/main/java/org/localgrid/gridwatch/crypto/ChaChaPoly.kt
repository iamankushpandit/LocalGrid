package org.localgrid.gridwatch.crypto

import java.math.BigInteger
import java.security.MessageDigest

/**
 * ChaCha20 and Poly1305 as RFC 8439 defines them, and the ChaCha20-Poly1305 AEAD seal.
 *
 * Small and dependency-free: the app handles one frame of at most 27 bytes every half second, so
 * clarity wins over speed. Tested against the RFC's own vectors (2.4.2, 2.5.2, 2.8.2).
 */
object ChaCha20 {
    private fun rotl(v: Int, c: Int) = (v shl c) or (v ushr (32 - c))

    private fun le32(b: ByteArray, off: Int) =
        (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8) or
            ((b[off + 2].toInt() and 0xFF) shl 16) or ((b[off + 3].toInt() and 0xFF) shl 24)

    /** One 64-byte keystream block (RFC 8439 2.3). */
    fun block(key: ByteArray, counter: Int, nonce: ByteArray): ByteArray {
        require(key.size == 32) { "ChaCha20 key must be 32 bytes" }
        require(nonce.size == 12) { "ChaCha20 nonce must be 12 bytes" }
        val s = IntArray(16)
        s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574
        for (i in 0 until 8) s[4 + i] = le32(key, 4 * i)
        s[12] = counter
        for (i in 0 until 3) s[13 + i] = le32(nonce, 4 * i)
        val x = s.copyOf()
        fun qr(a: Int, b: Int, c: Int, d: Int) {
            x[a] += x[b]; x[d] = rotl(x[d] xor x[a], 16)
            x[c] += x[d]; x[b] = rotl(x[b] xor x[c], 12)
            x[a] += x[b]; x[d] = rotl(x[d] xor x[a], 8)
            x[c] += x[d]; x[b] = rotl(x[b] xor x[c], 7)
        }
        repeat(10) {
            qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
            qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
        }
        val out = ByteArray(64)
        for (i in 0 until 16) {
            val v = x[i] + s[i]
            out[4 * i] = v.toByte()
            out[4 * i + 1] = (v ushr 8).toByte()
            out[4 * i + 2] = (v ushr 16).toByte()
            out[4 * i + 3] = (v ushr 24).toByte()
        }
        return out
    }

    /** Encrypts or decrypts (the same operation) starting at block [counter] (RFC 8439 2.4). */
    fun xor(key: ByteArray, counter: Int, nonce: ByteArray, input: ByteArray): ByteArray {
        val out = ByteArray(input.size)
        var off = 0
        var ctr = counter
        while (off < input.size) {
            val ks = block(key, ctr++, nonce)
            val n = minOf(64, input.size - off)
            for (i in 0 until n) out[off + i] = (input[off + i].toInt() xor ks[i].toInt()).toByte()
            off += n
        }
        return out
    }
}

object Poly1305 {
    private val P: BigInteger = BigInteger.ONE.shiftLeft(130).subtract(BigInteger.valueOf(5))
    private val MOD128: BigInteger = BigInteger.ONE.shiftLeft(128)
    private val CLAMP = BigInteger("0ffffffc0ffffffc0ffffffc0fffffff", 16)

    private fun leNum(b: ByteArray, off: Int, len: Int): BigInteger {
        val be = ByteArray(len + 1)                     // leading 0: always positive
        for (i in 0 until len) be[len - i] = b[off + i]
        return BigInteger(be)
    }

    /** The 16-byte tag of [msg] under the one-time 32-byte [key] (RFC 8439 2.5). */
    fun mac(key: ByteArray, msg: ByteArray): ByteArray {
        require(key.size == 32) { "Poly1305 key must be 32 bytes" }
        val r = leNum(key, 0, 16).and(CLAMP)
        val s = leNum(key, 16, 16)
        var acc = BigInteger.ZERO
        var off = 0
        while (off < msg.size) {
            val n = minOf(16, msg.size - off)
            val block = leNum(msg, off, n).setBit(8 * n)
            acc = acc.add(block).multiply(r).mod(P)
            off += n
        }
        acc = acc.add(s).mod(MOD128)
        val be = acc.toByteArray()                       // big-endian, maybe with a sign byte
        val out = ByteArray(16)
        for (i in 0 until 16) {
            val j = be.size - 1 - i
            out[i] = if (j >= 0) be[j] else 0
        }
        return out
    }
}

object ChaCha20Poly1305 {
    private fun pad16(n: Int) = (16 - n % 16) % 16

    private fun le64(v: Long) = ByteArray(8) { (v ushr (8 * it)).toByte() }

    /** Returns ciphertext followed by the full 16-byte tag (RFC 8439 2.8). */
    fun seal(key: ByteArray, nonce: ByteArray, plaintext: ByteArray, aad: ByteArray): ByteArray {
        val otk = ChaCha20.block(key, 0, nonce).copyOf(32)
        val ct = ChaCha20.xor(key, 1, nonce, plaintext)
        val macData = aad + ByteArray(pad16(aad.size)) + ct + ByteArray(pad16(ct.size)) +
            le64(aad.size.toLong()) + le64(ct.size.toLong())
        return ct + Poly1305.mac(otk, macData)
    }

    /**
     * The other side of [seal], with the full 16-byte tag the admin link uses (D70): returns the
     * plaintext, or null when the tag does not match. The tag is compared in constant time and
     * nothing is decrypted before it matches.
     */
    fun open(key: ByteArray, nonce: ByteArray, sealed: ByteArray, aad: ByteArray): ByteArray? {
        if (sealed.size < TAG) return null
        val ct = sealed.copyOfRange(0, sealed.size - TAG)
        val tag = sealed.copyOfRange(sealed.size - TAG, sealed.size)
        val otk = ChaCha20.block(key, 0, nonce).copyOf(32)
        val macData = aad + ByteArray(pad16(aad.size)) + ct + ByteArray(pad16(ct.size)) +
            le64(aad.size.toLong()) + le64(ct.size.toLong())
        if (!MessageDigest.isEqual(Poly1305.mac(otk, macData), tag)) return null
        return ChaCha20.xor(key, 1, nonce, ct)
    }

    const val TAG = 16
}
