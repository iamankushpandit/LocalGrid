package org.localgrid.gridwatch.ui

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.ScrollableTabRow
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.LifecycleResumeEffect
import kotlinx.coroutines.delay
import org.localgrid.gridwatch.R
import org.localgrid.gridwatch.grid.AlertView
import org.localgrid.gridwatch.grid.ApView
import org.localgrid.gridwatch.grid.GridEvent
import org.localgrid.gridwatch.grid.HandheldView
import org.localgrid.gridwatch.grid.Snapshot
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.link.Traffic
import org.localgrid.gridwatch.watch.GridHub
import org.localgrid.gridwatch.watch.LinkHub
import org.localgrid.gridwatch.watch.LinkView
import org.localgrid.gridwatch.watch.Radio
import java.text.DateFormat
import java.util.Date

/** The tabs of the AP's admin page and the laptop dashboard, on the phone (D70). */
enum class WatchTab(val label: String, val needsLogin: Boolean) {
    OVERVIEW("Overview", false),
    MAP("Map", true),
    NETWORK("Network", true),
    HANDHELDS("Handhelds", true),
    TRAFFIC("Traffic", true),
}

/** How long ago a pulled section arrived, in the same words as everything else. */
fun sectionAge(section: LinkData.Section<*>?, nowMs: Long): String? =
    section?.let { "From ${it.apName}, fetched ${ago(it.ageS(nowMs))}." }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Dashboard(onOpenPairing: () -> Unit) {
    val ctx = LocalContext.current
    val snap by GridHub.snapshot.collectAsState()
    val watching by GridHub.watching.collectAsState()
    val radio by GridHub.radio.collectAsState()
    val scanError by GridHub.scanError.collectAsState()
    val link by LinkHub.view.collectAsState()
    var tab by remember { mutableStateOf(WatchTab.OVERVIEW) }
    var nowMs by remember { mutableLongStateOf(System.currentTimeMillis()) }

    // The link runs only while this screen is on the phone's screen (battery, D70). The
    // background service goes on with the beacon and SOS notifications either way.
    LaunchedEffect(Unit) {
        while (true) {
            nowMs = System.currentTimeMillis()
            LinkHub.tick(ctx)
            delay(1000)
        }
    }
    LaunchedEffect(tab) { LinkHub.wantTraffic.value = tab == WatchTab.TRAFFIC }

    Scaffold(
        containerColor = Lg.bg,
        topBar = {
            TopAppBar(
                title = { Text(link.status?.data?.gridName ?: stringResource(R.string.app_name)) },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Lg.bg, titleContentColor = Lg.text),
                actions = {
                    TextButton(
                        onClick = { LinkHub.refresh(ctx, now = true) },
                        enabled = !link.busy,
                    ) { Text("Refresh") }
                    TextButton(onClick = onOpenPairing) { Text(stringResource(R.string.grid_button)) }
                },
            )
        },
    ) { pad ->
        Column(Modifier.fillMaxSize().padding(top = pad.calculateTopPadding())) {
            ScrollableTabRow(
                selectedTabIndex = tab.ordinal,
                containerColor = Lg.bg,
                contentColor = Lg.accent,
                edgePadding = 12.dp,
            ) {
                for (t in WatchTab.entries) {
                    Tab(
                        selected = t == tab,
                        onClick = { tab = t },
                        text = {
                            Text(
                                t.label,
                                fontSize = 15.sp,
                                fontWeight = if (t == tab) FontWeight.SemiBold else FontWeight.Normal,
                                color = if (t == tab) Lg.accent else Lg.muted,
                            )
                        },
                    )
                }
            }
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(
                    start = 16.dp, end = 16.dp, top = 12.dp,
                    bottom = pad.calculateBottomPadding() + 24.dp,
                ),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                if (tab.needsLogin && !link.loggedIn) {
                    item { LoginCard(link, tab) }
                    item { ConnectionLog(link, nowMs) }
                    item { Hint(readOnlyPromise) }
                    return@LazyColumn
                }
                when (tab) {
                    WatchTab.OVERVIEW -> overviewTab(snap, watching, radio, scanError, link, nowMs)
                    WatchTab.MAP -> mapTab(link, nowMs)
                    WatchTab.NETWORK -> networkTab(link, nowMs)
                    WatchTab.HANDHELDS -> handheldsTab(link, snap, nowMs)
                    WatchTab.TRAFFIC -> trafficTab(link, nowMs)
                }
                if (tab != WatchTab.OVERVIEW) {
                    item { LinkStatusLine(link) }
                    item { ConnectionLog(link, nowMs) }
                }
            }
        }
    }
}

const val readOnlyPromise =
    "This app watches and takes no part in the grid. It cannot read a message, hear push-to-talk, " +
        "send anything, or change a setting: the link to an AP has no request that writes, none " +
        "that carries message text and none that carries audio. It never joins the grid's Wi-Fi " +
        "and never registers as a handheld."

@Composable
fun LinkStatusLine(link: LinkView) {
    val color = when (link.state) {
        LinkData.State.ERROR -> Lg.warn
        LinkData.State.DONE -> Lg.muted
        else -> Lg.muted
    }
    Hint(if (link.busy) "Asking an AP…" else link.message, color)
}

// ---------------------------------------------------------------- Overview

private fun LazyListScope.overviewTab(
    snap: Snapshot?,
    watching: Boolean,
    radio: Radio,
    scanError: String?,
    link: LinkView,
    nowMs: Long,
) {
    item { Header(snap, watching) }
    item { WatchControl(watching, radio, scanError) }
    item { Banner(snap?.alert) }
    val traffic = link.traffic.firstOrNull()
    if (link.loggedIn && traffic != null) {
        item { TrafficNow(traffic, nowMs) }
    }
    // D71: one line per AP that has a module, and nothing at all when none has.
    val withLora = link.traffic.filter { it.data.lora?.fitted == true }
    if (link.loggedIn && withLora.isNotEmpty()) {
        item { LoraNow(withLora, link.status?.data) }
    }
    item { SectionTitle("APs") }
    val aps = snap?.aps.orEmpty()
    if (aps.isEmpty()) {
        item {
            Hint(
                if (watching) "Listening… no AP of this grid heard yet."
                else "Not watching. Start watching to listen for this grid's APs.",
            )
        }
    } else {
        items(aps, key = { "ap" + it.index }) { ApCard(it) }
    }
    item { Section("Handhelds") { Handhelds(snap?.handhelds.orEmpty()) } }
    item { Section("Events") { Events(snap) } }
    item { LinkStatusLine(link) }
}

/** The short version of the Traffic tab, on the Overview. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TrafficNow(section: LinkData.Section<Traffic.Record>, nowMs: Long) {
    val t = section.data
    Section("Traffic now", sectionAge(section, nowMs)) {
        FlowRow(Modifier.fillMaxWidth()) {
            Stat("Messages a minute", t.rate1m?.toString() ?: "--")
            Stat(
                "Voice dropped", t.fault("voice_dropped").toString(),
                color = if (t.fault("voice_dropped") > 0) Lg.danger else Lg.text,
            )
            Stat(
                "Free memory", fmtKB(t.perf.heapFree),
                color = if (t.perf.heapMin < Traffic.Limit.HEAP_BAD_KB * 1024) Lg.danger else Lg.text,
            )
            Stat("Handhelds connected", t.handhelds.sessions.toString())
        }
        val notes = Traffic.notes(t, section.apName) { GridHub.apName(it) }
        for (n in notes.filter { it.red }.take(4)) {
            NoteLine(n.text, true)
        }
    }
}

/**
 * The second backbone (D71) on the Overview: one short line per AP that has a module, and nothing
 * whatever for an AP that has none — an AP is never expected to carry one.
 *
 * The best peer's signal comes from `/api/status` when this phone asked that AP (it has a line
 * per peer); otherwise from the last part the AP's radio received, which is all the packed
 * traffic record carries.
 */
@Composable
private fun LoraNow(sections: List<LinkData.Section<Traffic.Record>>, status: Status.Record?) {
    Section("LoRa backbone") {
        for (s in sections) {
            val l = s.data.lora ?: continue
            val peers = status?.lora?.takeIf { status.ap == s.data.ap }?.peers.orEmpty()
            val best = peers.filter { it.up }.maxByOrNull { it.rssi }
            val up = if (peers.isNotEmpty()) peers.any { it.up } else l.peersUp.isNotEmpty() && !l.silent
            val signal = when {
                best != null -> "${best.rssi} dBm from ${GridHub.apName(best.ap)}"
                l.heardS != null -> "${l.rssi} dBm, ${ago(l.heardS)}"
                else -> "nothing heard yet"
            }
            val red = !up || l.silent || l.trouble > 0 ||
                l.queueDepth >= Traffic.Limit.LORA_QUEUE_BAD
            Field(
                s.apName,
                (if (up) "up · " else "down · ") + signal,
                if (red) Lg.danger else Lg.accent,
            )
        }
    }
}

@Composable
private fun Header(snap: Snapshot?, watching: Boolean) {
    Column {
        val gt = snap?.gridTime
        Text(
            "Grid time " + (gt?.let {
                DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.MEDIUM).format(Date(it * 1000))
            } ?: "not heard"),
            color = Lg.muted, fontSize = 14.sp,
        )
        val line = if (snap == null) {
            "An offline network, watched from this phone without joining its Wi-Fi."
        } else {
            "${snap.apsHeard} of ${snap.aps.size} AP(s) heard" +
                if (watching) "; listening for ${forText(snap.listeningS)}." else "; not watching (last state shown)."
        }
        Text(line, color = Lg.muted, fontSize = 14.sp)
    }
}

// ---------------------------------------------------------------- start, stop, and what blocks it

@Composable
private fun WatchControl(watching: Boolean, radio: Radio, scanError: String?) {
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    LifecycleResumeEffect(Unit) {
        refresh++
        onPauseOrDispose { }
    }
    var explain by remember { mutableStateOf(false) }
    var denied by remember { mutableStateOf(false) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        refresh++
        if (GridHub.canScan(ctx)) {
            denied = false
            GridHub.startWatching(ctx)
        } else {
            denied = true
        }
    }
    // Re-read on every resume (settings may have changed) and whenever the service's state moves.
    val missing = remember(refresh, watching, radio) { GridHub.missingPermissions(ctx) }
    val notifOff = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
        Manifest.permission.POST_NOTIFICATIONS in missing
    val btOn = remember(refresh, watching, radio) {
        ctx.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true
    }
    val batteryLimited = remember(refresh, watching) {
        ctx.getSystemService(PowerManager::class.java)?.isIgnoringBatteryOptimizations(ctx.packageName) == false
    }

    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(if (watching) "Watching" else "Not watching", fontWeight = FontWeight.SemiBold, fontSize = 17.sp)
                Text(
                    if (watching) "Keeps listening in the background; an SOS raises a loud notification."
                    else "Listens over BLE only: never joins the grid and never transmits into it.",
                    color = Lg.muted, fontSize = 13.sp,
                )
            }
            Spacer(Modifier.width(12.dp))
            if (watching) {
                OutlinedButton(onClick = {
                    GridHub.setWantWatch(ctx, false)
                    GridHub.stopWatching(ctx)
                }) { Text("Stop") }
            } else {
                Button(onClick = {
                    if (missing.isEmpty()) GridHub.startWatching(ctx) else explain = true
                }) { Text("Start watching") }
            }
        }
        if (!btOn) {
            Notice("Bluetooth is off. Switch it on to hear the APs.", Lg.warn, "Bluetooth settings") {
                ctx.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            }
        }
        if (watching && radio == Radio.FAILED && scanError != null) {
            Notice("Scanning failed: $scanError", Lg.danger)
        }
        if (radio == Radio.NO_PERMISSION || (denied && Manifest.permission.BLUETOOTH_SCAN in missing)) {
            Notice("Nearby devices permission is off, so the app cannot listen for the APs.", Lg.danger, "App settings") {
                openAppSettings(ctx)
            }
        }
        if (notifOff && (watching || denied)) {
            Notice("Notifications are off: an SOS will not sound while the app is closed.", Lg.warn, "App settings") {
                openAppSettings(ctx)
            }
        }
        if (watching && batteryLimited) {
            Notice("Battery optimisation may pause watching when the screen is off. Set LocalGrid Watch to Unrestricted.",
                Lg.muted, "Battery settings") {
                ctx.startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
            }
        }
    }

    if (explain) {
        AlertDialog(
            onDismissRequest = { explain = false },
            title = { Text("Allow listening") },
            text = {
                Text(
                    "LocalGrid Watch needs two permissions:\n\n" +
                        "• Nearby devices, to hear the APs' Bluetooth status beacon. The app only listens: " +
                        "it never transmits into the grid and does not use your location.\n\n" +
                        "• Notifications, to show that it is watching and to sound an SOS while the app is closed.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    explain = false
                    launcher.launch(missing.toTypedArray())
                }) { Text("Continue") }
            },
            dismissButton = { TextButton(onClick = { explain = false }) { Text("Not now") } },
        )
    }
}

fun openAppSettings(ctx: Context) {
    val i = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", ctx.packageName, null))
    if (ctx !is Activity) i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    ctx.startActivity(i)
}

@Composable
fun Notice(text: String, color: Color, action: String? = null, onAction: () -> Unit = {}) {
    Row(Modifier.fillMaxWidth().padding(top = 10.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(text, color = color, fontSize = 14.sp, modifier = Modifier.weight(1f))
        if (action != null) TextButton(onClick = onAction) { Text(action) }
    }
}

// ---------------------------------------------------------------- alert banner

@Composable
private fun Banner(a: AlertView?) {
    val (bg, fg, text, strong) = when {
        a == null -> Quad(Lg.surface, Lg.muted, "No alert heard.", false)
        a.allClear -> Quad(Lg.accent, Lg.accentInk, "All clear: ${a.name} is safe (${ago(a.ageS)})", true)
        a.active -> Quad(Lg.danger, Lg.dangerInk,
            "SOS from ${a.name} near ${a.near}, ${a.ageS / 60} min ago" + (a.reads?.let { ", read by $it" } ?: ""), true)
        else -> Quad(Lg.surface, Lg.muted,
            "Last SOS from ${a.name} near ${a.near}, ${a.ageS / 60} min ago (no longer active)" +
                (a.reads?.let { ", read by $it" } ?: ""), false)
    }
    Box(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(bg)
            .border(1.dp, if (strong) bg else Lg.line, RoundedCornerShape(10.dp))
            .padding(horizontal = 16.dp, vertical = 14.dp),
    ) {
        Text(text, color = fg, fontSize = if (strong) 18.sp else 15.sp,
            fontWeight = if (strong) FontWeight.SemiBold else FontWeight.Normal)
    }
}

private data class Quad(val bg: Color, val fg: Color, val text: String, val strong: Boolean)

// ---------------------------------------------------------------- AP cards

@Composable
private fun ApCard(a: ApView) {
    Card(Modifier.alpha(if (a.heard) 1f else 0.6f)) {
        Row(verticalAlignment = Alignment.Bottom) {
            Text(a.name, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.width(8.dp))
            Text("AP ${a.index}", color = Lg.muted, fontSize = 13.sp)
        }
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 4.dp)) {
            if (a.heard) {
                Pill("heard", Lg.accent, Lg.accentInk)
                a.rssi?.let { Text("  $it dBm", fontSize = 14.sp) }
                // Always say how long ago the last advert arrived: Bluetooth shares the antenna
                // with Wi-Fi, so quiet spells happen, and they should be visible well before the
                // AP is called lost.
                val quiet = a.silentS ?: 0
                if (quiet >= 2) {
                    Text(
                        "  last heard $quiet s ago", fontSize = 13.sp,
                        color = if (quiet >= 15) Lg.warn else Lg.muted,
                    )
                }
            } else {
                Pill("not heard", Lg.danger, Lg.dangerInk)
                Text("  for ${forText(a.silentS)}", fontSize = 14.sp)
            }
        }
        Spacer(Modifier.height(8.dp))
        val h = a.health
        if (h != null) {
            Field("Uptime", h.uptime)
            Field("Links", if (h.links.isEmpty()) "none" else h.links.joinToString(", "), if (h.links.isEmpty()) Lg.warn else null)
            Field("Handhelds", when {
                a.hereNames.isNotEmpty() -> a.hereNames.joinToString(", ")
                h.handheldsHere > 0 -> "${h.handheldsHere} registered"
                else -> "none"
            })
            Field("Time", h.time + if (h.timeGps) "  ⌖ GPS" else "", if (h.timeQuality == "unset") Lg.warn else if (h.timeGps) Lg.accent else null)
            Field("GPS", when {
                !h.gps.fitted -> "not fitted"
                h.gps.fix -> "fix, ${h.gps.sats} satellites"
                else -> "no fix (${h.gps.sats} satellites)"
            }, if (h.gps.fix) Lg.accent else null)
            Field("Lowest heap", (if (h.heapSat) "255+ " else "${h.heapKb} ") + "KB",
                if (h.heapKb < 20) Lg.danger else if (h.heapKb < 40) Lg.warn else null)
            Field("Restarts", "${h.restarts}, brownouts ${h.brownouts}", if (h.brownouts > 0) Lg.warn else null)
            Field("Last reset", h.reset, if (h.reset.contains("crash") || h.reset.contains("brownout")) Lg.danger else null)
            Field("Status", if (a.statusAgeS == null) "none" else ago(a.statusAgeS),
                when {
                    (a.statusAgeS ?: 0) > 45 -> Lg.danger
                    (a.statusAgeS ?: 0) > 15 -> Lg.warn
                    else -> null
                })
        } else {
            a.disc?.let { d ->
                Field("Backbone", if (d.backbone) "linked" else "no link", if (d.backbone) null else Lg.warn)
                Field("Grid time", if (d.time) "held" else "not held")
                Field("Handhelds", "${d.attached} attached, ${d.freeSlots} slots free")
            }
            Field("Status",
                if (a.badTag > 0) "cannot be verified (${a.badTag} frames): wrong pairing code?" else "none yet (discovery advert only)",
                if (a.badTag > 0) Lg.danger else Lg.warn)
        }
    }
}

// ---------------------------------------------------------------- handhelds heard on the beacon

@Composable
private fun Handhelds(list: List<HandheldView>) {
    if (list.isEmpty()) {
        Hint("None heard yet.")
        return
    }
    Row(Modifier.fillMaxWidth().padding(bottom = 4.dp)) {
        Text("Name", color = Lg.muted, fontSize = 13.sp, modifier = Modifier.weight(1.3f))
        Text("AP", color = Lg.muted, fontSize = 13.sp, modifier = Modifier.weight(1f))
        Text("Battery", color = Lg.muted, fontSize = 13.sp, modifier = Modifier.weight(1.2f))
    }
    for (h in list) {
        Divider()
        Row(Modifier.fillMaxWidth().padding(vertical = 7.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1.3f)) {
                Text(h.name, fontSize = 15.sp)
                Text(if (h.online) "online" else "offline", color = if (h.online) Lg.accent else Lg.muted, fontSize = 12.sp)
            }
            Text(h.apName ?: "–", fontSize = 15.sp, modifier = Modifier.weight(1f))
            Row(Modifier.weight(1.2f), verticalAlignment = Alignment.CenterVertically) {
                val b = h.battery
                if (b == null) {
                    Text("–", color = Lg.muted)
                } else {
                    BatteryBar(b)
                    Text(" $b%", fontSize = 14.sp)
                }
            }
        }
    }
}

// ---------------------------------------------------------------- events

private fun eventColor(kind: String) = when (kind) {
    "alert", "ap_lost", "bad_key" -> Lg.danger
    "all_clear", "ap_heard" -> Lg.accent
    "time_source", "ap_restart", "brownout" -> Lg.warn
    else -> Lg.text
}

@Composable
private fun Events(snap: Snapshot?) {
    val events: List<GridEvent> = snap?.events.orEmpty().take(30)
    if (events.isEmpty()) Hint("Nothing yet.")
    val fmt = remember { DateFormat.getTimeInstance(DateFormat.MEDIUM) }
    for (e in events) {
        Text(fmt.format(Date((e.t * 1000).toLong())) + "  " + e.text, color = eventColor(e.kind),
            fontFamily = FontFamily.Monospace, fontSize = 12.sp, modifier = Modifier.padding(vertical = 2.dp))
    }
    val s = snap?.stats.orEmpty()
    Spacer(Modifier.height(8.dp))
    Hint("Status frames verified ${s["ok"] ?: 0}, failed the check ${s["bad_tag"] ?: 0}, " +
        "replays dropped ${s["replay"] ?: 0}, other grids ignored ${s["other_grid"] ?: 0}.")
}
