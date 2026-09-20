package org.localgrid.gridwatch.map

import org.localgrid.gridwatch.link.Status

/**
 * One thing to draw on the map: an AP or a handheld, where it was, and when its fix was taken.
 *
 * Built from the admin page's own status reply, which is the only place a coordinate comes from
 * (positions stay out of the beacon, D68). Nothing here is ever sent anywhere.
 */
data class Pin(
    val isAp: Boolean, val id: Int, val lat: Double, val lon: Double, val name: String,
    val fixTime: Long, val sats: Int, val live: Boolean,
)

fun pinsOf(status: Status.Record): List<Pin> {
    val out = status.positions.map {
        Pin(
            isAp = it.isAp, id = it.subject, lat = it.lat, lon = it.lon,
            name = it.name ?: if (it.isAp) "AP ${it.subject}" else "Handheld ${it.subject}",
            fixTime = it.fixTime, sats = it.sats, live = false,
        )
    }.toMutableList()
    val g = status.gps
    if (g != null && g.hasPos) {
        val own = out.indexOfFirst { it.isAp && it.id == status.ap }
        val live = Pin(
            isAp = true, id = status.ap, lat = g.latU / 1e6, lon = g.lonU / 1e6,
            name = status.apName, sats = g.sats,
            fixTime = if (own >= 0) out[own].fixTime else 0, live = g.fix,
        )
        if (own >= 0) out[own] = live else out.add(live)
    }
    return out
}
