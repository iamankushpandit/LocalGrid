package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Test
import org.localgrid.gridwatch.crypto.ChaCha20
import org.localgrid.gridwatch.crypto.ChaCha20Poly1305
import org.localgrid.gridwatch.crypto.Poly1305

/** RFC 8439 test vectors for every primitive the app uses. */
class CryptoRfcTest {
    private val sunscreen = ("Ladies and Gentlemen of the class of '99: If I could offer you only one tip " +
        "for the future, sunscreen would be it.").toByteArray(Charsets.US_ASCII)
    private val key0 = ByteArray(32) { it.toByte() }

    @Test fun chacha20Block_rfc8439_2_3_2() {
        val out = ChaCha20.block(key0, 1, hex("000000090000004a00000000"))
        assertEquals("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e" +
            "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", out.toHex())
    }

    @Test fun chacha20Encrypt_rfc8439_2_4_2() {
        val ct = ChaCha20.xor(key0, 1, hex("000000000000004a00000000"), sunscreen)
        assertEquals("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b" +
            "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8" +
            "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736" +
            "5af90bbf74a35be6b40b8eedf2785e42874d", ct.toHex())
        // Decrypting is the same operation.
        assertEquals(sunscreen.toHex(), ChaCha20.xor(key0, 1, hex("000000000000004a00000000"), ct).toHex())
    }

    @Test fun poly1305_rfc8439_2_5_2() {
        val tag = Poly1305.mac(hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
            "Cryptographic Forum Research Group".toByteArray(Charsets.US_ASCII))
        assertEquals("a8061dc1305136c6c22b8baf0c0127a9", tag.toHex())
    }

    @Test fun aead_rfc8439_2_8_2() {
        val key = ByteArray(32) { (0x80 + it).toByte() }
        val sealed = ChaCha20Poly1305.seal(key, hex("070000004041424344454647"), sunscreen,
            hex("50515253c0c1c2c3c4c5c6c7"))
        assertEquals("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6" +
            "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36" +
            "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc" +
            "3ff4def08e4b7a9de576d26586cec64b6116", sealed.copyOfRange(0, sunscreen.size).toHex())
        assertEquals("1ae10b594f09e26a7e902ecbd0600691", sealed.copyOfRange(sunscreen.size, sealed.size).toHex())
    }

    @Test fun poly1305_emptyAndFullBlocks() {
        // A tag over nothing is s itself; a key with r = 0 gives s for any message.
        val k = ByteArray(32).also { for (i in 16 until 32) it[i] = (i * 7).toByte() }
        assertEquals(k.copyOfRange(16, 32).toHex(), Poly1305.mac(k, ByteArray(0)).toHex())
        assertEquals(k.copyOfRange(16, 32).toHex(), Poly1305.mac(k, ByteArray(48) { 0xFF.toByte() }).toHex())
    }
}
