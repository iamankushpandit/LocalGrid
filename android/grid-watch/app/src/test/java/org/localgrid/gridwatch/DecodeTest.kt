package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.localgrid.gridwatch.link.History
import org.localgrid.gridwatch.link.LinkException
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.link.Traffic
import org.localgrid.gridwatch.map.pinsOf

/**
 * The three replies, decoded from realistic blobs the laptop tool built: the admin page's own
 * /api/status JSON, its /api/history records, and the TRAFFIC counters traffic.c packs. Every
 * number here is what tools/grid_watch.py's decoder printed for the same bytes.
 */
class DecodeTest {
    // -- /api/status

    private val status: Status.Record = Status.decode(Vectors.statusJson)

    @Test fun statusHeader() {
        assertEquals("Lakeside Trip", status.gridName)
        assertEquals("Europe/Berlin", status.timezone)
        assertEquals(0, status.ap)
        assertEquals("MAIN", status.apName)
        assertEquals(41L, status.boot)
        assertEquals(18000L, status.uptimeS)
        assertEquals(1789700000L, status.gridTime)
        assertEquals(2, status.timeQuality)
        assertEquals(118000L, status.heapFree)
        assertEquals(3, status.handhelds)
    }

    @Test fun statusGpsAndLinks() {
        val g = status.gps!!
        assertTrue(g.fix && g.hasPos)
        assertEquals(9, g.sats)
        assertEquals(47623450, g.latU)
        assertEquals(13045670, g.lonU)
        assertEquals(2, status.links.size)
        assertTrue(status.links[0].up)
        assertEquals(-62, status.links[0].rssi)
        assertFalse(status.links[1].up)
        assertEquals(95000L, status.links[1].ageMs)
    }

    @Test fun statusPeopleGroupsAndAnnouncers() {
        assertEquals(3, status.devices.size)
        assertEquals("Ana", status.devices[0].name)
        assertEquals("ONLINE", status.devices[0].state)
        assertEquals(-1, status.devices[2].ap)
        assertEquals("Priya", status.nameOf(3))
        assertEquals("Handheld 9", status.nameOf(9))
        assertFalse(status.announceAll)
        assertEquals(listOf(1), status.announcers)
        assertEquals(2, status.groups.size)
        assertEquals("Cooks", status.groups[0].name)
        assertEquals(listOf(1, 3), status.groups[0].members)
    }

    @Test fun statusPositionsBecomePinsWithTheLiveFixOnTop() {
        assertEquals(2, status.positions.size)
        val pins = pinsOf(status)
        assertEquals(2, pins.size)
        val main = pins.first { it.isAp }
        assertEquals("MAIN", main.name)
        assertTrue(main.live)                     // MAIN's own GPS fix replaces its recorded one
        assertEquals(47.623450, main.lat, 1e-9)
        assertEquals(13.045670, main.lon, 1e-9)
        val priya = pins.first { !it.isAp }
        assertEquals("Priya", priya.name)
        assertEquals(47.625100, priya.lat, 1e-9)
        assertEquals(1789699800L, priya.fixTime)
    }

    @Test fun statusThatIsNotJsonIsRefusedAndNotGuessed() {
        try {
            Status.decode("{\"grid_name\": ")
            fail("half a JSON object should be refused")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("status reply"))
        }
    }

    // -- /api/history

    private val history: History.Record = History.decode(Vectors.historyBlob)

    @Test fun historyHeaderAndAps() {
        assertEquals(Vectors.int("hist_self"), history.self)
        assertEquals(Vectors.int("hist_aps"), history.aps.size)
        assertEquals(Vectors.bool("hist_time_gps"), history.timeSet.gps)
        assertEquals(Vectors.long("hist_newest_min"), history.newestMin)
        assertEquals("MAIN", history.aps[0].name)
        assertTrue(history.aps[0].self)
        assertEquals(Vectors.long("hist_ap0_prev_run"), history.aps[0].prevRunS)
        assertEquals(Vectors.str("hist_ap2_name"), history.aps[2].name)
        assertEquals(Vectors.long("hist_ap2_uptime"), history.aps[2].uptimeS)
        assertEquals(Vectors.str("hist_ap2_reset"), history.aps[2].reset)
        assertEquals(Vectors.int("hist_ap1_sync_drift"), history.aps[1].syncDriftMs)
        assertEquals(Vectors.long("hist_ap1_prev_run"), history.aps[1].prevRunS)
    }

    @Test fun historyAvailabilityIsTwoHoursOfMinutes() {
        for (a in history.aps) assertEquals(History.AVAIL_MINUTES, a.avail.size)
        assertEquals(Vectors.int("hist_ap2_avail_first"), history.aps[2].avail.first())
        assertEquals(Vectors.int("hist_ap2_avail_last"), history.aps[2].avail.last())
        assertEquals(Vectors.int("hist_ap1_avail4"), history.aps[1].avail[4])
        // SOUTH was unreachable for the last twenty minutes of the record.
        assertEquals(20, history.aps[2].avail.count { it == 3 })
    }

    @Test fun historyIncidents() {
        assertEquals(Vectors.int("hist_incidents"), history.incidents.size)
        val first = history.incidents[0]
        assertEquals(Vectors.str("hist_inc0_kind"), first.kind)
        assertEquals(Vectors.str("hist_inc0_reset"), first.reset)
        assertEquals(Vectors.int("hist_inc0_ap"), first.ap)
        assertEquals(Vectors.int("hist_inc0_duration"), first.durationS)
        assertEquals(Vectors.long("hist_inc0_prev_run"), first.prevRunS)
        assertEquals(Vectors.long("hist_inc1_prev_run"), history.incidents[1].prevRunS)
    }

    @Test fun historyInAnotherLayoutIsRefused() {
        val blob = Vectors.historyBlob
        blob[0] = 9
        try {
            History.decode(blob)
            fail("an unknown layout should be refused, not guessed")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("layout 1"))
        }
    }

    // -- TRAFFIC

    private val traffic: Traffic.Record = Traffic.decode(Vectors.trafficBlob)

    @Test fun trafficHeaderAndRates() {
        assertEquals(Vectors.int("traffic_ap"), traffic.ap)
        assertEquals(Vectors.int("traffic_cpu"), traffic.cpuBusy)
        assertEquals(Vectors.dbl("traffic_bucket_s"), traffic.bucketS, 1e-9)
        assertEquals(Vectors.long("traffic_uptime_s"), traffic.uptimeS)
        assertEquals(Vectors.long("traffic_in_total"), traffic.inTotal)
        assertEquals(Vectors.long("traffic_out_total"), traffic.outTotal)
        assertEquals(Vectors.dbl("traffic_rate_1m"), traffic.rate1m!!, 1e-9)
        assertEquals(Vectors.dbl("traffic_rate_5m"), traffic.rate5m!!, 1e-9)
        assertEquals(Vectors.int("traffic_buckets"), traffic.buckets.size)
        assertEquals(Vectors.int("traffic_bucket_last"), traffic.buckets.last().messages)
    }

    @Test fun trafficClassesAndFaults() {
        assertEquals(Traffic.CLASSES, traffic.messages.keys.toList())
        assertEquals(Vectors.long("traffic_voice_in"), traffic.messages["voice"]!!.inCount)
        assertEquals(640L, traffic.messages["direct"]!!.relayed)
        assertEquals(Traffic.FAULTS, traffic.faults.keys.toList())
        assertEquals(Vectors.long("traffic_voice_dropped"), traffic.fault("voice_dropped"))
        assertEquals(Vectors.long("traffic_duplicate"), traffic.fault("duplicate"))
    }

    @Test fun trafficHandheldsAndPerformance() {
        assertEquals(Vectors.int("traffic_sessions"), traffic.handhelds.sessions)
        assertEquals(Vectors.long("traffic_bytes_in"), traffic.handhelds.bytesIn)
        assertEquals(Vectors.long("traffic_slowest_send_ms"), traffic.handhelds.slowestSendMs)
        assertEquals(Vectors.long("traffic_heap_free"), traffic.perf.heapFree)
        assertEquals(Vectors.long("traffic_heap_min"), traffic.perf.heapMin)
        assertEquals(Vectors.int("traffic_queue_high"), traffic.perf.queueHigh)
        assertEquals(Vectors.long("traffic_loop_max_ms"), traffic.perf.loopMaxMs)
        // Offset 24 is milliseconds, offset 28 microseconds (traffic.c), so 6 µs, not 6 ms.
        assertEquals(Vectors.long("traffic_loop_avg_us"), traffic.perf.loopAvgUs)
        assertEquals("6 µs", Traffic.avgPass(traffic.perf.loopAvgUs))
        assertEquals("1.50 ms", Traffic.avgPass(1500))
        assertEquals("999 µs", Traffic.avgPass(999))
        assertEquals(Vectors.long("traffic_stack_link"), traffic.perf.stackLink)
        assertEquals(Vectors.long("traffic_wifi_errors"), traffic.perf.wifiErrors)
    }

    @Test fun trafficLinksCarryNoAddress() {
        assertEquals(Vectors.int("traffic_links"), traffic.links.size)
        val down = traffic.links[1]
        assertEquals(Vectors.int("traffic_link1_ap"), down.ap)
        assertEquals(Vectors.bool("traffic_link1_up"), down.up)
        assertEquals(Vectors.int("traffic_link1_rssi"), down.rssi)
        assertEquals(Vectors.long("traffic_link1_failures"), down.failures)
        assertEquals(Vectors.long("traffic_link1_heard_s"), down.heardS)
        assertTrue(traffic.links[0].up)
    }

    @Test fun trafficNotesSayWhatIsWrongInPlainEnglish() {
        val notes = Traffic.notes(traffic, "MAIN") { "AP $it" }
        val red = notes.filter { it.red }.map { it.text }
        assertTrue(red.any { it.contains("dropped 12 voice frames") })
        assertTrue(red.any { it.contains("18 KB of free memory") })
        assertTrue(red.any { it.contains("core queue reached 17") })
        assertTrue(red.any { it.contains("The link from MAIN to AP 2 is down") })
        assertTrue(notes.any { !it.red && it.text.contains("TTL") })
    }

    @Test fun aQuietApRaisesNothing() {
        val quiet = Traffic.decode(quietBlob())
        assertEquals(emptyList<Traffic.Note>(), Traffic.notes(quiet, "NORTH") { "AP $it" })
        assertNull(quiet.cpuBusy)
        assertEquals(0.0, quiet.rate1m!!, 1e-9)
    }

    @Test fun trafficInAnotherLayoutIsRefused() {
        val blob = Vectors.trafficBlob
        blob[0] = 2
        try {
            Traffic.decode(blob)
            fail("an unknown layout should be refused")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("layout 1"))
        }
    }

    @Test fun trafficWithAnotherNumberOfClassesIsRefused() {
        val blob = Vectors.trafficBlob
        blob[3] = 11
        try {
            Traffic.decode(blob)
            fail("a different set of message classes should be refused")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("message classes"))
        }
    }

    @Test fun trafficCutShortIsRefused() {
        try {
            Traffic.decode(Vectors.trafficBlob.copyOf(60))
            fail("a record that stops early should be refused")
        } catch (e: LinkException) {
            assertTrue(e.message!!.contains("stop before"))
        }
    }

    /** A healthy AP: no faults, plenty of heap, both links up, nothing counted. */
    private fun quietBlob(): ByteArray {
        val buckets = 10
        val out = ByteArray(
            Traffic.HEADER + Traffic.CLASSES.size * Traffic.CLASS + Traffic.FAULTS.size * 4 +
                Traffic.SESS + Traffic.PERF + buckets * Traffic.BUCKET,
        )
        out[0] = 1
        out[1] = 1
        out[2] = 0
        out[3] = Traffic.CLASSES.size.toByte()
        out[4] = buckets.toByte()
        out[5] = 255.toByte()                 // CPU busy not measured
        out[6] = 0x30; out[7] = 0x75          // 30000 ms buckets
        var o = Traffic.HEADER + Traffic.CLASSES.size * Traffic.CLASS + Traffic.FAULTS.size * 4 + Traffic.SESS
        // Free heap 200 KB now, 150 KB lowest, largest block 64 KB, and a quiet main loop.
        fun u32(at: Int, v: Long) {
            out[at] = v.toByte(); out[at + 1] = (v ushr 8).toByte()
            out[at + 2] = (v ushr 16).toByte(); out[at + 3] = (v ushr 24).toByte()
        }
        u32(o, 200 * 1024L)
        u32(o + 4, 150 * 1024L)
        u32(o + 8, 64 * 1024L)
        u32(o + 16, 4096)                     // core stack headroom
        u32(o + 20, 4096)                     // link task stack headroom
        return out
    }
}
