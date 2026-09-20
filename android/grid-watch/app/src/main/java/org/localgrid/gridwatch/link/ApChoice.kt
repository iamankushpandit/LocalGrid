package org.localgrid.gridwatch.link

/**
 * Which AP to ask, and in what order.
 *
 * Every AP carries the same answers — the status, the history and its own counters — so a
 * failure with one is no reason to give up: the app works down the list, strongest first, and
 * only reports a problem when every AP it can hear has failed, saying what happened to each.
 *
 * Pure arithmetic, so the order is testable without a radio.
 */
object ApChoice {
    /** An AP the scan has heard: its index, how loud it was, and how long ago (D21: no address). */
    data class Heard(val ap: Int, val rssi: Int, val ageMs: Long)

    const val FRESH_MS = 15_000L

    /**
     * The APs worth trying, loudest first. One heard a moment ago beats one heard a while back
     * at the same strength, and anything older than [freshMs] is not in range any more.
     */
    fun order(heard: List<Heard>, freshMs: Long = FRESH_MS): List<Int> =
        heard.filter { it.ageMs <= freshMs }
            .sortedWith(compareByDescending<Heard> { it.rssi }.thenBy { it.ageMs }.thenBy { it.ap })
            .map { it.ap }

    /** One line saying what happened to every AP tried, for the error the person reads. */
    fun summarise(failures: List<Pair<String, String>>): String = when {
        failures.isEmpty() -> "No AP of this grid is in range to ask."
        failures.size == 1 -> "${failures[0].first}: ${failures[0].second}"
        else -> "No AP answered. " + failures.joinToString("; ") { "${it.first}: ${it.second}" }
    }
}
