package org.localgrid.gridwatch.grid

import org.localgrid.gridwatch.proto.Discovery
import org.localgrid.gridwatch.proto.StatusFrame
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToLong

const val MAX_APS = 8                   // LG_MAX_NODES
/**
 * How long an AP may go unheard before it is shown as "not heard".
 *
 * Measured on the bench (2026-09-20): Bluetooth shares the radio and antenna with Wi-Fi, so a
 * watching laptop or phone misses adverts in bursts. Over 150 s, adverts from each AP normally
 * arrived every 0.25 s, but there were 10 to 13 quiet spells of 5 s or more per AP, the longest
 * 14.6 s, and 10 to 14 of those spells fell at the same moment on two different APs. Three
 * boards do not stop transmitting in unison, so the gaps are this receiver, not the grid.
 *
 * A 10 to 15 s threshold therefore produces constant false "AP lost / AP heard again" churn.
 * 45 s is about three times the worst measured gap. The AP card still shows how long ago the
 * last advert arrived, so a real quiet spell is visible long before this runs out.
 */
const val AP_SILENT_S = 45.0

/**
 * How long a handhelds or alert frame goes on counting.
 *
 * A handheld's presence reaches this phone only through an AP's rotating beacon: the AP sends
 * health every other frame and takes turns between handhelds, alert, battery and name, so one
 * handhelds frame arrives about every 4 s per AP (docs/ble-status.md). A 14.6 s receiver
 * blackout can swallow four of them in a row, and one blackout can follow another.
 *
 * So this one is more forgiving than the AP's: three times (the worst measured blackout plus one
 * turn of the rotation) is about 56 s, rounded up to 90 s. A handheld vanishing from the list is
 * a more alarming false signal than an AP flickering, and nothing urgent depends on this timer —
 * an alert frame raises its notification the moment it arrives, whatever these timers say.
 */
const val FRAME_FRESH_S = 90.0

val TIME_QUALITY = mapOf(0 to "unset", 1 to "carried", 2 to "authoritative", 3 to "unknown")

// esp_reset_reason_t, worded as components/lg_power does.
val RESET_WORDS = mapOf(
    0 to "unknown", 1 to "power-on or reset", 2 to "external reset", 3 to "software restart",
    4 to "crash (panic)", 5 to "crash (interrupt watchdog)", 6 to "crash (task watchdog)",
    7 to "crash (other watchdog)", 8 to "wake from deep sleep",
    9 to "low supply voltage (brownout)", 10 to "SDIO reset", 11 to "USB reset",
    12 to "JTAG reset", 13 to "eFuse error", 14 to "power glitch", 15 to "CPU lock-up",
)

fun minutesText(m: Int): String {
    if (m >= 0xFFFF) return "45 d or more"
    val d = m / 1440
    val h = (m % 1440) / 60
    val mi = m % 60
    return if (d > 0) "$d d $h h" else if (h > 0) "$h h $mi min" else "$mi min"
}

fun timeText(q: Int, gps: Boolean, stratum: Int): String {
    if (q == 0) return "no grid time"
    var s = TIME_QUALITY[q] ?: "?"
    if (gps) s += ", from GPS"
    s += if (stratum >= 31) ", stratum unknown" else ", stratum $stratum"
    return s
}

/** Python's round(): half to even does not matter for display; this rounds half up. */
private fun rnd(x: Double): Long = x.roundToLong()

data class GridEvent(
    val t: Double, val kind: String, val text: String,
    val ap: Int? = null, val device: Int? = null,
    /** For alert and all_clear: who and near which AP, for the notification. */
    val name: String? = null, val near: String? = null, val ageS: Int? = null,
)

data class DiscView(val backbone: Boolean, val time: Boolean, val attached: Int, val freeSlots: Int)
data class GpsView(val fitted: Boolean, val fix: Boolean, val sats: Int)
data class HealthView(
    val uptime: String, val links: List<String>, val handheldsHere: Int, val time: String,
    val timeQuality: String, val timeGps: Boolean, val heapKb: Int, val heapSat: Boolean,
    val reset: String, val restarts: Int, val brownouts: Int, val gps: GpsView,
)
data class ApView(
    val index: Int, val name: String, val heard: Boolean, val silentS: Long?, val rssi: Int?,
    val statusAgeS: Long?, val framesOk: Int, val badTag: Int, val boot: Long?,
    val here: List<Int>, val hereNames: List<String>, val disc: DiscView?, val health: HealthView?,
)
data class HandheldView(
    val device: Int, val name: String, val online: Boolean, val ap: Int?, val apName: String?,
    val battery: Int?, val batteryAgeS: Long?,
)
data class AlertView(
    val id: Int, val active: Boolean, val allClear: Boolean, val device: Int, val name: String,
    val near: String, val ageS: Long, val reads: Int?,
)
data class Snapshot(
    val now: Double, val gridTime: Long?, val listeningS: Long, val aps: List<ApView>,
    val handhelds: List<HandheldView>, val alert: AlertView?, val events: List<GridEvent>,
    val stats: Map<String, Int>,
) {
    val apsHeard: Int get() = aps.count { it.heard }
}

/**
 * Everything heard, keyed by AP index and device number, a port of GridState in
 * tools/grid_watch.py. Never holds a BLE address (D21). Times are seconds on the wall clock.
 */
class GridState(
    private val k: ByteArray,
    private val disc: ByteArray,
    private val onEvent: ((GridEvent) -> Unit)? = null,
    private val started: Double = System.currentTimeMillis() / 1000.0,
) {
    private class Ap(val index: Int, val first: Double) {
        var last = 0.0
        var rssi: Int? = null
        var heard = false
        var disc: Discovery? = null
        var advName: String? = null
        var statusLast: Double? = null
        var boot: Long? = null
        var counter = -1
        var health: StatusFrame.Health? = null
        var handhelds: StatusFrame.Handhelds? = null
        var hhAt = 0.0
        var alert: StatusFrame.Alert? = null
        var alertAt = 0.0
        var ok = 0
        var badTag = 0
        var warned = false
        var ever = false
    }

    private class Best(val carried: Double, val ap: Int, val al: StatusFrame.Alert, var reads: Int?)

    private val lock = Any()
    private val aps = LinkedHashMap<Int, Ap>()
    private val hhNames = HashMap<Int, Pair<String, Double>>()
    private val batteries = HashMap<Int, Triple<Int, Int, Double>>()   // percent, AP, heard at
    private var alertSeq = 0
    private var alertKey: Pair<Triple<Int, Boolean, Boolean>, Double>? = null
    private var gridOffset: Double? = null
    private val events = ArrayDeque<GridEvent>()
    private val stats = HashMap<String, Int>()

    private fun count(key: String) { stats[key] = (stats[key] ?: 0) + 1 }

    fun stat(key: String): Int = synchronized(lock) { stats[key] ?: 0 }

    // -- events

    private fun event(e: GridEvent) {
        events.addLast(e)
        while (events.size > 200) events.removeFirst()
        onEvent?.invoke(e)
    }

    private fun apName(ap: Int): String = aps[ap]?.advName ?: "AP $ap"

    private fun hhName(dev: Int): String = hhNames[dev]?.first ?: "Handheld $dev"

    private fun ap(index: Int, now: Double): Ap = aps.getOrPut(index) { Ap(index, now) }

    private fun heard(a: Ap, rssi: Int?, now: Double) {
        a.last = now
        if (rssi != null) a.rssi = rssi
        if (!a.heard) {
            a.heard = true
            val again = a.ever
            a.ever = true
            event(GridEvent(now, "ap_heard", "${apName(a.index)} (AP ${a.index}) " +
                if (again) "heard again" else "heard", ap = a.index))
        }
    }

    // -- input

    /** One BLE event: the manufacturer payloads under the grid's company ID it carried. */
    fun onAdvert(payloads: List<ByteArray>, rssi: Int?, now: Double = System.currentTimeMillis() / 1000.0) {
        synchronized(lock) {
            val discs = payloads.mapNotNull { Discovery.parse(it) }
            val ours = discs.filter { it.disc.contentEquals(disc) }
            if (discs.isNotEmpty() && ours.isEmpty()) {
                count("other_grid")                   // another LocalGrid network: not ours
                return
            }
            for (d in ours) {
                if (d.ap >= MAX_APS) continue
                val a = ap(d.ap, now)
                if (d.name != null) a.advName = d.name
                a.disc = d
                heard(a, rssi, now)
            }
            val paired = ours.isNotEmpty()
            for (p in payloads) {
                if (p.isNotEmpty() && (p[0].toInt() and 0xFF) == StatusFrame.MAGIC) status(p, rssi, now, paired)
            }
        }
    }

    private fun status(frame: ByteArray, rssi: Int?, now: Double, paired: Boolean) {
        val got = StatusFrame.open(k, frame)
        if (got is StatusFrame.Rejected) {
            count("bad_" + got.reason)
            // A frame paired with this grid's discovery advert that fails its check means the
            // key is wrong or the firmware differs; one without a pairing may be anyone's.
            if (paired && got.reason == "tag" && frame.size > 1) {
                val idx = frame[1].toInt() and 0x0F
                val a = aps[idx]
                if (a != null) {
                    a.badTag++
                    if (a.badTag >= 5 && a.ok == 0 && !a.warned) {
                        a.warned = true
                        event(GridEvent(now, "bad_key", "${apName(idx)} (AP $idx) sends status " +
                            "this phone cannot verify: the pairing code is not this grid's, or " +
                            "the AP runs a different frame format", ap = idx))
                    }
                }
            }
            return
        }
        val f = got as StatusFrame.Frame
        if (f.ap >= MAX_APS) {
            count("bad_ap")
            return
        }
        val a = ap(f.ap, now)
        // Replay rule: within one boot the counter must rise. A higher boot is a restart; a
        // lower one is an old recording. The same counter again is a re-delivered scan
        // response, dropped without counting as a replay.
        val lastBoot = a.boot
        if (lastBoot != null) {
            if (f.boot < lastBoot || (f.boot == lastBoot && f.counter < a.counter)) {
                count("replay")
                return
            }
            if (f.boot == lastBoot && f.counter == a.counter) {
                count("duplicate")
                return
            }
        }
        val body = StatusFrame.decode(f.type, f.plaintext)
        if (body == null) {
            count("bad_body")
            return
        }
        if (lastBoot != null && f.boot > lastBoot) {
            event(GridEvent(now, "ap_restart", "${apName(f.ap)} (AP ${f.ap}) restarted " +
                "(boot $lastBoot -> ${f.boot})", ap = f.ap))
        }
        a.boot = f.boot
        a.counter = f.counter
        a.ok++
        a.statusLast = now
        count("ok")
        heard(a, rssi, now)

        when (body) {
            is StatusFrame.Health -> {
                val old = a.health
                if (old != null && (old.timeQuality != body.timeQuality || old.timeGps != body.timeGps)) {
                    event(GridEvent(now, "time_source", "${apName(f.ap)} (AP ${f.ap}) time: " +
                        "${timeText(old.timeQuality, old.timeGps, old.stratum)} -> " +
                        timeText(body.timeQuality, body.timeGps, body.stratum), ap = f.ap))
                }
                if (old != null && body.brownouts != old.brownouts) {
                    event(GridEvent(now, "brownout", "${apName(f.ap)} (AP ${f.ap}) brownouts now " +
                        "${body.brownouts}", ap = f.ap))
                }
                a.health = body
            }
            is StatusFrame.Handhelds -> { a.handhelds = body; a.hhAt = now }
            is StatusFrame.Alert -> {
                a.alert = body
                a.alertAt = now
                if (body.gridTime != 0L) gridOffset = body.gridTime - now
                alertChanged(now)
            }
            is StatusFrame.Batteries -> for ((dev, pct) in body.batteries) batteries[dev] = Triple(pct, f.ap, now)
            is StatusFrame.Name -> if (body.device != 0) hhNames[body.device] = body.name to now
        }
    }

    // -- merging

    /** The newest urgent broadcast any AP heard recently reports, with its read count. */
    private fun newestAlert(now: Double): Best? {
        var best: Best? = null
        for (a in aps.values) {
            val al = a.alert ?: continue
            if (al.author == 0 || now - a.alertAt > FRAME_FRESH_S) continue
            val carried = a.alertAt - al.ageS
            if (best == null || carried > best.carried + 2) best = Best(carried, a.index, al, null)
        }
        val b = best ?: return null
        val reads = aps.values.mapNotNull { a ->
            val al = a.alert
            if (al != null && al.author == b.al.author && now - a.alertAt <= FRAME_FRESH_S &&
                al.reads != 255 && abs(a.alertAt - al.ageS - b.carried) <= 5) al.reads else null
        }
        b.reads = reads.maxOrNull()
        return b
    }

    private fun alertChanged(now: Double) {
        val al = newestAlert(now) ?: return
        val key = Triple(al.al.author, al.al.allClear, al.al.active)
        val prev = alertKey
        if (prev != null && key == prev.first && abs(al.carried - prev.second) <= 5) return
        alertKey = key to al.carried
        val who = hhName(al.al.author)
        if (al.al.allClear) {
            event(GridEvent(now, "all_clear", "ALL CLEAR: $who is safe", device = al.al.author,
                name = who, near = near(al, now), ageS = al.al.ageS))
        } else if (al.al.active) {
            alertSeq++
            val near = near(al, now)
            event(GridEvent(now, "alert", "SOS from $who near $near, ${al.al.ageS / 60} min ago",
                device = al.al.author, name = who, near = near, ageS = al.al.ageS))
        }
    }

    private fun near(al: Best, now: Double): String {
        val loc = locations(now)[al.al.author]
        return apName(loc ?: al.ap)
    }

    /** device -> AP index, from the newest fresh handhelds frame that places it. */
    private fun locations(now: Double): Map<Int, Int> {
        val best = HashMap<Int, Pair<Int, Double>>()
        for (a in aps.values) {
            val hh = a.handhelds ?: continue
            if (now - a.hhAt > FRAME_FRESH_S) continue
            for ((dev, where) in hh.where) {
                if (where != null && dev in hh.online) {
                    val b = best[dev]
                    if (b == null || a.hhAt > b.second) best[dev] = where to a.hhAt
                }
            }
        }
        return best.mapValues { it.value.first }
    }

    fun tick(now: Double = System.currentTimeMillis() / 1000.0) {
        synchronized(lock) {
            for (a in aps.values) {
                if (a.heard && now - a.last > AP_SILENT_S) {
                    a.heard = false
                    event(GridEvent(now, "ap_lost", "${apName(a.index)} (AP ${a.index}) " +
                        "not heard for ${AP_SILENT_S.toInt()} s", ap = a.index))
                }
            }
        }
    }

    // -- output

    fun snapshot(now: Double = System.currentTimeMillis() / 1000.0): Snapshot = synchronized(lock) {
        val online = HashSet<Int>()
        for (a in aps.values) {
            val hh = a.handhelds
            if (hh != null && now - a.hhAt <= FRAME_FRESH_S) online += hh.online
        }
        val where = locations(now)
        val devices = (online + batteries.keys + hhNames.keys + where.keys).sorted()
        val handhelds = devices.map { d ->
            val bat = batteries[d]
            HandheldView(
                device = d, name = hhName(d), online = d in online, ap = where[d],
                apName = where[d]?.let { apName(it) },
                battery = if (bat == null || bat.first == 255) null else minOf(bat.first, 100),
                batteryAgeS = bat?.let { rnd(now - it.third) },
            )
        }

        val apViews = aps.keys.sorted().map { i ->
            val a = aps.getValue(i)
            val h = a.health
            val d = a.disc
            val here = where.filter { it.value == i }.keys.sorted()
            ApView(
                index = i, name = apName(i), heard = a.heard,
                silentS = if (a.last > 0) rnd(now - a.last) else null, rssi = a.rssi,
                statusAgeS = a.statusLast?.let { rnd(now - it) }, framesOk = a.ok, badTag = a.badTag,
                boot = a.boot, here = here, hereNames = here.map { hhName(it) },
                disc = d?.let {
                    DiscView(it.flags and Discovery.FLAG_BACKBONE != 0, it.flags and Discovery.FLAG_TIME != 0,
                        it.attached, it.freeSlots)
                },
                health = h?.let {
                    HealthView(
                        uptime = minutesText(it.uptimeMin),
                        links = (0 until MAX_APS).filter { n -> it.links and (1 shl n) != 0 }.map { n -> apName(n) },
                        handheldsHere = it.handheldsHere,
                        time = timeText(it.timeQuality, it.timeGps, it.stratum),
                        timeQuality = TIME_QUALITY[it.timeQuality] ?: "?", timeGps = it.timeGps,
                        heapKb = it.heapKb, heapSat = it.heapKb >= 255,
                        reset = RESET_WORDS[it.resetReason] ?: "reason ${it.resetReason}",
                        restarts = it.restarts, brownouts = it.brownouts,
                        gps = GpsView(it.gpsFitted, it.gpsFix, it.gpsSats),
                    )
                },
            )
        }

        val al = newestAlert(now)
        val alert = al?.let {
            val loc = where[it.al.author]
            AlertView(
                id = alertSeq, active = it.al.active, allClear = it.al.allClear, device = it.al.author,
                name = hhName(it.al.author), near = apName(loc ?: it.ap),
                ageS = max(0L, rnd(now - it.carried)), reads = it.reads,
            )
        }
        Snapshot(
            now = now, gridTime = gridOffset?.let { rnd(now + it) }, listeningS = rnd(now - started),
            aps = apViews, handhelds = handhelds, alert = alert,
            events = events.toList().takeLast(60).reversed(), stats = HashMap(stats),
        )
    }
}
