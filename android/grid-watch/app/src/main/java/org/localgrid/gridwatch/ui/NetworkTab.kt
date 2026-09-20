package org.localgrid.gridwatch.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.localgrid.gridwatch.link.History
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.watch.LinkView
import java.text.DateFormat
import java.util.Date
import kotlin.math.roundToInt

private val TIME_QUALITY_WORDS = listOf("not set", "carried", "set here")

/**
 * The Network tab: every AP the grid knows, whether it was reachable minute by minute for the
 * last two hours, and what happened to each one. All of it from the APs' own shared record,
 * which survives restarts and is filled in from the other APs (D48).
 */
fun LazyListScope.networkTab(link: LinkView, nowMs: Long) {
    val hist = link.history
    if (hist == null) {
        item { Section("APs") { Hint("Waiting for the first answer from an AP…") } }
        return
    }
    val h = hist.data
    val status = link.status?.data
    item {
        Section("APs", sectionAge(hist, nowMs)) {
            ApTable(h, status)
        }
    }
    item {
        Section("Availability, last 2 hours") {
            for (a in h.aps) {
                Text(a.name.ifEmpty { "AP ${a.ap}" }, fontSize = 14.sp, color = Lg.text)
                AvailabilityBar(a.avail)
                val recorded = a.avail.count { it != 0 }
                val upMin = a.avail.sumOf { if (it == 1) 1.0 else if (it == 2) 0.5 else 0.0 }
                Text(
                    if (recorded > 0) "${(100 * upMin / recorded).roundToInt()}% of $recorded min" else "no record",
                    color = Lg.muted, fontSize = 13.sp,
                )
                Spacer(Modifier.height(8.dp))
            }
            Row(Modifier.fillMaxWidth()) {
                Text("2 h ago", color = Lg.muted, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text("1 h", color = Lg.muted, fontSize = 12.sp)
                Text("now", color = Lg.muted, fontSize = 12.sp, modifier = Modifier.padding(start = 40.dp))
            }
            Spacer(Modifier.height(8.dp))
            Legend()
            val from = h.aps.firstOrNull { it.self }?.name ?: "the AP asked"
            Spacer(Modifier.height(8.dp))
            Hint(
                "As seen from $from: whether each AP was reachable over the AP-to-AP link, minute " +
                    "by minute. The history survives restarts and unplugging; minutes $from was " +
                    "down are filled in from the other APs' records.",
            )
        }
    }
    item {
        Section("What happened to the APs") {
            val inc = h.incidents.sortedByDescending { it.downGridTime }
            if (inc.isEmpty()) {
                Hint("No AP outage on record. The log survives restarts and is shared between APs.")
            } else {
                TableRow(
                    listOf("When" to 1.5f, "AP" to 0.9f, "Down for" to 0.9f, "What happened" to 2f),
                    header = true,
                )
                for (i in inc) {
                    Divider()
                    val what = when (i.kind) {
                        "restarted" -> "Restarted: ${i.reset}" +
                            if (i.prevRunS >= 0) " after running ${fmtDur(i.prevRunS)}" else ""
                        "link" -> "Kept running; the AP-to-AP link dropped (radio or distance)"
                        "unexplained" -> "Came back but did not say why within a minute"
                        else -> "Unreachable now"
                    }
                    TableRow(
                        listOf(
                            (if (i.downGridTime > 0) {
                                DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                                    .format(Date(i.downGridTime * 1000))
                            } else {
                                "grid time was not set"
                            }) to 1.5f,
                            apNameIn(h, i.ap) to 0.9f,
                            fmtDur(i.durationS.toLong()) to 0.9f,
                            what to 2f,
                        ),
                        colors = listOf(null, null, null, if (i.kind == "down") Lg.danger else null),
                    )
                    Text("Seen by ${apNameIn(h, i.seenBy)}", color = Lg.muted, fontSize = 12.sp)
                }
            }
        }
    }
    item { LogOutRow() }
}

fun apNameIn(h: History.Record, ap: Int): String =
    h.aps.firstOrNull { it.ap == ap }?.name?.ifEmpty { null } ?: "AP $ap"

@Composable
private fun ApTable(h: History.Record, status: Status.Record?) {
    val links = status?.links.orEmpty().associateBy { it.ap }
    TableRow(listOf("AP" to 1.1f, "Status" to 1.1f, "Up for" to 1f, "Grid time" to 1f), header = true)
    for (a in h.aps) {
        Divider()
        val l = links[a.ap]
        val reachable = a.self || (l != null && l.up)
        val state = when {
            a.self -> "YOU ARE HERE"
            reachable -> "ONLINE"
            else -> "UNREACHABLE"
        }
        TableRow(
            listOf(
                a.name.ifEmpty { "AP ${a.ap}" } to 1.1f,
                state to 1.1f,
                (if (reachable) fmtDur(a.uptimeS) else "--") to 1f,
                (TIME_QUALITY_WORDS.getOrNull(a.timeQuality) ?: "unknown") to 1f,
            ),
            colors = listOf(null, if (reachable) Lg.accent else Lg.danger, null, null),
        )
        val sync = when {
            a.syncAgeS < 0 -> "never synced"
            a.syncFrom == a.ap -> "time set here ${fmtDur(a.syncAgeS)} ago"
            else -> "time from ${apNameIn(h, a.syncFrom)}, ${fmtDur(a.syncAgeS)} ago" +
                if (a.syncDriftMs != 0) " (${if (a.syncDriftMs > 0) "+" else ""}${a.syncDriftMs} ms)" else ""
        }
        val signal = when {
            a.self -> "this AP"
            l != null && l.up -> "${l.rssi} dBm"
            else -> "last heard ${fmtDur(a.heardS)} ago"
        }
        Text(
            "$sync · last restart ${a.reset}" +
                (if (a.prevRunS >= 0) ", after ${fmtDur(a.prevRunS)}" else "") + " · $signal",
            color = Lg.muted, fontSize = 12.sp, modifier = Modifier.padding(bottom = 4.dp),
        )
    }
}

/** Two hours of minutes, oldest on the left, exactly as the admin page draws them. */
@Composable
private fun AvailabilityBar(avail: IntArray) {
    Canvas(Modifier.fillMaxWidth().height(16.dp).padding(vertical = 2.dp)) {
        val n = avail.size.coerceAtLeast(1)
        val w = size.width / n
        avail.forEachIndexed { i, c ->
            val color = when (c) {
                1 -> Lg.accent
                2 -> Lg.warn
                3 -> Lg.danger
                else -> Lg.line
            }
            drawRect(color, topLeft = Offset(i * w, 0f), size = Size(w * 0.92f, size.height))
        }
    }
}

@Composable
private fun Legend() {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        for ((color, word) in listOf(
            Lg.accent to "reachable", Lg.warn to "part of the minute",
            Lg.danger to "unreachable", Lg.line to "no record",
        )) {
            Column(Modifier.weight(1f)) {
                Canvas(Modifier.height(8.dp).fillMaxWidth()) { drawRect(color, size = Size(size.width * 0.5f, size.height)) }
                Text(word, color = Lg.muted, fontSize = 11.sp)
            }
        }
    }
}
