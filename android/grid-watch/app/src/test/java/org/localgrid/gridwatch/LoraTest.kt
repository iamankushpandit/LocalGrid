package org.localgrid.gridwatch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.link.Traffic

/**
 * The second backbone (D71) as the phone reads it.
 *
 * The LoRa section is **appended** to the TRAFFIC record, not numbered into it: the layout byte
 * stays 1, so every one of these blobs is the same record with a different ending, and an AP that
 * sends no section at all must still decode perfectly. That compatibility case is the one that
 * matters most, because a grid may mix firmware.
 */
class LoraTest {
    private val fitted = Traffic.decode(Vectors.trafficLoraBlob)
    private val down = Traffic.decode(Vectors.trafficLoraDownBlob)
    private val none = Traffic.decode(Vectors.trafficLoraNoneBlob)

    // -- every field, from a module that is working

    @Test fun theWholeSectionIsDecoded() {
        assertEquals(Vectors.int("lora_record_len"), Vectors.trafficLoraBlob.size)
        val l = fitted.lora!!
        assertTrue(l.fitted)
        assertTrue(l.configured)
        assertTrue(l.broadcast)
        assertFalse(l.off)
        assertFalse(l.big)                     // LORA_LONG_PAYLOAD is 0 on these APs
        assertEquals(Vectors.int("lora_address"), l.address)
        assertEquals(Vectors.int("lora_network"), l.network)
        assertEquals(Vectors.int("lora_rssi"), l.rssi)
        assertEquals(Vectors.int("lora_snr"), l.snr)
        assertEquals(Vectors.long("lora_frames_out"), l.framesOut)
        assertEquals(Vectors.long("lora_frames_in"), l.framesIn)
        assertEquals(Vectors.long("lora_frames_first"), l.framesFirst)
        assertEquals(Vectors.long("lora_parts_out"), l.partsOut)
        assertEquals(Vectors.long("lora_parts_in"), l.partsIn)
        assertEquals(Vectors.long("lora_parts_dropped"), l.partsDropped)
        assertEquals(Vectors.long("lora_reasm_timeouts"), l.reasmTimeouts)
        assertEquals(Vectors.long("lora_seal_fail"), l.sealFail)
        assertEquals(Vectors.long("lora_refused_big"), l.refusedBig)
        assertEquals(Vectors.int("lora_queue_depth"), l.queueDepth)
        assertEquals(Vectors.int("lora_queue_high"), l.queueHigh)
        assertEquals(Vectors.long("lora_queue_dropped"), l.queueDropped)
        assertEquals(Vectors.long("lora_airtime_ms"), l.airtimeMs)
        assertEquals(Vectors.long("lora_retries"), l.retries)
        assertEquals(Vectors.long("lora_heard_s"), l.heardS)
        assertEquals(Vectors.int("lora_peer_bits"), l.peerBits)
        assertEquals(listOf(1, 2), l.peersUp)
        assertEquals(Vectors.int("lora_restarts"), l.restarts)
        assertFalse(l.silent)
    }

    @Test fun theRestOfTheRecordIsUntouchedByTheNewSection() {
        // The same bytes as the record without it, so nothing before the section may move.
        val older = Traffic.decode(Vectors.trafficBlob)
        assertEquals(older.ap, fitted.ap)
        assertEquals(older.inTotal, fitted.inTotal)
        assertEquals(older.outTotal, fitted.outTotal)
        assertEquals(older.links.size, fitted.links.size)
        assertEquals(older.buckets, fitted.buckets)
        assertEquals(older.perf, fitted.perf)
        assertEquals(older.faults, fitted.faults)
    }

    @Test fun theNumberThatSaysTheRadioEarnsItsKeepLeads() {
        val l = fitted.lora!!
        assertEquals(19L, l.framesFirst)
        assertTrue(Traffic.loraWorth(l).startsWith("19 of the 640 frames that arrived by LoRa"))
        val quiet = fitted.lora!!.copy(framesFirst = 0)
        assertTrue(Traffic.loraWorth(quiet).startsWith("Nothing has needed LoRa yet"))
    }

    // -- no module: a fact, never a fault

    @Test fun anApWithNoModuleSaysSoAndRaisesNothing() {
        val l = none.lora
        assertNotNull(l)
        assertFalse(l!!.fitted)
        assertFalse(l.configured)
        assertEquals(0L, l.framesIn)
        assertNull(l.heardS)                   // heard_age_ms is 0xFFFFFFFF, never
        assertFalse(l.silent)                  // silent means fitted and quiet, not absent
        assertEquals(emptyList<Traffic.Note>(), Traffic.loraNotes(none, "NORTH") { "AP $it" })
    }

    @Test fun fittedAndSilentIsNotTheSameAsNoModule() {
        val silent = fitted.lora!!.copy(heardS = null, peerBits = 0)
        assertTrue(silent.silent)
        assertFalse(none.lora!!.silent)
        val notes = Traffic.loraNotes(fitted.copy(lora = silent), "MAIN") { "AP $it" }
        assertTrue(notes.any { it.red && it.text.contains("has never heard another one") })
    }

    // -- a peer down, and everything that turns red with it

    @Test fun aPeerDownTurnsRedAndSaysWhy() {
        val l = down.lora!!
        assertEquals(Vectors.int("lora_down_peer_bits"), l.peerBits)
        assertEquals(listOf(1), l.peersUp)
        assertEquals(Vectors.int("lora_down_rssi"), l.rssi)
        assertEquals(Vectors.long("lora_down_seal_fail"), l.sealFail)
        assertEquals(Vectors.long("lora_down_queue_dropped"), l.queueDropped)
        assertEquals(Vectors.long("lora_down_heard_s"), l.heardS)
        assertEquals(Vectors.long("lora_down_parts_dropped"), l.partsDropped)
        assertEquals(Vectors.long("lora_down_refused_big"), l.refusedBig)
        assertTrue(l.trouble > 0)

        val peers = Status.decode(Vectors.statusJson).lora!!.peers
        val red = Traffic.loraNotes(down, "MAIN", peers) { "AP $it" }.filter { it.red }.map { it.text }
        assertTrue(red.any { it.contains("LoRa link to AP 2 is down") })
        assertTrue(red.any { it.contains("could not authenticate 3 LoRa frames") })
        assertTrue(red.any { it.contains("send queue overflowed 5 times") })
        assertTrue(red.any { it.contains("refused 1 LoRa payloads") })
        // A queue that reached its four slots lost nothing by itself: amber, not red.
        val amber = Traffic.loraNotes(down, "MAIN", peers) { "AP $it" }.filterNot { it.red }
        assertTrue(amber.any { it.text.contains("reached 4 of 4 waiting frames") })
    }

    @Test fun aWorkingRadioRaisesOnlyTheSmallThings() {
        val peers = Status.decode(Vectors.statusJson).lora!!.peers.filter { it.up }
        val notes = Traffic.loraNotes(fitted, "MAIN", peers) { "AP $it" }
        assertTrue(notes.none { it.red })
        assertTrue(notes.any { it.text.contains("dropped 2 LoRa parts") })
        assertTrue(notes.any { it.text.contains("6 LoRa frames never arrived complete") })
        assertTrue(notes.any { it.text.contains("reset a wedged LoRa module 1 times") })
        assertTrue(notes.any { it.text.contains("reached 4 of 4 waiting frames") })
        // 43.2 s of airtime in 5 hours up is 0.2%: nothing to say about it.
        assertTrue(notes.none { it.text.contains("transmitting") })
    }

    @Test fun theLoraNotesRideAlongWithTheRest() {
        val peers = Status.decode(Vectors.statusJson).lora!!.peers
        val all = Traffic.notes(down, "MAIN", peers) { "AP $it" }
        assertTrue(all.any { it.text.contains("voice frames") })          // the Wi-Fi side still speaks
        assertTrue(all.any { it.text.contains("LoRa link to AP 2 is down") })
    }

    // -- what an incomplete or older record must do

    @Test fun aSectionCutShortIsLeftOutRatherThanHalfRead() {
        val short = Vectors.trafficLoraBlob.copyOf(Vectors.trafficLoraBlob.size - 30)
        val t = Traffic.decode(short)
        assertNull(t.lora)                     // never a guess from the bytes that did arrive
        assertEquals(fitted.links.size, t.links.size)
        assertEquals(fitted.inTotal, t.inTotal)
        assertEquals(emptyList<Traffic.Note>(), Traffic.loraNotes(t, "MAIN") { "AP $it" })
    }

    @Test fun oneByteShortOfTheSectionIsStillNoSection() {
        val t = Traffic.decode(Vectors.trafficLoraBlob.copyOf(Vectors.trafficLoraBlob.size - 1))
        assertNull(t.lora)
    }

    /** The case that matters most: an AP flashed before D71 sends nothing after the links. */
    @Test fun anApOlderThanD71DecodesAndSaysNothingAboutLora() {
        val t = Traffic.decode(Vectors.trafficBlob)
        assertNull(t.lora)
        assertEquals(Vectors.int("traffic_links"), t.links.size)
        assertEquals(Vectors.long("traffic_in_total"), t.inTotal)
        assertEquals(emptyList<Traffic.Note>(), Traffic.loraNotes(t, "MAIN") { "AP $it" })
        // and the old notes are exactly the ones they always were
        assertTrue(Traffic.notes(t, "MAIN") { "AP $it" }.none { it.text.contains("LoRa") })
    }

    // -- /api/status: the version string and the per-peer signal the packed record cannot carry

    @Test fun statusCarriesTheModuleVersionAndALinePerPeer() {
        val l = Status.decode(Vectors.statusJson).lora!!
        assertTrue(l.fitted && l.configured && l.broadcast)
        assertFalse(l.off)
        assertEquals(Vectors.str("status_lora_version"), l.version)
        assertEquals(Vectors.int("lora_address"), l.address)
        assertEquals(Vectors.int("lora_network"), l.network)
        assertEquals(Vectors.int("status_lora_peers"), l.peers.size)
        val best = l.peers[0]
        assertTrue(best.up)
        assertEquals(Vectors.int("status_lora_peer0_rssi"), best.rssi)
        assertEquals(Vectors.int("status_lora_peer0_snr"), best.snr)
        assertEquals(Vectors.long("status_lora_peer0_age_s"), best.ageS)
        val gone = l.peers[1]
        assertFalse(gone.up)
        assertEquals(Vectors.int("status_lora_peer1_ap"), gone.ap)
        assertEquals(Vectors.long("status_lora_peer1_age_s"), gone.ageS)
    }

    @Test fun statusFromAnApOlderThanD71HasNoLoraAtAll() {
        val s = Status.decode(Vectors.statusJsonNoLora)
        assertNull(s.lora)
        assertEquals("MAIN", s.apName)         // and everything else still reads
        assertEquals(2, s.links.size)
        assertEquals(2, s.groups.size)
    }

    @Test fun theSignalStepsAreTheAdminPagesOwn() {
        assertFalse(Traffic.loraSignalIsWeak(-10))
        assertFalse(Traffic.loraSignalIsWeak(-99))
        assertTrue(Traffic.loraSignalIsWeak(-101))
        assertFalse(Traffic.loraSignalIsBad(-101))
        assertTrue(Traffic.loraSignalIsBad(-120))
    }
}
