package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.map.MapMath
import org.localgrid.gridwatch.map.TileUrl
import java.io.File
import kotlin.math.floor

/**
 * No coordinate of this grid may appear in any URL the app builds (D70, privacy).
 *
 * The app fetches exactly one kind of thing from the Internet — a map tile — and asks for it by
 * its z/x/y grid square. This test builds the tiles for real positions and checks that not one
 * digit of those coordinates is in the address, and then reads the app's own sources to make
 * sure no other URL has crept in.
 */
class MapUrlTest {
    private val places = listOf(
        47.623450 to 13.045670,        // the sample grid's MAIN
        47.625100 to 13.048900,
        -33.856159 to 151.215256,
        51.500729 to -0.124625,
        0.0 to 0.0,
    )

    @Test fun aTileAddressIsOnlyItsGridSquare() {
        for ((lat, lon) in places) {
            for (z in listOf(10, 14, 17, 19)) {
                val p = MapMath.worldPx(lat, lon, z)
                val x = floor(p.x / MapMath.TILE).toInt()
                val y = floor(p.y / MapMath.TILE).toInt()
                val url = TileUrl.of(z, x, y)
                assertEquals("${TileUrl.HOST}/$z/$x/$y.png", url)
                assertFalse("a tile URL must carry no query string: $url", url.contains("?"))
                assertFalse("a tile URL must carry no fragment: $url", url.contains("#"))
                for (text in coordinateSpellings(lat) + coordinateSpellings(lon)) {
                    assertFalse("\"$text\" must not appear in $url", url.contains(text))
                }
            }
        }
    }

    /** Every way a coordinate could leak into a string: full, rounded, and in micro-degrees. */
    private fun coordinateSpellings(v: Double): List<String> = listOf(
        v.toString(),
        String.format(java.util.Locale.US, "%.6f", v),
        String.format(java.util.Locale.US, "%.5f", v),
        String.format(java.util.Locale.US, "%.4f", v),
        String.format(java.util.Locale.US, "%.3f", v),
        Math.round(v * 1e6).toString(),
    ).filter { it.length >= 4 }

    @Test fun theCopyableLinkIsTextForAPersonNotSomethingTheAppFetches() {
        // This one does carry the coordinate, on purpose: it goes on the clipboard so a person
        // can open it later on a device with Internet. The app never fetches or opens it, and
        // nothing in the app passes it to a network call.
        val link = TileUrl.copyableOsmLink(47.623450, 13.045670)
        assertTrue(link.startsWith("https://www.openstreetmap.org/?mlat="))
        val sources = appSources()
        val fetchers = sources.filter { it.readText().contains("HttpURLConnection") }
        assertEquals(1, fetchers.size)
        assertEquals("Tiles.kt", fetchers[0].name)
        assertFalse(
            "the tile fetcher must not know about the copyable link",
            fetchers[0].readText().contains("copyableOsmLink"),
        )
    }

    @Test fun theAppBuildsNoOtherUrl() {
        val allowed = setOf(
            "https://tile.openstreetmap.org",                     // TileUrl.HOST
            "https://tile.openstreetmap.org/z/x/y.png",            // the doc comment beside it
            "https://www.openstreetmap.org/?mlat=",                // the copyable text link
        )
        val found = LinkedHashMap<String, String>()
        for (file in appSources()) {
            for (line in file.readLines()) {
                var i = line.indexOf("http")
                while (i >= 0) {
                    val rest = line.substring(i)
                    val url = rest.takeWhile { it != '"' && it != ' ' && it != '$' }
                    if (allowed.none { url.startsWith(it) } && !url.startsWith("http://schemas.android.com")) {
                        found[url] = file.name
                    }
                    i = line.indexOf("http", i + 4)
                }
            }
        }
        assertEquals("the app must build no URL but a map tile's: $found", emptyMap<String, String>(), found)
    }

    private fun appSources(): List<File> {
        val root = File("src/main/java")
        assertTrue("the app's sources should be beside the tests: ${root.absolutePath}", root.isDirectory)
        return root.walkTopDown().filter { it.isFile && it.extension == "kt" }.toList()
    }

    @Test fun distancesAndBearingsMatchTheLaptopDashboard() {
        val db = MapMath.distBearing(47.623450, 13.045670, 47.625100, 13.048900)
        assertEquals(305.0, db.metres, 5.0)
        assertEquals("NE", MapMath.compass(db.degrees))
        assertEquals("305 m", MapMath.fmtDist(305.0))
        assertEquals("1.20 km", MapMath.fmtDist(1200.0))
        assertEquals("12.0 km", MapMath.fmtDist(12000.0))
        assertEquals("47.62345, 13.04567", MapMath.fmtCoord(47.623450, 13.045670))
        assertEquals("47° 37' 24.4\" N", MapMath.dms(47.623450, "N", "S"))
        assertEquals("0° 7' 28.7\" W", MapMath.dms(-0.124625, "E", "W"))
    }
}
