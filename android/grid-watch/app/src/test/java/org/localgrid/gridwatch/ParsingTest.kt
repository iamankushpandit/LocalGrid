package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.proto.AdParser
import org.localgrid.gridwatch.proto.Discovery
import org.localgrid.gridwatch.proto.PairingCode

class ParsingTest {
    private val status = hex("53116d0100000a00008469d80d4cafe1f42a5401c3ebc4")
    // 'L' 'G', version 1, discriminator 11 22 33 44, AP 1, reserved, 13 free, flags 3, 2 attached
    private val disc = "LG".toByteArray() + hex("01112233440100" + "0d0302")

    private fun manufacturer(payload: ByteArray) =
        byteArrayOf((3 + payload.size).toByte(), 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte()) + payload

    /** Android's merged record: advert (flags + discovery), zero padding, then the scan response. */
    @Test fun bothManufacturerEntriesUnderOneCompanyId() {
        val advert = hex("020106") + manufacturer(disc)
        val scanResp = manufacturer(status)
        val record = advert + ByteArray(31 - advert.size) + scanResp + ByteArray(31 - scanResp.size)
        val got = AdParser.manufacturerPayloads(record, 0xFFFF)
        assertEquals(2, got.size)
        assertEquals(disc.toHex(), got[0].toHex())
        assertEquals(status.toHex(), got[1].toHex())
        assertEquals(0, AdParser.manufacturerPayloads(record, 0x02E5).size)
        val d = Discovery.parse(got[0])!!
        assertEquals(1, d.ap)
        assertEquals("11223344", d.disc.toHex())
        assertEquals(2, d.attached)
        assertEquals(13, d.freeSlots)
    }

    @Test fun truncatedOrEmptyRecordsAreSafe() {
        assertTrue(AdParser.manufacturerPayloads(hex("1effffff53"), 0xFFFF).isEmpty())   // claims 30, has 4
        assertTrue(AdParser.manufacturerPayloads(null, 0xFFFF).isEmpty())
        assertTrue(AdParser.manufacturerPayloads(ByteArray(62), 0xFFFF).isEmpty())
    }

    @Test fun discoveryWithNameTail() {
        assertEquals("MAIN", Discovery.parse(disc + byteArrayOf(4) + "MAIN".toByteArray())!!.name)
        assertNull(Discovery.parse(disc + byteArrayOf(9) + "MAIN".toByteArray())!!.name)   // bad length
        assertNull(Discovery.parse(status))
        assertNull(Discovery.parse(disc.copyOf(11)))
    }

    @Test fun pairingCodeRejections() {
        val good = "LGW1:__8RIjNEgRlfj5TmygEq9cZ28DcQgb4lVmRxrA_tQFt3OiddaEc"
        assertTrue(PairingCode.parse(good) is PairingCode.Ok)
        assertTrue(PairingCode.parse("  $good\n") is PairingCode.Ok)                    // pasted
        assertTrue(PairingCode.parse(good.replace("LGW1:", "LGW2:")) is PairingCode.Bad)
        assertTrue(PairingCode.parse(good.dropLast(2)) is PairingCode.Bad)
        assertTrue(PairingCode.parse(good + "AA") is PairingCode.Bad)
        assertTrue(PairingCode.parse(good.replace('_', '+')) is PairingCode.Bad)       // not base64url
        assertTrue(PairingCode.parse("") is PairingCode.Bad)
        assertTrue(PairingCode.parse("https://example.org/") is PairingCode.Bad)
    }
}
