package org.localgrid.gridwatch.crypto

import javax.crypto.Mac
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/**
 * HMAC-SHA256, HKDF-SHA256 and PBKDF2-HMAC-SHA256, the three the admin link needs (D70).
 *
 * The link key is HKDF(salt "LG-BLE-LINK-1", ikm = K, info = "admin link"); logging in hashes
 * the admin password with PBKDF2 exactly as the AP stored it, then proves it with one HMAC. All
 * three come from the platform's own JCA, which has them on every Android this app supports.
 */
object Kdf {
    private const val HMAC = "HmacSHA256"
    private const val LEN = 32

    fun hmacSha256(key: ByteArray, msg: ByteArray): ByteArray {
        val mac = Mac.getInstance(HMAC)
        mac.init(SecretKeySpec(if (key.isEmpty()) ByteArray(1) else key, HMAC))
        return mac.doFinal(msg)
    }

    /** RFC 5869 extract-and-expand, for output lengths up to 255 * 32 bytes. */
    fun hkdfSha256(salt: ByteArray, ikm: ByteArray, info: ByteArray, length: Int = LEN): ByteArray {
        require(length in 1..(255 * LEN)) { "HKDF length out of range" }
        val prk = hmacSha256(salt, ikm)
        val out = ByteArray(length)
        var t = ByteArray(0)
        var done = 0
        var counter = 1
        while (done < length) {
            t = hmacSha256(prk, t + info + byteArrayOf(counter.toByte()))
            val n = minOf(LEN, length - done)
            System.arraycopy(t, 0, out, done, n)
            done += n
            counter++
        }
        return out
    }

    /** What the AP stores for the admin password. The password is never kept in a String here. */
    fun pbkdf2Sha256(password: CharArray, salt: ByteArray, iterations: Int, length: Int = LEN): ByteArray {
        val spec = PBEKeySpec(password, salt, iterations, length * 8)
        try {
            return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        } finally {
            spec.clearPassword()
        }
    }
}
