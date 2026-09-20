package org.localgrid.gridwatch.map

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * The arithmetic the Map tab needs, drawn on this phone and nowhere else.
 *
 * Coordinates never appear in a URL, a query string or a referrer: the only thing fetched is a
 * map tile, and a tile is asked for by its z/x/y grid square (see [TileUrl]).
 */
object MapMath {
    const val TILE = 256.0

    data class Point(val x: Double, val y: Double)

    /** Web Mercator pixel position at zoom [z], the projection OpenStreetMap tiles use. */
    fun worldPx(lat: Double, lon: Double, z: Int): Point {
        val n = TILE * Math.pow(2.0, z.toDouble())
        val r = lat * PI / 180.0
        return Point(
            x = (lon + 180.0) / 360.0 * n,
            y = (1.0 - ln(tan(r) + 1.0 / cos(r)) / PI) / 2.0 * n,
        )
    }

    data class Bearing(val metres: Double, val degrees: Double)

    /** Great-circle distance and initial bearing from a to b. */
    fun distBearing(aLat: Double, aLon: Double, bLat: Double, bLon: Double): Bearing {
        val r = 6371000.0
        val rad = PI / 180.0
        val f1 = aLat * rad
        val f2 = bLat * rad
        val df = (bLat - aLat) * rad
        val dl = (bLon - aLon) * rad
        val h = sin(df / 2) * sin(df / 2) + cos(f1) * cos(f2) * sin(dl / 2) * sin(dl / 2)
        val d = 2 * r * asin(min(1.0, sqrt(h)))
        val brg = atan2(sin(dl) * cos(f2), cos(f1) * sin(f2) - sin(f1) * cos(f2) * cos(dl))
        return Bearing(d, (brg / rad + 360.0) % 360.0)
    }

    val COMPASS = listOf("N", "NE", "E", "SE", "S", "SW", "W", "NW")

    fun compass(degrees: Double): String = COMPASS[(degrees / 45.0).roundToInt() % 8]

    fun fmtDist(m: Double): String = when {
        m < 1000 -> "${m.roundToInt()} m"
        m < 10000 -> String.format(java.util.Locale.US, "%.2f km", m / 1000)
        else -> String.format(java.util.Locale.US, "%.1f km", m / 1000)
    }

    /** Degrees, minutes and seconds, as the laptop dashboard writes them. */
    fun dms(v: Double, pos: String, neg: String): String {
        val a = abs(v)
        val d = floor(a).toInt()
        val mf = (a - d) * 60
        val m = floor(mf).toInt()
        val s = String.format(java.util.Locale.US, "%.1f", (mf - m) * 60)
        return "$d° $m' $s\" " + if (v >= 0) pos else neg
    }

    fun fmtCoord(lat: Double, lon: Double): String =
        String.format(java.util.Locale.US, "%.5f, %.5f", lat, lon)
}

/**
 * Where a tile comes from. This is the app's only URL, and it is built from the tile's grid
 * numbers alone: no coordinate, no query string, nothing about this phone or this grid. The
 * standard OpenStreetMap attribution goes on the map.
 */
object TileUrl {
    const val HOST = "https://tile.openstreetmap.org"
    const val USER_AGENT =
        "LocalGrid-Watch/0.2.3 (offline network monitor; one phone, only the tiles on screen)"
    const val ATTRIBUTION = "© OpenStreetMap contributors"
    const val MAX_ZOOM = 19

    fun valid(z: Int, x: Int, y: Int): Boolean {
        if (z !in 0..MAX_ZOOM) return false
        val n = 1 shl z
        return x in 0 until n && y in 0 until n
    }

    /** "https://tile.openstreetmap.org/z/x/y.png" and nothing else. */
    fun of(z: Int, x: Int, y: Int): String {
        require(valid(z, x, y)) { "tile out of range" }
        return "$HOST/$z/$x/$y.png"
    }

    /**
     * The link a person can copy and open later on a device with Internet. It is text on the
     * clipboard, never something this app fetches or opens by itself, so no coordinate of this
     * grid is ever sent anywhere by the phone.
     */
    fun copyableOsmLink(lat: Double, lon: Double): String {
        val la = String.format(java.util.Locale.US, "%.6f", lat)
        val lo = String.format(java.util.Locale.US, "%.6f", lon)
        return "https://www.openstreetmap.org/?mlat=$la&mlon=$lo#map=17/$la/$lo"
    }
}
