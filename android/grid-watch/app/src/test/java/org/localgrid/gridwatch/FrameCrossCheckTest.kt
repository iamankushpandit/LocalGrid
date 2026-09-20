package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.proto.PairingCode
import org.localgrid.gridwatch.proto.StatusFrame
import org.localgrid.gridwatch.proto.StatusFrame.Alert
import org.localgrid.gridwatch.proto.StatusFrame.Batteries
import org.localgrid.gridwatch.proto.StatusFrame.Handhelds
import org.localgrid.gridwatch.proto.StatusFrame.Health
import org.localgrid.gridwatch.proto.StatusFrame.Name

/**
 * Frames sealed by tools/grid_watch.py's own seal_frame() with a TEST key (the self-check's
 * backbone bytes 0..31 through its HKDF; never a real lg_secrets.h). The Kotlin decoder must
 * open them and read the same fields grid_watch.py's decode_body() printed.
 */
class FrameCrossCheckTest {
    // grid_watch.derive_key(bytes(range(32)))
    private val k = hex("81195f8f94e6ca012af5c676f0371081be25566471ac0fed405b773a275d6847")
    // The same K as a pairing code: company ID 0xFFFF, discriminator 11 22 33 44.
    private val code = "LGW1:__8RIjNEgRlfj5TmygEq9cZ28DcQgb4lVmRxrA_tQFt3OiddaEc"

    private fun open(frameHex: String): StatusFrame.Frame {
        val r = StatusFrame.open(k, hex(frameHex))
        assertTrue("frame should open: $r", r is StatusFrame.Frame)
        return r as StatusFrame.Frame
    }

    @Test fun pairingCodeFromPythonCarriesK() {
        val r = PairingCode.parse(code) as PairingCode.Ok
        assertEquals(0xFFFF, r.pairing.companyId)
        assertEquals("11223344", r.pairing.discriminator.toHex())
        assertEquals(k.toHex(), r.pairing.key.toHex())
        assertEquals(code, r.pairing.toCode())
    }

    @Test fun health() {
        val f = open("53116d0100000a00008469d80d4cafe1f42a5401c3ebc4")
        assertEquals(1, f.ap); assertEquals(StatusFrame.T_HEALTH, f.type); assertEquals(365L, f.boot); assertEquals(10, f.counter)
        assertEquals(Health(uptimeMin = 300, links = 5, handheldsHere = 3, timeQuality = 2, timeGps = true, stratum = 2,
            heapKb = 87, resetReason = 9, restarts = 12, brownouts = 4, gpsFitted = true, gpsFix = true, gpsSats = 7),
            StatusFrame.decode(f.type, f.plaintext))
    }

    @Test fun handhelds() {
        val f = open("53216d0100000b0000dff696615fcdf5f7cee279b4dbbeb0946d74")
        val b = StatusFrame.decode(f.type, f.plaintext) as Handhelds
        assertEquals(setOf(1, 3, 4), b.online)
        assertEquals(1, b.where[1]); assertNull(b.where[2]); assertEquals(2, b.where[3]); assertEquals(1, b.where[4])
        assertTrue((5..20).all { b.where[it] == null })
    }

    @Test fun batteries() {
        val f = open("53416d0100000c000024e28b0b2801bb431cf6")
        assertEquals(Batteries(mapOf(1 to 80, 3 to 15, 4 to 255)), StatusFrame.decode(f.type, f.plaintext))
    }

    @Test fun nameUtf8() {
        val f = open("53516d0100000d0000ca924961c27fe3ce551cd9ca4b01c113")
        assertEquals(Name(3, "Priya été"), StatusFrame.decode(f.type, f.plaintext))
    }

    @Test fun alert() {
        val f = open("53316d0100000e0000b0c698fad54e9806551d1a35cde7")
        assertEquals(Alert(active = true, allClear = false, author = 3, ageS = 120, reads = 2, gridTime = 1_789_700_000L),
            StatusFrame.decode(f.type, f.plaintext))
    }

    @Test fun emptyBatteryFrameFromAp2() {
        val f = open("53426d01000007000038d774e1")
        assertEquals(2, f.ap); assertEquals(7, f.counter)
        assertEquals(Batteries(emptyMap()), StatusFrame.decode(f.type, f.plaintext))
    }

    @Test fun kotlinSealMatchesPythonSeal() {
        val pt = hex("2c0105031657090c041f")
        assertEquals("53116d0100000a00008469d80d4cafe1f42a5401c3ebc4",
            StatusFrame.seal(k, 1, StatusFrame.T_HEALTH, 365, 10, pt).toHex())
    }
}
