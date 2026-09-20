package org.localgrid.gridwatch.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.link.Traffic
import org.localgrid.gridwatch.watch.GridHub
import org.localgrid.gridwatch.watch.LinkView
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The Traffic tab (D70): how busy the grid is and how well the APs are coping.
 *
 * Counts, sizes and rates, and a plain-English line in red whenever something is over a sensible
 * limit. Never a message, never a sound, never who said what to whom: the counters carry none of
 * that, because the AP counts only classes and faults.
 */
fun LazyListScope.trafficTab(link: LinkView, nowMs: Long) {
    // Names: the AP's own record first (it knows every AP), then whatever the beacon has heard.
    val name: (Int) -> String = { ap ->
        link.history?.data?.let { apNameIn(it, ap) }?.takeIf { !it.startsWith("AP ") }
            ?: GridHub.apName(ap)
    }
    if (link.traffic.isEmpty()) {
        item {
            Section("Traffic and performance") {
                Hint(
                    "Asking an AP for its counters… while this tab is open they are fetched every " +
                        "30 seconds, so the APs are left alone the rest of the time.",
                )
                if (link.state == LinkData.State.ERROR) {
                    Spacer(Modifier.height(8.dp))
                    Hint(link.message, Lg.warn)
                }
            }
        }
        item { LogOutRow() }
        return
    }

    // /api/status comes from one AP only, and carries the LoRa detail the packed record cannot:
    // the module's version and a line per peer. Used for that AP, never assumed of the others.
    val statusLora = link.status?.data?.takeIf { it.lora != null }
    val loraPeersOf: (Int) -> List<Status.LoraPeer> = { ap ->
        if (statusLora != null && statusLora.ap == ap) statusLora.lora!!.peers else emptyList()
    }

    val allNotes = ArrayList<Traffic.Note>()
    for (section in link.traffic) {
        allNotes += Traffic.notes(section.data, section.apName, loraPeersOf(section.data.ap), name)
    }
    item {
        Section("Over the limit") {
            if (allNotes.isEmpty()) {
                Hint("Nothing over its limit: the APs are coping with what the grid is carrying.", Lg.accent)
            } else {
                for (n in allNotes) NoteLine(n.text, n.red)
            }
        }
    }
    for (section in link.traffic) {
        item {
            ApTraffic(
                section, nowMs, name,
                if (statusLora != null && statusLora.ap == section.data.ap) statusLora.lora else null,
            )
        }
    }
    if (link.traffic.size > 1) {
        item {
            Section("Across the ${link.traffic.size} APs asked") {
                val total = LinkedHashMap<String, Traffic.Counts>()
                var dropped = 0L
                for (s in link.traffic) {
                    dropped += s.data.fault("voice_dropped")
                    for ((cls, c) in s.data.messages) {
                        val t = total[cls] ?: Traffic.Counts(0, 0, 0)
                        total[cls] = Traffic.Counts(t.inCount + c.inCount, t.out + c.out, t.relayed + c.relayed)
                    }
                }
                MessageTable(total, dropped)
            }
        }
    }
    item { LogOutRow() }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ApTraffic(
    section: LinkData.Section<Traffic.Record>,
    nowMs: Long,
    name: (Int) -> String,
    statusLora: Status.Lora?,
) {
    val t = section.data
    val p = t.perf
    Section(
        "${section.apName} · up ${fmtDur(t.uptimeS)}",
        "Counters fetched ${ago(section.ageS(nowMs))}.",
    ) {
        FlowRow(Modifier.fillMaxWidth()) {
            Stat("Messages a minute", t.rate1m?.toString() ?: "--")
            Stat("Over 5 minutes", t.rate5m?.toString() ?: "--")
            Stat("Since it started", "${t.inTotal} in, ${t.outTotal} out")
            Stat(
                "Voice dropped", t.fault("voice_dropped").toString(),
                color = if (t.fault("voice_dropped") > 0) Lg.danger else Lg.text,
            )
        }
        if (t.buckets.isNotEmpty()) {
            Sparkline(t.buckets)
            Hint(
                "Messages in and out, one step every ${t.bucketS.roundToInt()} s; the newest is on " +
                    "the right.",
            )
        }

        SubHeading("Messages")
        MessageTable(t.messages, t.fault("voice_dropped"))

        SubHeading("Drops and faults")
        for ((name, v) in t.faults) {
            TableRow(
                listOf((Traffic.FAULT_WORDS[name] ?: name) to 2f, v.toString() to 1f),
                colors = listOf(
                    null,
                    when {
                        v > 0 && name in Traffic.FAULTS_THAT_MATTER -> Lg.danger
                        v > 0 -> Lg.warn
                        else -> Lg.muted
                    },
                ),
            )
        }

        if (t.links.isNotEmpty()) {
            SubHeading("Backbone links")
            TableRow(
                listOf("Link to" to 1.1f, "State" to 1.2f, "Sent" to 1f, "Failed" to 1f, "Last heard" to 1.2f),
                header = true,
            )
            for (l in t.links) {
                Divider()
                val failing = l.sent > 0 && 100.0 * l.failures / l.sent >= Traffic.Limit.LINK_FAIL_PCT
                TableRow(
                    listOf(
                        name(l.ap) to 1.1f,
                        (if (l.up) "up, ${l.rssi} dBm" else "down") to 1.2f,
                        l.sent.toString() to 1f,
                        l.failures.toString() to 1f,
                        (if (l.heardS == null) "never" else "${fmtDur(l.heardS)} ago") to 1.2f,
                    ),
                    colors = listOf(null, if (l.up) Lg.accent else Lg.danger, null,
                        if (failing) Lg.danger else null, null),
                )
                Text(
                    "${fmtBytes(l.bytesOut)} out, ${fmtBytes(l.bytesIn)} in, ${l.received} frames received",
                    color = Lg.muted, fontSize = 12.sp,
                )
            }
        }

        LoraPanel(t, section.apName, statusLora, name)

        SubHeading("Handhelds on this AP")
        val hh = t.handhelds
        Field("Connected now", "${hh.sessions} session${if (hh.sessions == 1) "" else "s"}, ${hh.registered} registered")
        Field("Since it started", "${hh.opened} sessions, ${hh.registrations} registrations, ${hh.disconnects} disconnects")
        Field(
            "Slowest send wait", "${hh.slowestSendMs} ms",
            if (hh.slowestSendMs >= Traffic.Limit.SEND_WAIT_WARN_MS) Lg.warn else null,
        )
        Field("Bytes", "${fmtBytes(hh.bytesIn)} in, ${fmtBytes(hh.bytesOut)} out")

        SubHeading("This AP's performance")
        val heapKb = (p.heapMin / 1024.0).roundToInt()
        Field(
            "Free memory", "${fmtKB(p.heapFree)} now, lowest ${fmtKB(p.heapMin)}",
            if (heapKb < Traffic.Limit.HEAP_BAD_KB) Lg.danger else if (heapKb < Traffic.Limit.HEAP_WARN_KB) Lg.warn else null,
        )
        Field("Largest free block", fmtKB(p.heapLargest))
        Field(
            "Core queue", "${p.queueDepth} waiting, highest ${p.queueHigh}",
            if (p.queueHigh >= Traffic.Limit.QUEUE_BAD) Lg.danger else if (p.queueHigh >= Traffic.Limit.QUEUE_WARN) Lg.warn else null,
        )
        Field(
            "Spare stack", "core ${fmtBytes(p.stackCore)}, link ${fmtBytes(p.stackLink)}",
            if (minOf(p.stackCore, p.stackLink) < Traffic.Limit.STACK_LOW_B) Lg.danger else null,
        )
        Field(
            "Main loop", "longest ${p.loopMaxMs} ms, average ${Traffic.avgPass(p.loopAvgUs)}",
            if (p.loopMaxMs >= Traffic.Limit.LOOP_BAD_MS) Lg.danger else if (p.loopMaxMs >= Traffic.Limit.LOOP_WARN_MS) Lg.warn else null,
        )
        Field(
            "Radio errors", "ESP-NOW ${p.espnowErrors}, Wi-Fi side ${p.wifiErrors}",
            if (p.espnowErrors > 0 || p.wifiErrors > 0) Lg.danger else null,
        )
        Field(
            "CPU busy", p.cpuBusy?.let { "$it%" } ?: "not measured",
            if (p.cpuBusy != null && p.cpuBusy >= Traffic.Limit.CPU_WARN) Lg.danger else null,
        )
    }
}

/**
 * The second backbone (D71), per AP, in the admin page's order: what it is worth first, then the
 * module, then each peer's link, then the counters.
 *
 * "No module here" and "fitted and silent" are two different things and are said as two different
 * things. An AP that answered before D71 sends no LoRa section at all, and then this says so
 * rather than showing a row of zeros that would look like a dead radio.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun LoraPanel(
    t: Traffic.Record,
    apName: String,
    statusLora: Status.Lora?,
    name: (Int) -> String,
) {
    SubHeading("LoRa backbone")
    val l = t.lora
    if (l == null) {
        Hint("This AP does not report LoRa. Its firmware is older than the second backbone (D71).")
        return
    }
    if (!l.fitted) {
        Hint(
            "No LoRa module on $apName. This AP works exactly as it does without one, and the " +
                "other APs are unaffected.",
        )
        return
    }

    // The number that says whether the radio earns its keep, before anything else.
    FlowRow(Modifier.fillMaxWidth()) {
        Stat(
            "Arrived by LoRa first", l.framesFirst.toString(),
            color = if (l.framesFirst > 0) Lg.accent else Lg.text,
        )
        Stat(
            "Last signal",
            if (l.heardS == null) "--" else "${l.rssi} dBm",
            color = when {
                l.heardS == null -> Lg.danger
                Traffic.loraSignalIsBad(l.rssi) -> Lg.danger
                Traffic.loraSignalIsWeak(l.rssi) -> Lg.warn
                else -> Lg.accent
            },
        )
        Stat("SNR", if (l.heardS == null) "--" else l.snr.toString())
        Stat(
            "Dropped", l.trouble.toString(),
            color = if (l.trouble > 0) Lg.danger else Lg.text,
        )
    }
    Hint(Traffic.loraWorth(l), if (l.framesFirst > 0) Lg.accent else Lg.muted)

    Field(
        "Module",
        (if (l.configured) "configured" else "answered, not configured") +
            (statusLora?.version?.let { ", $it" } ?: ""),
        if (l.configured) null else Lg.danger,
    )
    Field("Address", "${l.address} on network ${l.network}")
    Field("Radio", Traffic.Radio.words)
    Field(
        "Last heard",
        if (l.heardS == null) "nothing received yet" else "${fmtDur(l.heardS)} ago, ${l.rssi} dBm, SNR ${l.snr}",
        when {
            l.heardS == null -> Lg.danger
            l.heardS >= Traffic.Limit.LORA_SILENT_S -> Lg.danger
            Traffic.loraSignalIsWeak(l.rssi) -> Lg.warn
            else -> null
        },
    )
    if (l.off) Field("Switched off", "held off for a test; it comes back by itself", Lg.warn)
    if (!l.big) Field("Long payloads", "no room on this AP; nothing sends one yet", Lg.muted)
    if (!l.broadcast) Field("Broadcast", "one transmission per peer (broadcast address off)")

    // Per peer. The packed record carries only which heartbeats are current, so the signal and
    // the age come from /api/status when this phone asked this AP; otherwise up or down only.
    val peers = statusLora?.peers.orEmpty()
    if (peers.isNotEmpty()) {
        TableRow(
            listOf("Peer" to 1.1f, "Link" to 1f, "Signal" to 1.1f, "SNR" to 0.7f, "Last heard" to 1.2f),
            header = true,
        )
        for (p in peers) {
            Divider()
            TableRow(
                listOf(
                    name(p.ap) to 1.1f,
                    (if (p.up) "up" else "down") to 1f,
                    "${p.rssi} dBm" to 1.1f,
                    p.snr.toString() to 0.7f,
                    (if (p.ageS == null) "never" else "${fmtDur(p.ageS)} ago") to 1.2f,
                ),
                colors = listOf(
                    null,
                    if (p.up) Lg.accent else Lg.danger,
                    when {
                        Traffic.loraSignalIsBad(p.rssi) -> Lg.danger
                        Traffic.loraSignalIsWeak(p.rssi) -> Lg.warn
                        else -> Lg.accent
                    },
                ),
            )
        }
    } else {
        val up = l.peersUp
        Field(
            "Peers heard",
            if (up.isEmpty()) "none: no other module's heartbeat is current"
            else up.joinToString(", ") { name(it) },
            if (up.isEmpty()) Lg.danger else Lg.accent,
        )
        Hint(
            "Per-peer signal comes from the AP this phone asked for its status; this AP only " +
                "reports which heartbeats are current.",
        )
    }

    Field("Frames", "${l.framesIn} in, ${l.framesOut} out")
    Field("Parts", "${l.partsIn} in, ${l.partsOut} out, ${l.retries} retries")
    Field(
        "Parts dropped", l.partsDropped.toString(),
        if (l.partsDropped > 0) Lg.warn else null,
    )
    Field(
        "Reassembly given up", l.reasmTimeouts.toString(),
        if (l.reasmTimeouts > 0) Lg.warn else null,
    )
    Field(
        "Could not authenticate", l.sealFail.toString(),
        if (l.sealFail > 0) Lg.danger else null,
    )
    Field(
        "Send queue", "${l.queueDepth} waiting, highest ${l.queueHigh}, ${l.queueDropped} dropped",
        if (l.queueDropped > 0 || l.queueDepth >= Traffic.Limit.LORA_QUEUE_BAD) Lg.danger
        else if (l.queueHigh >= Traffic.Limit.LORA_QUEUE_WARN) Lg.warn else null,
    )
    if (l.refusedBig > 0) Field("Too large to carry", l.refusedBig.toString(), Lg.danger)
    val pct = if (t.uptimeS > 0) 100.0 * l.airtimeS / t.uptimeS else 0.0
    Field(
        "On the air",
        String.format(java.util.Locale.US, "%.1f s (%.1f%% of the time up)", l.airtimeS, pct),
        if (pct >= Traffic.Limit.LORA_AIRTIME_PCT_BAD) Lg.danger
        else if (pct >= Traffic.Limit.LORA_AIRTIME_PCT_WARN) Lg.warn else null,
    )
    Field("Module restarts", l.restarts.toString(), if (l.restarts > 0) Lg.warn else null)
}

@Composable
private fun MessageTable(messages: Map<String, Traffic.Counts>, voiceDropped: Long) {
    TableRow(listOf("Class" to 2f, "In" to 1f, "Out" to 1f, "Relayed" to 1f), header = true)
    for ((cls, m) in messages) {
        if (m.inCount == 0L && m.out == 0L && m.relayed == 0L) continue
        Divider()
        val name = (Traffic.CLASS_WORDS[cls] ?: cls) +
            if (cls == "voice" && voiceDropped > 0) "  ($voiceDropped dropped)" else ""
        TableRow(
            listOf(name to 2f, m.inCount.toString() to 1f, m.out.toString() to 1f, m.relayed.toString() to 1f),
            colors = listOf(if (cls == "voice" && voiceDropped > 0) Lg.danger else null),
        )
    }
}

/** Messages a step, oldest on the left: the same shape the laptop dashboard draws. */
@Composable
private fun Sparkline(buckets: List<Traffic.Bucket>) {
    Canvas(Modifier.fillMaxWidth().height(36.dp)) {
        val vals = buckets.map { it.messages }
        val top = max(1, vals.max())
        val step = size.width / max(1, vals.size - 1)
        var prev: Offset? = null
        vals.forEachIndexed { i, v ->
            val point = Offset(i * step, size.height - size.height * 0.9f * v / top)
            prev?.let { drawLine(Lg.accent, it, point, strokeWidth = 2f) }
            prev = point
        }
    }
}
