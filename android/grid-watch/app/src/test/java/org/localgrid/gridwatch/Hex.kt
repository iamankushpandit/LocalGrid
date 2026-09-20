package org.localgrid.gridwatch

fun hex(s: String): ByteArray {
    val t = s.replace(" ", "").replace(":", "")
    return ByteArray(t.length / 2) { t.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}

fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }
