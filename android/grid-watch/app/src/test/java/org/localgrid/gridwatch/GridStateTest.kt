package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.grid.AP_SILENT_S
import org.localgrid.gridwatch.grid.FRAME_FRESH_S
import org.localgrid.gridwatch.grid.GridEvent
import org.localgrid.gridwatch.grid.GridState
import org.localgrid.gridwatch.proto.StatusFrame
import org.localgrid.gridwatch.proto.StatusFrame.T_ALERT
import org.localgrid.gridwatch.proto.StatusFrame.T_BATTERY
import org.localgrid.gridwatch.proto.StatusFrame.T_HANDHELDS
import org.localgrid.gridwatch.proto.StatusFrame.T_HEALTH
import org.localgrid.gridwatch.proto.StatusFrame.T_NAME

/** The scenario of grid_watch.py --self-check, run through the Kotlin port. */
class GridStateTest {
    private val k = hex("81195f8f94e6ca012af5c676f0371081be25566471ac0fed405b773a275d6847")
    private val disc = hex("11223344")
    private val t0 = 1_000_000.0
    private val boot = 365L
    private val health = hex("2c0105031657090c041f")

    private fun discovery(ap: Int, d: ByteArray = disc, attached: Int = 2, name: String? = null): ByteArray {
        val base = "LG".toByteArray() + byteArrayOf(1) + d +
            byteArrayOf(ap.toByte(), 0, (15 - attached).toByte(), 0x03, attached.toByte())
        if (name == null) return base
        val n = name.toByteArray()
        return base + byteArrayOf(n.size.toByte()) + n
    }

    private fun seal(ap: Int, type: Int, counter: Int, pt: ByteArray, b: Long = boot) =
        StatusFrame.seal(k, ap, type, b, counter, pt)

    private fun le32(v: Long) = byteArrayOf(v.toByte(), (v shr 8).toByte(), (v shr 16).toByte(), (v shr 24).toByte())

    private fun flipped(f: ByteArray, index: Int, bit: Int): ByteArray {
        val i = if (index < 0) f.size + index else index
        return f.copyOf().also { it[i] = (it[i].toInt() xor bit).toByte() }
    }

    @Test fun selfCheckScenario() {
        val events = mutableListOf<GridEvent>()
        val st = GridState(k, disc, onEvent = { events += it }, started = t0)

        st.onAdvert(listOf(discovery(1, name = "NORTH")), -60, t0)
        var s = st.snapshot(t0)
        assertEquals(1, s.aps.size)
        assertEquals("NORTH", s.aps[0].name)
        assertTrue(s.aps[0].heard)
        assertTrue(s.aps[0].disc!!.backbone)
        assertEquals(2, s.aps[0].disc!!.attached)

        // Health.
        val f = seal(1, T_HEALTH, 10, health)
        assertEquals(23, f.size)
        st.onAdvert(listOf(discovery(1), f), -58, t0 + 0.5)
        val h = st.snapshot(t0 + 0.5).aps[0].health!!
        assertEquals("5 h 0 min", h.uptime)
        assertEquals(listOf("AP 0", "AP 2"), h.links)          // no discovery names for 0 and 2 yet
        assertEquals(3, h.handheldsHere)
        assertEquals("authoritative", h.timeQuality)
        assertTrue(h.timeGps)
        assertTrue(h.time.contains("stratum 2"))
        assertEquals(87, h.heapKb)
        assertEquals("low supply voltage (brownout)", h.reset)
        assertEquals(12, h.restarts)
        assertEquals(4, h.brownouts)
        assertTrue(h.gps.fitted && h.gps.fix)
        assertEquals(7, h.gps.sats)

        // Tampering.
        assertEquals(StatusFrame.Rejected("tag"), StatusFrame.open(k, flipped(f, -1, 1)))
        assertEquals(StatusFrame.Rejected("tag"), StatusFrame.open(k, flipped(f, 12, 0x80)))
        assertEquals(StatusFrame.Rejected("tag"), StatusFrame.open(k, flipped(f, 3, 1)))
        assertEquals(StatusFrame.Rejected("tag"), StatusFrame.open(ByteArray(32) { 7 }, f))
        assertEquals(StatusFrame.Rejected("length"), StatusFrame.open(k, f + ByteArray(5)))
        assertEquals(StatusFrame.Rejected("magic"), StatusFrame.open(k, flipped(f, 0, 0x07)))
        val okBefore = st.stat("ok")
        st.onAdvert(listOf(discovery(1), flipped(f, -2, 4)), -58, t0 + 0.6)
        assertEquals(okBefore, st.stat("ok"))
        assertEquals(1, st.stat("bad_tag"))

        // Replay.
        st.onAdvert(listOf(discovery(1), f), -58, t0 + 0.7)
        assertEquals(okBefore, st.stat("ok"))
        assertEquals(1, st.stat("duplicate"))
        st.onAdvert(listOf(seal(1, T_HEALTH, 9, health)), -58, t0 + 0.8)
        assertEquals(okBefore, st.stat("ok"))
        assertEquals(1, st.stat("replay"))
        st.onAdvert(listOf(seal(1, T_HEALTH, 500, health, boot - 1)), -58, t0 + 0.9)
        assertEquals(2, st.stat("replay"))

        // Handhelds, batteries, names.
        val online = byteArrayOf(0x0D, 0, 0, 0)                  // devices 1, 3, 4
        val where = ByteArray(10) { 0xFF.toByte() }.also { it[0] = 0xF1.toByte(); it[1] = 0x12 }
        st.onAdvert(listOf(seal(1, T_HANDHELDS, 11, online + where)), -58, t0 + 1)
        st.onAdvert(listOf(seal(1, T_BATTERY, 12, byteArrayOf(1, 80, 3, 15, 4, 0xFF.toByte()))), -58, t0 + 1.1)
        st.onAdvert(listOf(seal(1, T_NAME, 13, byteArrayOf(3) + "Priya été".toByteArray())), -58, t0 + 1.2)
        st.onAdvert(listOf(discovery(2, name = "SOUTH")), -70, t0 + 1.25)   // AP 2 names itself
        val hs = st.snapshot(t0 + 1.3).handhelds.associateBy { it.device }
        assertEquals(setOf(1, 3, 4), hs.keys)
        assertEquals("NORTH", hs.getValue(1).apName)
        assertEquals("SOUTH", hs.getValue(3).apName)
        assertEquals("NORTH", hs.getValue(4).apName)
        assertTrue(hs.values.all { it.online })
        assertEquals(80, hs.getValue(1).battery)
        assertEquals(15, hs.getValue(3).battery)
        assertNull(hs.getValue(4).battery)                           // 255: no battery sense
        assertEquals("Priya été", hs.getValue(3).name)
        assertEquals("Handheld 1", hs.getValue(1).name)

        // Alert: active SOS, then all clear.
        val alert = byteArrayOf(1, 3, 120, 0, 2, 0) + le32(1_789_700_000)
        st.onAdvert(listOf(seal(1, T_ALERT, 14, alert)), -58, t0 + 2)
        s = st.snapshot(t0 + 2)
        val a = s.alert!!
        assertTrue(a.active)
        assertFalse(a.allClear)
        assertEquals("Priya été", a.name)
        assertEquals("SOUTH", a.near)
        assertEquals(120L, a.ageS)
        assertEquals(2, a.reads)
        assertEquals(1_789_700_000L, s.gridTime)
        val sos = events.single { it.kind == "alert" }
        assertEquals("Priya été", sos.name)
        assertEquals("SOUTH", sos.near)
        assertEquals(3, sos.device)

        // The same alert heard again a second later raises no second event.
        st.onAdvert(listOf(seal(1, T_ALERT, 15, alert.copyOf().also { it[2] = 121.toByte() })), -58, t0 + 3)
        assertEquals(1, events.count { it.kind == "alert" })

        val clear = byteArrayOf(2, 3, 5, 0, 0xFF.toByte(), 0) + le32(1_789_700_060)
        st.onAdvert(listOf(seal(1, T_ALERT, 16, clear)), -58, t0 + 62)
        val c = st.snapshot(t0 + 62).alert!!
        assertTrue(c.allClear)
        assertNull(c.reads)
        assertEquals(1, events.count { it.kind == "all_clear" })

        // Restart: a new boot starts a new counter space.
        st.onAdvert(listOf(seal(1, T_HEALTH, 0, health, boot + 1)), -58, t0 + 63)
        assertEquals(boot + 1, st.snapshot(t0 + 63).aps.first { it.index == 1 }.boot)
        assertTrue(events.any { it.kind == "ap_restart" })

        // Another grid: ignored, status and all.
        val nAps = st.snapshot(t0 + 64).aps.size
        val foreign = StatusFrame.seal(ByteArray(32) { 9 }, 5, T_HEALTH, 1, 1, health)
        st.onAdvert(listOf(discovery(5, d = hex("99999999")), foreign), -70, t0 + 64)
        assertEquals(nAps, st.snapshot(t0 + 64).aps.size)
        assertEquals(1, st.stat("other_grid"))

        // Silence.
        st.tick(t0 + 63 + AP_SILENT_S + 1)
        s = st.snapshot(t0 + 63 + AP_SILENT_S + 1)
        assertFalse(s.aps.first { it.index == 1 }.heard)
        assertTrue(s.events.any { it.kind == "ap_lost" })
    }

    @Test fun badKeyWarnsOnceAfterFiveFrames() {
        val events = mutableListOf<GridEvent>()
        val st = GridState(k, disc, onEvent = { events += it }, started = t0)
        val wrong = ByteArray(32) { 3 }
        for (i in 0 until 6) {
            st.onAdvert(listOf(discovery(0), StatusFrame.seal(wrong, 0, T_HEALTH, 1, i, health)), -60, t0 + i)
        }
        assertEquals(1, events.count { it.kind == "bad_key" })
        assertEquals(6, st.snapshot(t0 + 6).aps[0].badTag)
        assertNull(st.snapshot(t0 + 6).aps[0].health)
    }

    @Test fun onlyHandheldsActuallyHeardAndOnlyWhileFresh() {
        val st = GridState(k, disc, started = t0)
        val where = ByteArray(10) { 0xFF.toByte() }.also { it[0] = 0xF0.toByte() }
        st.onAdvert(listOf(discovery(0), seal(0, T_HANDHELDS, 1, byteArrayOf(1, 0, 0, 0) + where)), -60, t0)
        assertTrue(st.snapshot(t0 + 1).handhelds.single().online)
        // A handhelds frame arrives about every 4 s per AP and this receiver goes quiet for
        // seconds at a time, so presence is forgiving: still shown well after a 15 s gap.
        assertTrue(st.snapshot(t0 + FRAME_FRESH_S - 1).handhelds.single().online)
        assertTrue(st.snapshot(t0 + FRAME_FRESH_S + 1).handhelds.isEmpty())
    }

    /** The bench measured receiver blackouts of up to 14.6 s: none of them may lose an AP. */
    @Test fun aQuietSpellDoesNotLoseAnAp() {
        val events = mutableListOf<GridEvent>()
        val st = GridState(k, disc, onEvent = { events += it }, started = t0)
        st.onAdvert(listOf(discovery(1, name = "NORTH")), -60, t0)
        for (gap in listOf(5.0, 14.6, 30.0, AP_SILENT_S - 1)) {
            st.tick(t0 + gap)
            val ap = st.snapshot(t0 + gap).aps.single()
            assertTrue("a $gap s quiet spell must not lose an AP", ap.heard)
            assertEquals(Math.round(gap), ap.silentS)          // but it is always visible
        }
        assertFalse(events.any { it.kind == "ap_lost" })
        st.tick(t0 + AP_SILENT_S + 1)
        assertFalse(st.snapshot(t0 + AP_SILENT_S + 1).aps.single().heard)
        assertEquals(1, events.count { it.kind == "ap_lost" })
    }

    /** An SOS raises its event the moment the frame arrives, whatever the silence timers say. */
    @Test fun anAlertIsRaisedAtOnceAfterALongQuietSpell() {
        val events = mutableListOf<GridEvent>()
        val st = GridState(k, disc, onEvent = { events += it }, started = t0)
        st.onAdvert(listOf(discovery(1, name = "NORTH")), -60, t0)
        st.tick(t0 + AP_SILENT_S + 30)                          // the AP has long been "not heard"
        assertTrue(events.any { it.kind == "ap_lost" })
        val alert = byteArrayOf(1, 3, 120, 0, 2, 0) + le32(1_789_700_000)
        val at = t0 + AP_SILENT_S + 31
        st.onAdvert(listOf(seal(1, T_ALERT, 14, alert)), -58, at)
        assertEquals(1, events.count { it.kind == "alert" })
        assertTrue(st.snapshot(at).alert!!.active)
    }

    @Test fun eventTextsMatchThePythonTool() {
        val events = mutableListOf<GridEvent>()
        val st = GridState(k, disc, onEvent = { events += it }, started = t0)
        st.onAdvert(listOf(discovery(1, name = "NORTH")), -60, t0)
        assertEquals("NORTH (AP 1) heard", events.last().text)
        st.tick(t0 + AP_SILENT_S + 1)
        assertEquals("NORTH (AP 1) not heard for 45 s", events.last().text)
        st.onAdvert(listOf(discovery(1)), -60, t0 + AP_SILENT_S + 2)
        assertEquals("NORTH (AP 1) heard again", events.last().text)
        assertNotNull(st.snapshot(t0 + AP_SILENT_S + 2).aps[0].disc)
    }
}
