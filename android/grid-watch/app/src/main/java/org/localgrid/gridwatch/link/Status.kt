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

    /**
     * One AP this one has heard over LoRa (D71). The traffic record carries only a bitmask of
     * which heartbeats are current; this is where the per-peer signal comes from, and only for
     * the AP the phone actually asked.
     */
    data class LoraPeer(val ap: Int, val up: Boolean, val rssi: Int, val snr: Int, val ageS: Long?)

    /**
     * The second backbone as `/api/status` reports it: the same facts as the traffic record plus
     * the module's version string and a line per peer. Absent on an AP built before D71.
     */
    data class Lora(
        val fitted: Boolean, val configured: Boolean, val off: Boolean, val broadcast: Boolean,
        val big: Boolean, val version: String?, val address: Int, val network: Int,
        val peers: List<LoraPeer>,
    )

    data class Record(
        val gridName: String, val timezone: String, val ap: Int, val apName: String,
        val boot: Long, val uptimeS: Long, val gridTime: Long, val timeQuality: Int,
        val heapFree: Long, val heapMin: Long, val handhelds: Int, val gps: Gps?,
        val links: List<Link>, val devices: List<Device>, val users: Map<Int, String>,
        val announceAll: Boolean, val announcers: List<Int>, val groups: List<Group>,
        val positions: List<Position>,
        /** Null when this AP sent no `lora` object at all: an AP older than D71. */
        val lora: Lora? = null,
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
        val loraNode = root["lora"]
        val lora = if (loraNode is JsonValue.Obj) {
            Lora(
                fitted = loraNode["fitted"].bool(), configured = loraNode["configured"].bool(),
                off = loraNode["off"].bool(), broadcast = loraNode["broadcast"].bool(),
                big = loraNode["big"].bool(),
                version = loraNode["version"].strOrNull()?.takeIf { it.isNotBlank() },
                address = loraNode["address"].int(), network = loraNode["network"].int(),
                peers = loraNode["peers"].arr().map {
                    val age = it["age_ms"].long(-1)
                    LoraPeer(
                        ap = it["node"].int(), up = it["up"].bool(), rssi = it["rssi"].int(),
                        snr = it["snr"].int(),
                        ageS = if (age < 0 || age >= 0xFFFFFFFFL) null else age / 1000,
                    )
                },
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
            lora = lora,
        )
    }
}
