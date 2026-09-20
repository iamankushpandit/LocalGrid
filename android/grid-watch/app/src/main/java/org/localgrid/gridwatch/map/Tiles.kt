package org.localgrid.gridwatch.map

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Log
import android.util.LruCache
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Semaphore

/**
 * Map tiles, fetched only when the phone has Internet and only for the squares on screen.
 *
 * This is the one thing in the app that uses the Internet, and it is why the app asks for the
 * INTERNET permission. What goes out is a tile's grid square (z/x/y) with a proper User-Agent
 * and no referrer; what never goes out is anything about this grid — no coordinate, no name, no
 * status, no query string. Grid data never leaves the phone.
 *
 * Gentle on OpenStreetMap's tile policy: at most two fetches at a time, nothing prefetched, a
 * small memory cache, nothing written to disk, and a pause after a failure so an offline phone
 * stops asking.
 */
object Tiles {
    private const val TAG = "GridWatch"
    private const val MAX_TILES = 96                 // roughly four screens' worth, in memory only
    private const val MAX_IN_FLIGHT = 2
    private const val RETRY_MS = 15_000L
    private const val TIMEOUT_MS = 6_000
    private const val MAX_BYTES = 1 shl 20

    private val cache = object : LruCache<String, Bitmap>(MAX_TILES) {}
    private val digests = object : LruCache<String, String>(MAX_TILES) {}
    private val inFlight = Semaphore(MAX_IN_FLIGHT)

    @Volatile private var failedAtMs = 0L

    /** null until a tile has been tried: true if the last one arrived, false if it did not. */
    @Volatile var online: Boolean? = null
        private set

    /**
     * What a tile's bytes hash to. The map uses it to notice that the server has answered with
     * the same picture for every square, which is how OpenStreetMap refuses a client: it sends
     * an "Access blocked" picture with an ordinary 200. Showing that as a map would be worse
     * than showing the plan.
     */
    fun digest(z: Int, x: Int, y: Int): String? = digests.get(key(z, x, y))

    private fun key(z: Int, x: Int, y: Int) = "$z/$x/$y"

    /** Fetches one tile, or returns null when this phone cannot reach the tile server. */
    suspend fun fetch(z: Int, x: Int, y: Int): Bitmap? {
        if (!TileUrl.valid(z, x, y)) return null
        cache.get(key(z, x, y))?.let { return it }
        if (System.currentTimeMillis() - failedAtMs < RETRY_MS) return null
        return withContext(Dispatchers.IO) {
            inFlight.acquire()
            try {
                cache.get(key(z, x, y))?.let { return@withContext it }
                val conn = (URL(TileUrl.of(z, x, y)).openConnection() as HttpURLConnection).apply {
                    requestMethod = "GET"
                    connectTimeout = TIMEOUT_MS
                    readTimeout = TIMEOUT_MS
                    instanceFollowRedirects = true
                    setRequestProperty("User-Agent", TileUrl.USER_AGENT)
                    setRequestProperty("Accept", "image/png,image/*")
                    setRequestProperty("Referer", "")
                }
                try {
                    if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                        failedAtMs = System.currentTimeMillis()
                        online = false
                        return@withContext null
                    }
                    val bytes = conn.inputStream.use { it.readBytes(MAX_BYTES) }
                    val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                    if (bmp == null) {
                        failedAtMs = System.currentTimeMillis()
                        online = false
                        return@withContext null
                    }
                    cache.put(key(z, x, y), bmp)
                    digests.put(key(z, x, y), sha256(bytes))
                    online = true
                    bmp
                } finally {
                    conn.disconnect()
                }
            } catch (e: Exception) {
                // Offline is the normal case out in the field, so this is not an error.
                Log.i(TAG, "[MAP] no map tiles (${e.javaClass.simpleName}); drawing the plan instead")
                failedAtMs = System.currentTimeMillis()
                online = false
                null
            } finally {
                inFlight.release()
            }
        }
    }

    private fun sha256(b: ByteArray): String =
        java.security.MessageDigest.getInstance("SHA-256").digest(b)
            .joinToString("") { "%02x".format(it) }

    private fun java.io.InputStream.readBytes(limit: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(8192)
        while (true) {
            val n = read(buf)
            if (n < 0 || out.size() > limit) break
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }
}
