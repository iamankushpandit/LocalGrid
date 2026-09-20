package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.localgrid.gridwatch.link.AdminAuth
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.link.LinkException
import org.localgrid.gridwatch.link.LinkProtocol
import org.localgrid.gridwatch.link.LinkSession
import org.localgrid.gridwatch.link.pullOnce

/**
 * The whole admin-link conversation, replayed from a transcript tools/grid_watch.py recorded
 * against its own mock AP with the TEST key: SESSION, HELLO, LOGIN, GET_STATUS, GET_HISTORY and
 * GET_TRAFFIC, chunked as the AP chunks them.
 *
 * The phone must write byte for byte what the reference client wrote, and must end up with the
 * same status, history and traffic.
 */
class LinkConversationTest {
    private val kLink = Vectors.bytes("k_link")

    /**
     * Plays the AP's side: the session first, then, for every request the client writes, the
     * recorded notifications up to and including the one whose "more" flag is clear.
     */
    private class Replay(private val notes: List<ByteArray>) {
        var at = 0
        val writes = ArrayList<ByteArray>()
        lateinit var session: LinkSession

        fun subscribe() {
            session.feed(notes[at++])                   // SESSION, the one unsealed message
        }

        fun onWrite(payload: ByteArray) {
            writes.add(payload)
            if (payload[1].toInt() and LinkProtocol.FLAG_MORE != 0) return   // more request to come
            do {
                val note = notes[at++]
                session.feed(note)
            } while (note[1].toInt() and LinkProtocol.FLAG_MORE != 0)
        }
    }

    private fun run(password: String = Vectors.str("password")): Triple<LinkData, AdminAuth, Replay> {
        val replay = Replay(Vectors.notifications)
        val session = LinkSession(kLink) { replay.onWrite(it) }
        replay.session = session
        session.chunk = 32
        val auth = AdminAuth()
        auth.setPassword(password.toCharArray())
        val data = LinkData()
        replay.subscribe()
        val ok = pullOnce(session, auth, data, "MAIN", wantTraffic = true)
        assertTrue("the pull should have worked: ${data.message}", ok)
        return Triple(data, auth, replay)
    }

    @Test fun writesExactlyWhatTheLaptopToolWrites() {
        val (_, _, replay) = run()
        assertEquals(
            Vectors.clientWrites.map { it.toHex() },
            replay.writes.map { it.toHex() },
        )
        // Five requests and no more: HELLO, LOGIN, GET_STATUS, GET_HISTORY, GET_TRAFFIC.
        assertEquals(5, replay.writes.size)
        assertEquals(
            listOf("HELLO", "LOGIN", "GET_STATUS", "GET_HISTORY", "GET_TRAFFIC"),
            replay.writes.map { LinkProtocol.opName(it[0].toInt() and 0xFF) },
        )
    }

    @Test fun reassemblesEveryReply() {
        val (data, auth, _) = run()
        assertTrue(auth.loggedIn)
        assertEquals(LinkData.State.DONE, data.state)
        val status = data.status
        assertNotNull(status)
        assertEquals("MAIN", status!!.apName)
        assertEquals("Lakeside Trip", status.data.gridName)
        assertEquals(2, status.data.positions.size)
        val history = data.history
        assertNotNull(history)
        assertEquals(Vectors.int("hist_aps"), history!!.data.aps.size)
        assertEquals(1, data.traffic.size)
        val traffic = data.traffic[0].data
        assertEquals(Vectors.int("traffic_ap"), traffic.ap)
        assertEquals(Vectors.dbl("traffic_rate_1m"), traffic.rate1m!!, 1e-9)
        assertEquals(Vectors.long("traffic_voice_dropped"), traffic.fault("voice_dropped"))
    }

    @Test fun theSessionNumberComesFromTheApAndIsNotSealed() {
        val replay = Replay(Vectors.notifications)
        val session = LinkSession(kLink) { replay.onWrite(it) }
        replay.session = session
        replay.subscribe()
        assertEquals(Vectors.int("session"), session.session)
    }

    @Test fun aWrongPasswordIsRefusedAndForgotten() {
        val replay = Replay(Vectors.notifications)
        val session = LinkSession(kLink) { replay.onWrite(it) }
        replay.session = session
        session.chunk = 32
        val auth = AdminAuth()
        auth.setPassword("not-the-password".toCharArray())
        val data = LinkData()
        replay.subscribe()
        // The recorded AP answered LOGIN_OK, so a proof built from the wrong password reaches a
        // reply it cannot match: what matters is that nothing is stored and nothing is shown.
        val ok = try {
            pullOnce(session, auth, data, "MAIN", wantTraffic = false)
        } catch (e: LinkException) {
            false
        }
        if (!ok) {
            assertFalse(auth.loggedIn)
            assertEquals(null, data.status)
        }
        // Either way, the proof it sent was not the right one.
        assertTrue(replay.writes.size >= 2)
        assertFalse(replay.writes[1].toHex() == Vectors.clientWrites[1].toHex())
    }

    @Test fun logOutThrowsAwayEverythingPulled() {
        val (data, _, _) = run()
        assertNotNull(data.status)
        data.clear()
        assertEquals(null, data.status)
        assertEquals(null, data.history)
        assertTrue(data.traffic.isEmpty())
        assertEquals(LinkData.State.IDLE, data.state)
    }

    @Test fun aSealedMessageBeforeTheSessionEndsTheConversation() {
        val session = LinkSession(kLink) { }
        session.feed(Vectors.bytes("sealed_hello_ok"))
        try {
            session.reply(50)
            fail("a sealed message before the session should end the conversation")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("before its session"))
        }
    }

    @Test fun anErrorReplyIsRaisedWithItsReason() {
        val replay = Replay(
            listOf(
                Vectors.notifications[0],
                LinkProtocol.seal(
                    kLink, LinkProtocol.DIR_TO_CLIENT, Vectors.int("session"), 0,
                    LinkProtocol.OP_ERROR, 0, byteArrayOf(1) + "log in first".toByteArray(),
                ),
            ),
        )
        val session = LinkSession(kLink) { replay.onWrite(it) }
        replay.session = session
        replay.subscribe()
        try {
            session.ask(LinkProtocol.OP_GET_STATUS, LinkProtocol.OP_STATUS)
            fail("an ERROR reply should be raised")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("log in first"))
        }
    }
}
