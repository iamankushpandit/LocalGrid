package org.localgrid.gridwatch.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.localgrid.gridwatch.grid.Snapshot
import org.localgrid.gridwatch.watch.LinkView

/**
 * The Handhelds tab: the groups, who may send an announcement, and every handheld the grid has
 * seen — including ones out of this phone's own range.
 *
 * Facts about the grid only. No message, no group message and no announcement text crosses this
 * link: the AP has no request that returns any message body, and a 1:1 message is sealed end to
 * end between handhelds, so not even the AP can read one.
 */
fun LazyListScope.handheldsTab(link: LinkView, snap: Snapshot?, nowMs: Long) {
    val section = link.status
    if (section == null) {
        item { Section("Groups") { Hint("Waiting for the first answer from an AP…") } }
        return
    }
    val status = section.data
    val battery = snap?.handhelds.orEmpty().associateBy { it.device }

    item {
        Section("Groups", sectionAge(section, nowMs)) {
            Hint(
                "Groups are shared by every AP and handheld. This app only watches: groups are " +
                    "made and changed on a handheld or on the AP's admin page.",
            )
            Spacer(Modifier.height(8.dp))
            if (status.groups.isEmpty()) {
                Hint("No groups yet.")
            } else {
                TableRow(listOf("Name" to 1f, "Members" to 2f), header = true)
                for (g in status.groups) {
                    Divider()
                    TableRow(
                        listOf(
                            g.name to 1f,
                            (g.members.joinToString(", ") { status.nameOf(it) }.ifEmpty { "nobody" }) to 2f,
                        ),
                    )
                }
            }
        }
    }
    item {
        Section("Announcements") {
            Hint(
                "An announcement goes to every handheld and takes over its screen, so only these " +
                    "people may send one. Urgent messages are never limited: anyone can always " +
                    "raise an emergency.",
            )
            Spacer(Modifier.height(8.dp))
            Text(
                if (status.announceAll) {
                    "Everyone may send an announcement, including handhelds the grid has not seen yet."
                } else {
                    "May announce: " +
                        (status.announcers.joinToString(", ") { status.nameOf(it) }.ifEmpty { "nobody" })
                },
                color = Lg.text, fontSize = 14.sp,
            )
        }
    }
    item {
        Section("Handhelds", sectionAge(section, nowMs)) {
            if (status.devices.isEmpty()) {
                Hint("No handhelds have connected yet.")
            } else {
                TableRow(
                    listOf("Name" to 1.3f, "Status" to 1f, "Connected to" to 1.1f, "Battery" to 1f),
                    header = true,
                )
                for (d in status.devices) {
                    Divider()
                    val where = if (d.state == "ONLINE" && d.ap >= 0) apNameFor(link, status.ap, status.apName, d.ap) else "--"
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Text(d.name ?: "Handheld ${d.device}", fontSize = 14.sp, color = Lg.text,
                            modifier = Modifier.weight(1.3f))
                        Text(d.state, fontSize = 14.sp,
                            color = if (d.state == "ONLINE") Lg.accent else Lg.muted,
                            modifier = Modifier.weight(1f))
                        Text(where, fontSize = 14.sp, color = Lg.text, modifier = Modifier.weight(1.1f))
                        Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                            val b = battery[d.device]?.battery
                            if (b == null) Text("–", color = Lg.muted) else {
                                BatteryBar(b)
                                Text(" $b%", fontSize = 13.sp)
                            }
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                }
                Spacer(Modifier.height(6.dp))
                Hint(
                    "Batteries come from the beacon, so they show for handhelds this phone can " +
                        "hear; the rest of the row comes from the AP that answered.",
                )
            }
        }
    }
    item { LogOutRow() }
}

private fun apNameFor(link: LinkView, selfAp: Int, selfName: String, ap: Int): String {
    if (ap == selfAp) return selfName
    val h = link.history?.data ?: return "AP $ap"
    return apNameIn(h, ap)
}
