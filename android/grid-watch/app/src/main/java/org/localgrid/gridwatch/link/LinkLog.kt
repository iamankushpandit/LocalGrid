package org.localgrid.gridwatch.link

/**
 * A short log of what the app tried on the admin link and what came back, readable in the app.
 *
 * It exists because a wrong guess in an error message ("busy") has twice sent people looking at
 * the APs when the problem was on the phone. Every step of a connection writes a line here, so
 * the next failure can be read rather than guessed at.
 *
 * It never holds a BLE address (D21), a password, a proof or any grid content: only AP names,
 * step names, MTUs and status codes.
 */
object LinkLog {
    const val MAX_LINES = 60

    data class Line(val atMs: Long, val text: String)

    private val lock = Any()
    private val lines = ArrayDeque<Line>()

    @Volatile var revision: Int = 0
        private set

    fun add(text: String, nowMs: Long = System.currentTimeMillis()) {
        synchronized(lock) {
            lines.addLast(Line(nowMs, text))
            while (lines.size > MAX_LINES) lines.removeFirst()
            revision++
        }
    }

    /** Newest first, for the screen. */
    fun lines(): List<Line> = synchronized(lock) { lines.toList().asReversed() }

    fun clear() {
        synchronized(lock) {
            lines.clear()
            revision++
        }
    }
}
