package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.crypto.Kdf
import org.localgrid.gridwatch.link.AdminAuth
import org.localgrid.gridwatch.link.LinkProtocol

/**
 * Logging in (docs/ble-link.md): PBKDF2-HMAC-SHA256 makes the hash the AP already stores, and one
 * HMAC over the AP's challenge proves it. The password itself never crosses the link, so the
 * whole test is about matching the laptop tool's arithmetic exactly.
 */
class LoginProofTest {
    private val salt = Vectors.bytes("salt")
    private val iterations = Vectors.int("iterations")
    private val challenge = Vectors.bytes("challenge")
    private val password = Vectors.str("password")

    @Test fun pbkdf2MatchesTheLaptopTool() {
        assertEquals(
            Vectors.str("login_hash"),
            LinkProtocol.loginHash(password.toCharArray(), salt, iterations).toHex(),
        )
    }

    @Test fun proofMatchesTheLaptopTool() {
        val hash = Vectors.bytes("login_hash")
        assertEquals(Vectors.str("login_proof"), LinkProtocol.loginProof(hash, challenge).toHex())
    }

    @Test fun theProofIsHmacOverChallengeAndContext() {
        val hash = Vectors.bytes("login_hash")
        assertEquals(
            Kdf.hmacSha256(hash, challenge + "lg-ble-admin".toByteArray()).toHex(),
            LinkProtocol.loginProof(hash, challenge).toHex(),
        )
    }

    @Test fun anotherChallengeGivesAnotherProof() {
        val hash = Vectors.bytes("login_hash")
        val other = challenge.copyOf().also { it[0] = (it[0] + 1).toByte() }
        assertNotEquals(
            LinkProtocol.loginProof(hash, challenge).toHex(),
            LinkProtocol.loginProof(hash, other).toHex(),
        )
    }

    @Test fun authHoldsNoPasswordUntilOneIsTyped() {
        val auth = AdminAuth()
        assertFalse(auth.havePassword())
        assertNull(auth.proof(salt, iterations, challenge))
        assertEquals(AdminAuth.State.NONE, auth.state)
    }

    @Test fun authMakesTheSameProofAndForgetsAWrongPassword() {
        val auth = AdminAuth()
        auth.setPassword(password.toCharArray())
        assertTrue(auth.havePassword())
        assertEquals(Vectors.str("login_proof"), auth.proof(salt, iterations, challenge)!!.toHex())
        // The cached hash is reused for the same salt and iterations, and only then.
        assertEquals(Vectors.str("login_proof"), auth.proof(salt, iterations, challenge)!!.toHex())
        auth.wrongPassword("Wrong admin password.")
        assertFalse(auth.havePassword())
        assertNull(auth.proof(salt, iterations, challenge))
        assertEquals(AdminAuth.State.FAILED, auth.state)
    }

    @Test fun loggingOutForgetsEverything() {
        val auth = AdminAuth()
        auth.setPassword(password.toCharArray(), remembered = true)
        auth.result(true, "Logged in.")
        assertTrue(auth.loggedIn)
        auth.forget()
        assertFalse(auth.loggedIn)
        assertFalse(auth.havePassword())
        assertFalse(auth.remembered)
    }

    @Test fun hkdfMatchesTheLaptopTool() {
        assertEquals(
            Vectors.str("k_link"),
            Kdf.hkdfSha256("LG-BLE-LINK-1".toByteArray(), Vectors.bytes("k"), "admin link".toByteArray()).toHex(),
        )
    }
}
