package org.localgrid.gridwatch

import java.util.Properties

/**
 * Test vectors generated from tools/grid_watch.py — the laptop watcher, which is the reference
 * implementation for the admin link (D70) — with a TEST key: HKDF over the bytes 0..31, never
 * this grid's firmware/common/lg_secrets.h. The same key the existing frame tests already use.
 */
object Vectors {
    private fun resource(name: String): ByteArray =
        Vectors::class.java.classLoader!!.getResourceAsStream("vectors/$name")
            ?.use { it.readBytes() }
            ?: error("missing test resource vectors/$name")

    val props: Properties = Properties().apply {
        resource("vectors.properties").inputStream().use { load(it) }
    }

    fun str(key: String): String = props.getProperty(key) ?: error("no vector \"$key\"")
    fun bytes(key: String): ByteArray = hex(str(key))
    fun int(key: String): Int = str(key).toInt()
    fun long(key: String): Long = str(key).toLong()
    fun dbl(key: String): Double = str(key).toDouble()
    fun bool(key: String): Boolean = str(key).toBoolean()
    fun list(key: String): List<ByteArray> = str(key).split(",").map { hex(it) }

    val statusJson: String get() = String(resource("status.json"), Charsets.UTF_8)
    val historyBlob: ByteArray get() = hex(String(resource("history.hex"), Charsets.UTF_8).trim())
    val trafficBlob: ByteArray get() = hex(String(resource("traffic.hex"), Charsets.UTF_8).trim())

    /** The whole recorded conversation: what the AP notified, and what the reference client wrote. */
    private val transcript: List<Pair<String, String>> by lazy {
        String(resource("transcript.txt"), Charsets.UTF_8).lines()
            .filter { it.isNotBlank() && !it.startsWith("#") }
            .map { it.substringBefore(' ') to it.substringAfter(' ').trim() }
    }

    val notifications: List<ByteArray> get() = transcript.filter { it.first == "N" }.map { hex(it.second) }
    val clientWrites: List<ByteArray> get() = transcript.filter { it.first == "W" }.map { hex(it.second) }
}
