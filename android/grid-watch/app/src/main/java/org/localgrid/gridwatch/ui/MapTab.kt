package org.localgrid.gridwatch.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.withTimeoutOrNull
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.map.MapMath
import org.localgrid.gridwatch.map.Pin
import org.localgrid.gridwatch.map.pinsOf
import org.localgrid.gridwatch.map.TileUrl
import org.localgrid.gridwatch.map.Tiles
import org.localgrid.gridwatch.watch.LinkHub
import org.localgrid.gridwatch.watch.LinkView
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * The Map tab (D65, D70): everyone's coordinates, drawn on this phone.
 *
 * The phone often has mobile data although the grid has none, so with Internet it draws real
 * OpenStreetMap tiles, and without it a plan with distances and bearings from MAIN, north up.
 * Either way the coordinates are shown as text and can be copied. Nothing about this grid is
 * ever in a URL: a tile is asked for by its z/x/y square and nothing else.
 */
/** How long one round of tile fetching may take before the plan simply stays. */
private const val TILE_ROUND_MS = 5_000L

fun LazyListScope.mapTab(link: LinkView, nowMs: Long) {
    val status = link.status
    if (status == null) {
        item { Section("Map") { Hint("Waiting for the first answer from an AP…") } }
        return
    }
    val pins = pinsOf(status.data)
    item {
        Section("Map", sectionAge(status, nowMs)) {
            if (pins.isEmpty()) {
                Hint(
                    "No positions yet. MAIN appears here once its GPS has a fix, and a handheld " +
                        "once its own GPS does.",
                )
            } else {
                MapBody(pins, status.data, nowMs)
            }
        }
    }
    item { LogOutRow() }
}

@Composable
private fun MapBody(pins: List<Pin>, status: Status.Record, nowMs: Long) {
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val tilesOn by LinkHub.tilesOn.collectAsState()
    val main = pins.firstOrNull { it.isAp && it.id == 0 } ?: pins.firstOrNull { it.isAp }
    val nowS = if (status.gridTime > 0) status.gridTime else nowMs / 1000

    if (main != null) {
        Text(
            "${main.name}: ${MapMath.fmtCoord(main.lat, main.lon)} · " +
                "${MapMath.dms(main.lat, "N", "S")}, ${MapMath.dms(main.lon, "E", "W")} · " +
                if (main.live) "GPS fix, ${main.sats} satellites"
                else "fix " + fixAge(nowS, main.fixTime) + ", ${main.sats} satellites",
            color = Lg.muted, fontSize = 13.sp,
        )
    } else {
        Hint("Where MAIN is is not known: it has no GPS fix, or this AP has not heard it yet.")
    }
    Spacer(Modifier.height(8.dp))

    val haveTiles = remember { mutableStateOf(false) }
    val refused = remember { mutableStateOf(false) }
    MapCanvas(pins, main, tilesOn, haveTiles, refused)

    Text(
        when {
            tilesOn && haveTiles.value ->
                "Map tiles from OpenStreetMap, fetched by this phone for the squares shown. The grid " +
                    "has no Internet and never sees them, and no coordinate is in a tile's address."
            !tilesOn -> "Map tiles are off, so the plan is drawn here from the coordinates, north up."
            refused.value ->
                "The tile server sent the same picture for every square, which is how it refuses a " +
                    "client it does not want. The plan below is drawn here from the coordinates instead."
            else ->
                "No map tiles yet: this phone has no Internet, or the tile server did not answer. " +
                    "The plan is drawn here from the coordinates, with distances and bearings."
        },
        color = Lg.muted, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp),
    )

    Row(Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Switch(checked = tilesOn, onCheckedChange = { LinkHub.setTiles(ctx, it) })
        Text(
            "Map tiles from the Internet", color = Lg.text, fontSize = 14.sp,
            modifier = Modifier.weight(1f).padding(start = 10.dp),
        )
    }

    if (main != null) {
        val linkText = TileUrl.copyableOsmLink(main.lat, main.lon)
        Row(Modifier.fillMaxWidth().padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(
                linkText, color = Lg.muted, fontSize = 12.sp, fontFamily = FontFamily.Monospace,
                modifier = Modifier.weight(1f),
            )
            TextButton(onClick = { clipboard.setText(AnnotatedString(linkText)) }) { Text("Copy") }
        }
        Hint(
            "Copied as text, for a device with Internet later. This phone never opens it, and it " +
                "never sends a coordinate anywhere.",
        )
    }

    Spacer(Modifier.height(10.dp))
    TableRow(listOf("Name" to 1.4f, "Coordinates" to 1.6f, "From MAIN" to 1f, "Fix" to 0.9f), header = true)
    for (p in pins.sortedWith(compareByDescending<Pin> { it.isAp }.thenBy { it.id })) {
        Divider()
        val from = when {
            main == null -> "--"
            p === main -> "MAIN"
            else -> {
                val db = MapMath.distBearing(main.lat, main.lon, p.lat, p.lon)
                if (db.metres < 5) "here" else MapMath.fmtDist(db.metres) + " " + MapMath.compass(db.degrees)
            }
        }
        TableRow(
            listOf(
                (p.name + if (p.isAp) " (AP)" else "") to 1.4f,
                MapMath.fmtCoord(p.lat, p.lon) to 1.6f,
                from to 1f,
                (if (p.live) "live" else fixAge(nowS, p.fixTime)) to 0.9f,
            ),
        )
    }
    Spacer(Modifier.height(8.dp))
    Hint(
        "Positions stay in memory on the APs and handhelds, never in flash, and are shown only " +
            "here, behind the admin password. Grid data never leaves this phone.",
    )
}

private fun fixAge(nowS: Long, fixTime: Long): String {
    if (fixTime <= 0) return "--"
    val s = nowS - fixTime
    return when {
        s < 60 -> "just now"
        s < 3600 -> "${s / 60} min ago"
        s < 86400 -> "${s / 3600} h ago"
        else -> "${s / 86400} d ago"
    }
}

/**
 * The map itself. Tiles and the plan share one measured box, so the choice between them is made
 * where the size is known, and the plan is what shows until a tile actually arrives.
 */
@Composable
private fun MapCanvas(
    pins: List<Pin>,
    main: Pin?,
    tilesOn: Boolean,
    haveTiles: MutableState<Boolean>,
    refused: MutableState<Boolean>,
) {
    val bitmaps = remember { mutableStateMapOf<String, Bitmap>() }
    BoxWithConstraints(
        Modifier.fillMaxWidth().height(300.dp).clip(RoundedCornerShape(10.dp))
            .background(Lg.bg).border(1.dp, Lg.line, RoundedCornerShape(10.dp)),
    ) {
        val density = LocalDensity.current
        val w = with(density) { maxWidth.toPx().toDouble() }
        val h = with(density) { maxHeight.toPx().toDouble() }

        // Zoom in as far as every pin still fits with a margin, as the laptop dashboard does.
        var z = TileUrl.MAX_ZOOM - 2
        var lo = MapMath.Point(0.0, 0.0)
        var hi = MapMath.Point(0.0, 0.0)
        while (z >= 10) {
            val pts = pins.map { MapMath.worldPx(it.lat, it.lon, z) }
            lo = MapMath.Point(pts.minOf { it.x }, pts.minOf { it.y })
            hi = MapMath.Point(pts.maxOf { it.x }, pts.maxOf { it.y })
            if (hi.x - lo.x <= w - 92 && hi.y - lo.y <= h - 92) break
            z--
        }
        if (z < 10) z = 10
        val left = (lo.x + hi.x) / 2 - w / 2
        val top = (lo.y + hi.y) / 2 - h / 2
        val n = 1 shl z
        val rows = floor(top / MapMath.TILE).toInt()..floor((top + h) / MapMath.TILE).toInt()
        val cols = floor(left / MapMath.TILE).toInt()..floor((left + w) / MapMath.TILE).toInt()
        val squares = ArrayList<Triple<Int, Int, Int>>()
        for (ty in rows) {
            if (ty < 0 || ty >= n) continue
            for (tx in cols) squares.add(Triple(z, ((tx % n) + n) % n, ty))
        }

        // Only the squares on screen, one at a time: nothing is prefetched in bulk. The round
        // has a deadline, so a phone with no Internet settles on the plan within a few seconds
        // instead of waiting out every square in turn.
        LaunchedEffect(tilesOn, squares.joinToString(";") { "${it.first}/${it.second}/${it.third}" }) {
            if (!tilesOn) return@LaunchedEffect
            withTimeoutOrNull(TILE_ROUND_MS) {
                for ((tz, tx, ty) in squares) {
                    if (bitmaps.containsKey("$tz/$tx/$ty")) continue
                    Tiles.fetch(tz, tx, ty)?.let { bitmaps["$tz/$tx/$ty"] = it }
                    LinkHub.tilesOnline.value = Tiles.online
                }
            }
            LinkHub.tilesOnline.value = Tiles.online
        }
        // A tile server that refuses a client answers with one picture for every square, with an
        // ordinary 200. That is not a map, so it counts as no tiles. (An all-sea view would look
        // the same; the plan is a fine thing to show there too.)
        val seen = squares.mapNotNull { Tiles.digest(it.first, it.second, it.third) }
        val sameEverywhere = seen.size >= 4 && seen.distinct().size == 1
        val drew = tilesOn && !sameEverywhere &&
            squares.any { bitmaps.containsKey("${it.first}/${it.second}/${it.third}") }
        LaunchedEffect(drew) { haveTiles.value = drew }
        LaunchedEffect(sameEverywhere) { refused.value = sameEverywhere }

        if (!drew) {
            Plan(pins, main, w.toFloat(), h.toFloat())
            return@BoxWithConstraints
        }
        for (ty in rows) {
            for (tx in cols) {
                val bmp = bitmaps["$z/${((tx % n) + n) % n}/$ty"] ?: continue
                Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier
                        .offset(
                            x = with(density) { (tx * MapMath.TILE - left).toFloat().toDp() },
                            y = with(density) { (ty * MapMath.TILE - top).toFloat().toDp() },
                        )
                        .size(with(density) { MapMath.TILE.toFloat().toDp() }),
                )
            }
        }
        for (p in pins) {
            val q = MapMath.worldPx(p.lat, p.lon, z)
            val dx = with(density) { (q.x - left).toFloat().toDp() }
            val dy = with(density) { (q.y - top).toFloat().toDp() }
            Box(
                Modifier.offset(x = dx - 7.dp, y = dy - 7.dp).size(14.dp)
                    .clip(RoundedCornerShape(50))
                    .background(if (p.isAp) Lg.danger else Lg.accent)
                    .border(2.dp, Lg.bg, RoundedCornerShape(50)),
            )
            Text(
                p.name, color = Lg.text, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                modifier = Modifier.offset(x = dx + 10.dp, y = dy - 8.dp)
                    .background(Lg.surface).padding(horizontal = 3.dp),
            )
        }
        Text(
            TileUrl.ATTRIBUTION, color = Lg.muted, fontSize = 10.sp,
            modifier = Modifier.align(Alignment.BottomEnd).background(Lg.bg).padding(horizontal = 4.dp),
        )
    }
}

/** The fallback: relative positions, distances and bearings from MAIN, north up. */
@Composable
private fun Plan(pins: List<Pin>, main: Pin?, w: Float, h: Float) {
    val density = LocalDensity.current
    val ref = main ?: pins.first()
    val places = pins.map { p ->
        val db = MapMath.distBearing(ref.lat, ref.lon, p.lat, p.lon)
        val a = db.degrees * Math.PI / 180.0
        Triple(p, Offset((db.metres * sin(a)).toFloat(), (-db.metres * cos(a)).toFloat()), db)
    }
    val span = max(20.0, places.maxOf { hypot(it.second.x.toDouble(), it.second.y.toDouble()) })
    val cx = w / 2
    val cy = h / 2
    val scale = ((min(w, h) / 2 - with(density) { 50.dp.toPx() }) / span).toFloat()
    Canvas(Modifier.fillMaxSize()) {
        for (r in listOf(0.25f, 0.5f, 1f)) {
            drawCircle(Lg.line, radius = span.toFloat() * scale * r, center = Offset(cx, cy), style = Stroke(1f))
        }
        drawLine(Lg.line, Offset(cx, 18f), Offset(cx, h - 18f))
        drawLine(Lg.line, Offset(18f, cy), Offset(w - 18f, cy))
        for ((p, off, _) in places) {
            drawCircle(
                if (p.isAp) Lg.danger else Lg.accent, radius = if (p.isAp) 9f else 7f,
                center = Offset(cx + off.x * scale, cy + off.y * scale),
            )
        }
    }
    Text("N", color = Lg.muted, fontSize = 12.sp,
        modifier = Modifier.offset(x = with(density) { (cx + 5).toDp() }, y = 2.dp))
    for (r in listOf(0.25, 0.5, 1.0)) {
        Text(
            MapMath.fmtDist(span * r), color = Lg.muted, fontSize = 11.sp,
            modifier = Modifier.offset(
                x = with(density) { (cx + 4).toDp() },
                y = with(density) { (cy - span.toFloat() * scale * r.toFloat()).toDp() },
            ),
        )
    }
    // Labels sit to the right of their dot, pulled back in when that would run off the edge.
    for ((p, off, db) in places) {
        val text = p.name +
            if (db.metres < 5) "" else " · ${MapMath.fmtDist(db.metres)} ${MapMath.compass(db.degrees)}"
        val wanted = cx + off.x * scale + with(density) { 8.dp.toPx() }
        val room = with(density) { (text.length * 7).dp.toPx() }
        val x = wanted.coerceIn(0f, max(0f, w - room))
        Text(
            text, color = Lg.text, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1,
            modifier = Modifier.offset(
                x = with(density) { x.toDp() },
                y = with(density) { (cy + off.y * scale).toDp() } - 8.dp,
            ),
        )
    }
    Box(Modifier.fillMaxSize()) {
        Text(
            "Drawn on this phone: everyone's place relative to ${ref.name}, north up.",
            color = Lg.muted, fontSize = 11.sp,
            modifier = Modifier.align(Alignment.BottomStart).padding(8.dp),
        )
    }
}
