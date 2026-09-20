package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.localgrid.gridwatch.link.ApChoice
import org.localgrid.gridwatch.link.GattStatus
import org.localgrid.gridwatch.link.LinkException
import org.localgrid.gridwatch.link.LinkProtocol
import org.localgrid.gridwatch.link.LinkSession

/**
 * The bug of 2026-09-20 on the owner's phone: every attempt on AP 2 ended with "the AP did not
 * answer in time, it may be busy with another watcher", while the AP was demonstrably fine.
 *
 * Two faults made that message. The link dropped after the subscription — the MTU had stayed at
 * Android's 23-byte default, which the AP refuses because a chunk could carry no body — and
 * nothing woke the code waiting for a reply, so it sat out the twelve seconds and then blamed
 * the AP for being busy. "Busy" is ERROR code 2 from the AP and must never be a guess.
 */
class LinkFailureTest {
    private val kLink = Vectors.bytes("k_link")

    @Test fun aDroppedLinkWakesTheWaitWithTheRealReason() {
        val session = LinkSession(kLink) { }
        session.feed(Vectors.notifications[0])                    // SESSION
        session.request(LinkProtocol.OP_HELLO, byteArrayOf(1))
        // The AP closes the link, as it does when the watcher's MTU is too small.
        session.fail(GattStatus.disconnectReason("SOUTH", GattStatus.CONNECTION_TERMINATED_BY_PEER))
        val started = System.currentTimeMillis()
        try {
            session.reply()
            fail("a dropped link must raise, not wait out the timeout")
        } catch (e: LinkException) {
            assertEquals("SOUTH's link dropped: the AP closed the link (19)", e.message)
        }
        // And it must come back at once, not after twelve seconds.
        assertTrue("it should not have waited", System.currentTimeMillis() - started < 2000)
    }

    @Test fun aRealSilenceNoLongerBlamesTheAp() {
        val session = LinkSession(kLink) { }
        session.feed(Vectors.notifications[0])
        try {
            session.reply(timeoutMs = 50)
            fail("silence must raise")
        } catch (e: LinkException) {
            assertFalse("only the AP may say it is busy", e.message!!.contains("busy"))
            assertTrue(e.message!!.contains("did not drop"))
        }
    }

    @Test fun onlyTheApItselfSaysBusy() {
        val session = LinkSession(kLink) { }
        session.feed(Vectors.notifications[0])
        // ERROR code 2 with the AP's own words is the one place "busy" comes from.
        session.feed(
            LinkProtocol.seal(
                kLink, LinkProtocol.DIR_TO_CLIENT, Vectors.int("session"), 0,
                LinkProtocol.OP_ERROR, 0, byteArrayOf(2) + "busy, ask again".toByteArray(),
            ),
        )
        try {
            session.reply()
            fail("an ERROR reply must raise")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("busy, ask again"))
        }
    }

    @Test fun everyFailureSaysWhatHappenedAndKeepsTheNumber() {
        // The ones Android actually hands back, each recognisable and each carrying its code.
        assertTrue(GattStatus.describe(8).contains("timed out"))
        assertTrue(GattStatus.describe(19).contains("the AP closed"))
        assertTrue(GattStatus.describe(22).contains("this phone closed"))
        assertTrue(GattStatus.describe(62).contains("never got established"))
        assertTrue(GattStatus.describe(133).contains("busy radio"))
        for (status in listOf(0, 1, 5, 6, 8, 15, 19, 22, 34, 62, 133, 256, 999)) {
            assertTrue(
                "status $status must name its number: ${GattStatus.describe(status)}",
                GattStatus.describe(status).contains(status.toString()),
            )
        }
        // None of them may claim another watcher is holding the AP.
        for (status in 0..300) assertFalse(GattStatus.describe(status).contains("another watcher"))
    }

    @Test fun anMtuTooSmallSaysSoInNumbers() {
        val text = GattStatus.mtuTooSmall("SOUTH", 23)
        assertTrue(text.contains("23"))
        assertTrue(text.contains("64"))                          // LinkProtocol.MTU_MIN
        assertTrue(text.contains("radio was busy"))
    }

    @Test fun aDisconnectWithNoErrorIsNotCalledAFailure() {
        assertEquals("SOUTH closed the link", GattStatus.disconnectReason("SOUTH", GattStatus.SUCCESS))
    }

    // -- which AP to ask, and in what order

    private fun heard(ap: Int, rssi: Int, ageMs: Long = 0) = ApChoice.Heard(ap, rssi, ageMs)

    @Test fun theLoudestApIsTriedFirst() {
        val order = ApChoice.order(listOf(heard(0, -80), heard(2, -55), heard(1, -70)))
        assertEquals(listOf(2, 1, 0), order)
    }

    @Test fun anApNotHeardLatelyIsNotWorthTrying() {
        val order = ApChoice.order(listOf(heard(0, -50, ageMs = 60_000), heard(1, -90, ageMs = 500)))
        assertEquals(listOf(1), order)
    }

    @Test fun equalStrengthPrefersTheOneHeardMoreRecently() {
        val order = ApChoice.order(listOf(heard(1, -60, ageMs = 9_000), heard(2, -60, ageMs = 200)))
        assertEquals(listOf(2, 1), order)
    }

    @Test fun everyApHeardIsTried() {
        val all = listOf(heard(0, -80), heard(1, -70), heard(2, -60))
        assertEquals(3, ApChoice.order(all).size)
    }

    @Test fun theErrorNamesWhatHappenedToEachAp() {
        assertEquals("No AP of this grid is in range to ask.", ApChoice.summarise(emptyList()))
        assertEquals(
            "SOUTH: the connection timed out: the AP went out of range or stopped answering (8)",
            ApChoice.summarise(listOf("SOUTH" to GattStatus.describe(8))),
        )
        val many = ApChoice.summarise(
            listOf("SOUTH" to "MTU 23 is too small", "NORTH" to "no admin link service"),
        )
        assertTrue(many.startsWith("No AP answered."))
        assertTrue(many.contains("SOUTH: MTU 23 is too small"))
        assertTrue(many.contains("NORTH: no admin link service"))
    }
}
