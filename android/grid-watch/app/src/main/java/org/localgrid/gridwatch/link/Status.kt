package org.localgrid.gridwatch.link

import org.localgrid.gridwatch.json.Json
import org.localgrid.gridwatch.json.JsonException
import org.localgrid.gridwatch.json.JsonValue
import org.localgrid.gridwatch.json.arr
import org.localgrid.gridwatch.json.bool
import org.localgrid.gridwatch.json.get
import org.localgrid.gridwatch.json.int
import org.localgrid.gridwatch.json.long
import org.localgrid.gridwatch.json.str
import org.localgrid.gridwatch.json.strOrNull

/**
 * `GET /api/status`: the admin page's own reply, carried over the link so the page and the
 * watcher cannot drift apart (docs/ble-link.md). It holds facts about the grid — APs, links,
 * handhelds, groups, who may announce and where everyone is — and no message text of any kind.
 */
object Status {
    data class Gps(val started: Boolean, val heard: Boolean, val fix: Boolean, val sats: Int,
                   val hasPos: Boolean, val latU: Int, val lonU: Int)

    data class Link(val ap: Int, val up: Boolean, val rssi: Int, val ageMs: Long)

    data class Device(val device: Int, val name: String?, val state: String, val ap: Int)

    data class Group(val id: Int, val name: String, val members: List<Int>)

    data class Position(
        val subject: Int, val name: String?, val isAp: Boolean, val latU: Int, val lonU: Int,
        val fixTime: Long, val sats: Int,
    ) {
        val lat: Double get() = latU / 1e6
        val lon: Double get() = lonU / 1e6
    }

    data class Record(
        val gridName: String, val timezone: String, val ap: Int, val apName: String,
        val boot: Long, val uptimeS: Long, val gridTime: Long, val timeQuality: Int,
        val heapFree: Long, val heapMin: Long, val handhelds: Int, val gps: Gps?,
        val links: List<Link>, val devices: List<Device>, val users: Map<Int, String>,
        val announceAll: Boolean, val announcers: List<Int>, val groups: List<Group>,
        val positions: List<Position>,
    ) {
        fun nameOf(device: Int): String = users[device] ?: "Handheld $device"
    }

    fun decode(text: String): Record {
        val root = try {
            Json.parse(text)
        } catch (e: JsonException) {
            throw LinkException("the AP's status reply is not JSON this app can read (${e.message})")
        }
        if (root !is JsonValue.Obj) throw LinkException("the AP's status reply is not an object")

        val gpsNode = root["gps"]
        val gps = if (gpsNode is JsonValue.Obj) {
            Gps(
                started = gpsNode["started"].bool(), heard = gpsNode["heard"].bool(),
                fix = gpsNode["fix"].bool(), sats = gpsNode["sats"].int(),
                hasPos = gpsNode["pos"].bool(), latU = gpsNode["lat_u"].int(),
                lonU = gpsNode["lon_u"].int(),
            )
        } else {
            null
        }
        val users = LinkedHashMap<Int, String>()
        for (u in root["users"].arr()) {
            val n = u["name"].strOrNull() ?: continue
            users[u["device"].int()] = n
        }
        return Record(
            gridName = root["grid_name"].str("LocalGrid"),
            timezone = root["timezone"].str(),
            ap = root["node"].int(),
            apName = root["node_name"].str("AP ${root["node"].int()}"),
            boot = root["boot"].long(),
            uptimeS = root["uptime_s"].long(),
            gridTime = root["grid_time"].long(),
            timeQuality = root["time_quality"].int(),
            heapFree = root["heap_free"].long(),
            heapMin = root["heap_min"].long(),
            handhelds = root["handhelds"].int(),
            gps = gps,
            links = root["links"].arr().map {
                Link(it["node"].int(), it["up"].bool(), it["rssi"].int(), it["age_ms"].long())
            },
            devices = root["devices"].arr().map {
                Device(it["device"].int(), it["name"].strOrNull(), it["state"].str("?"), it["node"].int(-1))
            },
            users = users,
            announceAll = root["announce_all"].bool(true),
            announcers = root["announcers"].arr().map { it.int() },
            groups = root["groups"].arr().map {
                Group(it["id"].int(), it["name"].str("group"), it["members"].arr().map { m -> m.int() })
            },
            positions = root["positions"].arr().map {
                Position(
                    subject = it["subject"].int(), name = it["name"].strOrNull(),
                    isAp = it["ap"].bool(), latU = it["lat_u"].int(), lonU = it["lon_u"].int(),
                    fixTime = it["fix_time"].long(), sats = it["sats"].int(),
                )
            },
        )
    }
}
