package org.localgrid.gridwatch.link

/**
 * What the last pull brought back, and how it went.
 *
 * Never a BLE address (D21): a section remembers only the AP's name and when it answered. The
 * whole thing is dropped the moment the watcher logs out, so a lost phone shows the beacon and
 * nothing else until someone types the admin password again.
 */
class LinkData {
    enum class State {
        IDLE, CONNECTING, PULLING, DONE, ERROR,

        /** Android will not let this app connect: a different problem, and fixable in one tap. */
        NEEDS_PERMISSION,
    }

    data class Section<T>(val data: T, val apName: String, val atMs: Long) {
        fun ageS(nowMs: Long): Long = ((nowMs - atMs) / 1000.0).toLong()
    }

    private val lock = Any()

    @Volatile var state: State = State.IDLE
        private set

    @Volatile var message: String = "Not connected to an AP yet."
        private set

    @Volatile var lastTryMs: Long? = null
        private set

    @Volatile var status: Section<Status.Record>? = null
        private set

    @Volatile var history: Section<History.Record>? = null
        private set

    /** Kept per AP, so the Traffic tab can total across the APs it has reached. */
    @Volatile var traffic: List<Section<Traffic.Record>> = emptyList()
        private set

    /** Rises by one whenever anything here changes, so Compose knows to draw again. */
    @Volatile var revision: Int = 0
        private set

    private fun touch() {
        revision++
    }

    fun note(state: State, message: String, nowMs: Long = System.currentTimeMillis()) {
        synchronized(lock) {
            this.state = state
            this.message = message
            lastTryMs = nowMs
            touch()
        }
    }

    fun storeStatus(data: Status.Record, apName: String, nowMs: Long = System.currentTimeMillis()) {
        synchronized(lock) {
            status = Section(data, apName, nowMs)
            touch()
        }
    }

    fun storeHistory(data: History.Record, apName: String, nowMs: Long = System.currentTimeMillis()) {
        synchronized(lock) {
            history = Section(data, apName, nowMs)
            touch()
        }
    }

    fun storeTraffic(data: Traffic.Record, apName: String, nowMs: Long = System.currentTimeMillis()) {
        synchronized(lock) {
            traffic = (traffic.filter { it.data.ap != data.ap } + Section(data, apName, nowMs))
                .sortedBy { it.data.ap }
            touch()
        }
    }

    fun dropTraffic() {
        synchronized(lock) {
            traffic = emptyList()
            touch()
        }
    }

    /** Logging out throws away everything pulled: positions never linger on a lost phone. */
    fun clear() {
        synchronized(lock) {
            status = null
            history = null
            traffic = emptyList()
            state = State.IDLE
            message = "Not connected to an AP yet."
            touch()
        }
    }
}

/**
 * HELLO, LOGIN, GET_STATUS, GET_HISTORY and, when the Traffic tab is open, GET_TRAFFIC — the
 * whole conversation, with no idea of how the bytes travel. Returns true when it got in.
 */
fun pullOnce(
    session: LinkSession,
    auth: AdminAuth,
    data: LinkData,
    apName: String,
    wantTraffic: Boolean,
): Boolean {
    val info = session.sayHello()
    when (val r = session.logIn(auth)) {
        is LinkSession.LoginResult.Refused -> {
            if (r.wrongPassword) auth.wrongPassword(r.message) else auth.result(false, r.message)
            data.note(LinkData.State.ERROR, r.message)
            return false
        }
        LinkSession.LoginResult.Ok -> auth.result(true, "Logged in.")
    }
    data.note(LinkData.State.PULLING, "Reading $apName…")
    data.storeStatus(Status.decode(String(session.ask(LinkProtocol.OP_GET_STATUS, LinkProtocol.OP_STATUS), Charsets.UTF_8)), apName)
    data.storeHistory(History.decode(session.ask(LinkProtocol.OP_GET_HISTORY, LinkProtocol.OP_HISTORY)), apName)
    if (wantTraffic) {
        try {
            val decoded = Traffic.decode(session.ask(LinkProtocol.OP_GET_TRAFFIC, LinkProtocol.OP_TRAFFIC))
            data.storeTraffic(decoded, apName)
        } catch (e: LinkException) {
            data.dropTraffic()
            data.note(
                LinkData.State.DONE,
                "$apName answered status and history; no traffic counters (${e.message}).",
            )
            return true
        }
    }
    data.note(LinkData.State.DONE, "Pulled from $apName (AP ${info.ap}).")
    return true
}
