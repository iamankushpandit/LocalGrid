package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.localgrid.gridwatch.link.ChunkJoiner
import org.localgrid.gridwatch.link.LinkException
import org.localgrid.gridwatch.link.LinkProtocol
import org.junit.Test

/**
 * The sealed framing of docs/ble-link.md, against bytes tools/grid_watch.py sealed with the TEST
 * key. Anything that does not add up — a changed tag, a counter that repeats, the wrong session
 * or the wrong direction — must refuse to open, because on the real link it closes the
 * connection.
 */
class LinkProtocolTest {
    private val k = Vectors.bytes("k")
    private val kLink = Vectors.bytes("k_link")
    private val session = Vectors.int("session")

    @Test fun linkKeyMatchesTheLaptopTool() {
        assertEquals(kLink.toHex(), LinkProtocol.linkKey(k).toHex())
    }

    @Test fun nonceIsDirSessionCounter() {
        // dir, session u16 LE, 0, counter u32 LE, four zero bytes.
        assertEquals("013a9c000700000000000000", LinkProtocol.nonce(1, 0x9C3A, 7).toHex())
        assertEquals("003a9c000000000000000000", LinkProtocol.nonce(0, 0x9C3A, 0).toHex())
    }

    @Test fun sealsTheSameBytesAsTheLaptopTool() {
        assertEquals(
            Vectors.str("sealed_hello"),
            LinkProtocol.seal(kLink, LinkProtocol.DIR_TO_AP, session, 0, LinkProtocol.OP_HELLO, 0,
                byteArrayOf(1)).toHex(),
        )
        assertEquals(
            Vectors.str("sealed_get_status_c7"),
            LinkProtocol.seal(kLink, LinkProtocol.DIR_TO_AP, session, 7, LinkProtocol.OP_GET_STATUS, 0,
                ByteArray(0)).toHex(),
        )
    }

    @Test fun opensTheApsHelloOk() {
        val m = LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, Vectors.bytes("sealed_hello_ok"))
        assertEquals(LinkProtocol.OP_HELLO_OK, m.opcode)
        assertEquals(Vectors.str("hello_ok_body"), m.body.toHex())
        val hello = LinkProtocol.parseHelloOk(m.body)
        assertEquals(1, hello.version)
        assertEquals(0, hello.ap)
        assertEquals(41L, hello.boot)
        assertTrue(hello.passwordSet)
        assertEquals(Vectors.str("salt"), hello.salt.toHex())
        assertEquals(Vectors.int("iterations"), hello.iterations)
        assertEquals(Vectors.str("challenge"), hello.challenge.toHex())
    }

    private fun expectRefusal(what: String, body: () -> Unit) {
        try {
            body()
            fail("$what should have been refused")
        } catch (e: LinkException) {
            assertTrue(e.message!!.isNotEmpty())
        }
    }

    @Test fun refusesABadTag() {
        val sealed = Vectors.bytes("sealed_hello_ok")
        sealed[sealed.size - 1] = (sealed[sealed.size - 1].toInt() xor 1).toByte()
        expectRefusal("a changed tag") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, sealed)
        }
    }

    @Test fun refusesAChangedHeader() {
        // The header is in the clear but it is the AAD, so changing it fails the tag.
        val sealed = Vectors.bytes("sealed_hello_ok")
        sealed[0] = LinkProtocol.OP_STATUS.toByte()
        expectRefusal("a changed header") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, sealed)
        }
    }

    @Test fun refusesAReplayedCounter() {
        val first = Vectors.bytes("sealed_hello_ok")
        LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, first)   // counter 0 opens
        expectRefusal("the same message again at counter 1") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 1, first)
        }
    }

    @Test fun refusesTheWrongSession() {
        expectRefusal("another connection's session") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session + 1, 0, Vectors.bytes("sealed_hello_ok"))
        }
    }

    @Test fun refusesTheWrongDirection() {
        // A reply opened as if the watcher had sent it: the direction byte is in the nonce.
        expectRefusal("a reply read as a request") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_AP, session, 0, Vectors.bytes("sealed_hello_ok"))
        }
    }

    @Test fun refusesALengthThatDisagreesWithTheHeader() {
        val sealed = Vectors.bytes("sealed_hello_ok")
        sealed[2] = (sealed[2].toInt() + 1).toByte()
        expectRefusal("a header claiming another length") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, sealed)
        }
    }

    @Test fun refusesAMessageShorterThanAHeaderAndATag() {
        expectRefusal("a runt message") {
            LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, 0, ByteArray(9))
        }
    }

    @Test fun joinsChunksInOrder() {
        val joiner = ChunkJoiner()
        var out: Pair<Int, ByteArray>? = null
        Vectors.list("chunked_reply").forEachIndexed { i, chunk ->
            val m = LinkProtocol.open(kLink, LinkProtocol.DIR_TO_CLIENT, session, i.toLong(), chunk)
            assertEquals(LinkProtocol.OP_STATUS, m.opcode)
            val done = joiner.add(m.opcode, m.flags, m.body)
            if (done != null) out = done
        }
        assertEquals(LinkProtocol.OP_STATUS, out!!.first)
        assertEquals(Vectors.str("chunked_body"), out!!.second.toHex())
    }

    @Test fun refusesAReplyThatChangesOpcodeHalfway() {
        val joiner = ChunkJoiner()
        assertNull(joiner.add(LinkProtocol.OP_STATUS, LinkProtocol.FLAG_MORE, byteArrayOf(1, 2)))
        expectRefusal("a second opcode in one reply") {
            joiner.add(LinkProtocol.OP_HISTORY, 0, byteArrayOf(3))
        }
    }

    @Test fun splitsTheWayTheApExpects() {
        val body = ByteArray(70) { it.toByte() }
        val parts = LinkProtocol.splitChunks(body, 32)
        assertEquals(3, parts.size)
        assertEquals(LinkProtocol.FLAG_MORE, parts[0].first)
        assertEquals(LinkProtocol.FLAG_MORE, parts[1].first)
        assertEquals(0, parts[2].first)
        assertEquals(6, parts[2].second.size)
        assertEquals(body.toHex(), parts.joinToString("") { it.second.toHex() })
        // An empty request still sends one final chunk, as GET_STATUS does.
        val empty = LinkProtocol.splitChunks(ByteArray(0), 32)
        assertEquals(1, empty.size)
        assertEquals(0, empty[0].first)
        assertEquals(0, empty[0].second.size)
    }

    @Test fun readOnlyOpcodesOnly() {
        // Every opcode this app can send. None writes, none carries message text, none audio.
        val canSend = listOf(
            LinkProtocol.OP_HELLO, LinkProtocol.OP_LOGIN, LinkProtocol.OP_GET_STATUS,
            LinkProtocol.OP_GET_HISTORY, LinkProtocol.OP_GET_TRAFFIC,
        )
        assertEquals(listOf("HELLO", "LOGIN", "GET_STATUS", "GET_HISTORY", "GET_TRAFFIC"),
            canSend.map { LinkProtocol.opName(it) })
        assertNotEquals(LinkProtocol.OP_HELLO, LinkProtocol.OP_SESSION)
    }
}
