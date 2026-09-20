package org.localgrid.gridwatch.link

import org.localgrid.gridwatch.link.LinkProtocol.DIR_TO_AP
import org.localgrid.gridwatch.link.LinkProtocol.DIR_TO_CLIENT
import org.localgrid.gridwatch.link.LinkProtocol.FLAG_MORE
import org.localgrid.gridwatch.link.LinkProtocol.HEADER
import org.localgrid.gridwatch.link.LinkProtocol.MAX_REPLY
import org.localgrid.gridwatch.link.LinkProtocol.OP_ERROR
import org.localgrid.gridwatch.link.LinkProtocol.OP_HELLO
import org.localgrid.gridwatch.link.LinkProtocol.OP_HELLO_OK
import org.localgrid.gridwatch.link.LinkProtocol.OP_LOGIN
import org.localgrid.gridwatch.link.LinkProtocol.OP_LOGIN_FAIL
import org.localgrid.gridwatch.link.LinkProtocol.OP_LOGIN_OK
import org.localgrid.gridwatch.link.LinkProtocol.OP_SESSION
import java.io.ByteArrayOutputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/** Joins a reply's chunks. Each chunk is its own sealed message; bit 0 of its flags means more. */
class ChunkJoiner {
    private var opcode: Int? = null
    private val parts = ByteArrayOutputStream()

    /** Returns the whole reply when the last chunk arrives, else null. */
    fun add(opcode: Int, flags: Int, body: ByteArray): Pair<Int, ByteArray>? {
        val current = this.opcode
        if (current != null && opcode != current) {
            throw LinkException("a reply changed opcode halfway through its chunks")
        }
        this.opcode = opcode
        parts.write(body)
        if (parts.size() > MAX_REPLY) throw LinkException("a reply longer than this app accepts")
        if (flags and FLAG_MORE != 0) return null
        val out = opcode to parts.toByteArray()
        this.opcode = null
        parts.reset()
        return out
    }
}

/**
 * The sealed conversation with one AP, independent of how the bytes travel: a real BLE
 * connection feeds it notifications, and the tests feed it a recorded transcript.
 *
 * Counters rise by one per message in each direction and never repeat, so a nonce is never used
 * twice under this key. HELLO goes out at counter 0 as soon as the AP has sent its session
 * number, which is the only message that is not sealed.
 */
class LinkSession(private val key: ByteArray, private val send: (ByteArray) -> Unit) {
    @Volatile var session: Int? = null
        private set

    /** Body bytes per request chunk, narrowed to whatever MTU the connection negotiated. */
    @Volatile var chunk: Int = LinkProtocol.CHUNK_BODY

    @Volatile var hello: LinkProtocol.Hello? = null
        private set

    @Volatile var loggedIn = false
        private set

    private val lock = Any()
    private var tx = 0L
    private var rx = 0L
    private val joiner = ChunkJoiner()
    private val queue = LinkedBlockingQueue<Any>()

    /**
     * The link died under the conversation: wake whoever is waiting for a reply with the real
     * reason, instead of letting them sit out the timeout and then guess.
     */
    fun fail(reason: String) {
        queue.put(LinkException(reason))
    }

    /** One notification from the AP. Anything wrong ends the conversation (docs/ble-link.md). */
    fun feed(data: ByteArray) {
        if (data.size == HEADER + 2 && (data[0].toInt() and 0xFF) == OP_SESSION) {
            // The only unsealed message: it carries the session number both directions' nonces
            // are built from. A nonce is public by design, and this one is random.
            session = (data[HEADER].toInt() and 0xFF) or ((data[HEADER + 1].toInt() and 0xFF) shl 8)
            queue.put(Opened)
            return
        }
        val s = session
        if (s == null) {
            queue.put(LinkException("the AP sent a sealed message before its session"))
            return
        }
        try {
            val done = synchronized(lock) {
                val m = LinkProtocol.open(key, DIR_TO_CLIENT, s, rx, data)
                rx++
                joiner.add(m.opcode, m.flags, m.body)
            }
            if (done != null) queue.put(done)
        } catch (e: LinkException) {
            queue.put(e)
        }
    }

    private object Opened

    /** The AP sends its session as soon as the watcher subscribes to the reply characteristic. */
    fun waitOpen(timeoutMs: Long = TIMEOUT_MS) {
        if (session != null) return
        val got = queue.poll(timeoutMs, TimeUnit.MILLISECONDS)
            ?: throw LinkException(
                "subscribed to the replies but the AP never sent its session number",
            )
        if (got is LinkException) throw got
        if (got !== Opened) throw LinkException("the AP sent a reply before its session")
    }

    fun request(opcode: Int, body: ByteArray = ByteArray(0)) {
        val s = session ?: throw LinkException("no session yet")
        for ((flags, part) in LinkProtocol.splitChunks(body, chunk)) {
            val payload = synchronized(lock) {
                LinkProtocol.seal(key, DIR_TO_AP, s, tx, opcode, flags, part).also { tx++ }
            }
            send(payload)
        }
    }

    fun reply(timeoutMs: Long = TIMEOUT_MS): Pair<Int, ByteArray> {
        val got = queue.poll(timeoutMs, TimeUnit.MILLISECONDS)
            ?: throw LinkException(
                "no answer within ${timeoutMs / 1000} s of the request, and the link did not drop",
            )
        if (got is LinkException) throw got
        if (got === Opened) return reply(timeoutMs)
        val pair = @Suppress("UNCHECKED_CAST") (got as Pair<Int, ByteArray>)
        val opcode = pair.first
        val body = pair.second
        if (opcode == OP_ERROR) {
            val reason = if (body.size > 1) String(body, 1, body.size - 1, Charsets.UTF_8) else ""
            throw LinkException("the AP refused: " + reason.ifEmpty { "code ${body.firstOrNull() ?: 0}" })
        }
        return opcode to body
    }

    fun ask(opcode: Int, want: Int, body: ByteArray = ByteArray(0), timeoutMs: Long = TIMEOUT_MS): ByteArray {
        request(opcode, body)
        val (got, reply) = reply(timeoutMs)
        if (got != want) {
            throw LinkException("asked for ${LinkProtocol.opName(want)} and got ${LinkProtocol.opName(got)}")
        }
        return reply
    }

    fun sayHello(): LinkProtocol.Hello {
        waitOpen()
        val info = LinkProtocol.parseHelloOk(ask(OP_HELLO, OP_HELLO_OK, byteArrayOf(LinkProtocol.VERSION.toByte())))
        if (info.version != LinkProtocol.VERSION) {
            throw LinkException("this AP speaks admin link version ${info.version}, this app speaks ${LinkProtocol.VERSION}")
        }
        hello = info
        return info
    }

    sealed interface LoginResult {
        data object Ok : LoginResult
        data class Refused(val message: String, val wrongPassword: Boolean, val waitS: Int = 0) : LoginResult
    }

    /** Sends the HMAC proof, never the password. */
    fun logIn(auth: AdminAuth): LoginResult {
        val info = hello ?: throw LinkException("say hello first")
        if (!info.passwordSet) {
            return LoginResult.Refused(
                "This AP has no admin password yet: set it up on its admin page first.", false,
            )
        }
        val proof = auth.proof(info.salt, info.iterations, info.challenge)
            ?: return LoginResult.Refused("Log in to see positions, groups, availability and traffic.", false)
        request(OP_LOGIN, proof)
        val (opcode, body) = reply()
        if (opcode == OP_LOGIN_OK) {
            loggedIn = true
            return LoginResult.Ok
        }
        if (opcode == OP_LOGIN_FAIL) {
            val wait = if (body.size >= 4) {
                (body[0].toInt() and 0xFF) or ((body[1].toInt() and 0xFF) shl 8) or
                    ((body[2].toInt() and 0xFF) shl 16) or ((body[3].toInt() and 0xFF) shl 24)
            } else {
                0
            }
            return LoginResult.Refused(
                if (wait <= 0) "Wrong admin password."
                else "Too many failed attempts. Try again in $wait seconds.",
                true, wait,
            )
        }
        throw LinkException("unexpected reply to LOGIN: ${LinkProtocol.opName(opcode)}")
    }

    companion object {
        /** One request and its reply. */
        const val TIMEOUT_MS = 12_000L
    }
}
