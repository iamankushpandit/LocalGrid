package org.localgrid.gridwatch.watch

import android.annotation.SuppressLint
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.app.ServiceCompat
import org.localgrid.gridwatch.grid.GridState
import org.localgrid.gridwatch.proto.AdParser
import org.localgrid.gridwatch.proto.Discovery

/**
 * Listens for this grid's APs over BLE and keeps the grid state, in the foreground so it goes on
 * with the screen off. This service scans and nothing else: it never connects, and it never
 * transmits into the grid (D25). The admin link (D70) is separate, runs only while the app is on
 * screen, and lives in LinkHub.
 *
 * A BLE address is never shown, logged or stored (D21). The scan's handle for each AP is kept in
 * memory so the admin link can connect to the one heard best, and is never read for its address.
 */
class WatchService : Service() {
    private val handler = Handler(Looper.getMainLooper())
    private var state: GridState? = null
    private var companyId = 0
    private var discriminator: ByteArray? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanMode: Int? = null                  // the mode of the running scan, null = none
    private val starts = ArrayDeque<Long>()            // recent scan starts (Android allows 5 in 30 s)
    private var retryAt = 0L
    private var lastWatchText: String? = null
    private var started = false

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) = take(result)

        override fun onBatchScanResults(results: MutableList<ScanResult>) = results.forEach { take(it) }

        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "[BLE] scan failed, error $errorCode")
            scanMode = null
            GridHub.radio.value = Radio.FAILED
            GridHub.scanError.value = scanErrorText(errorCode)
            retryAt = SystemClock.elapsedRealtime() + 10_000
        }
    }

    private fun take(result: ScanResult) {
        val payloads = AdParser.manufacturerPayloads(result.scanRecord?.bytes, companyId)
        if (payloads.isEmpty()) return
        state?.onAdvert(payloads, result.rssi)
        // Remember which handle belongs to which AP index, so the admin link (D70) can connect
        // to the one heard best. The handle is never read for its address, shown or logged (D21).
        val mine = discriminator ?: return
        for (p in payloads) {
            val d = Discovery.parse(p) ?: continue
            if (d.disc.contentEquals(mine)) GridHub.noteAp(d.ap, result.device, result.rssi)
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            state?.tick()
            GridHub.publish()
            ensureScan()
            updateWatchNotification()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            GridHub.setWantWatch(this, false)
            stopSelf()
            return START_NOT_STICKY
        }
        GridHub.init(this)
        Notifications.createChannels(this)
        val text = Notifications.watchText(this, 0, false)
        try {
            ServiceCompat.startForeground(this, Notifications.ID_WATCH, Notifications.watching(this, text),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (e: Exception) {
            // Missing BLUETOOTH_SCAN, or started from the background where Android forbids it.
            Log.w(TAG, "[BLE] cannot run in the foreground: ${e.javaClass.simpleName}")
            GridHub.radio.value = Radio.NO_PERMISSION
            stopSelf()
            return START_NOT_STICKY
        }
        lastWatchText = text
        val pairing = GridHub.pairing.value
        val st = GridHub.gridState()
        if (pairing == null || st == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (!started) {
            started = true
            companyId = pairing.companyId
            discriminator = pairing.discriminator
            state = st
            GridHub.eventSink = { e -> handler.post { Notifications.onGridEvent(this, e) } }
            GridHub.watching.value = true
            Log.i(TAG, "[BLE] watching grid ${pairing.gridId}, company ID 0x%04X".format(companyId))
            handler.post(tick)
        }
        return START_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        stopScan()
        GridHub.eventSink = null
        GridHub.watching.value = false
        GridHub.radio.value = Radio.IDLE
        GridHub.publish()
        started = false
        super.onDestroy()
    }

    // -- scanning

    private fun wantedMode() =
        if (GridHub.visible.value) ScanSettings.SCAN_MODE_LOW_LATENCY else ScanSettings.SCAN_MODE_BALANCED

    private fun ensureScan() {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) {
            GridHub.radio.value = Radio.NO_ADAPTER
            return
        }
        if (!adapter.isEnabled) {
            if (scanMode != null) Log.i(TAG, "[BLE] Bluetooth switched off; waiting for it")
            scanMode = null                            // the stack drops scans when it goes off
            scanner = null
            GridHub.radio.value = Radio.BLUETOOTH_OFF
            return
        }
        if (!GridHub.canScan(this)) {
            stopScan()
            GridHub.radio.value = Radio.NO_PERMISSION
            return
        }
        if (GridHub.scanPaused.value) {
            // A watcher is talking to an AP: one radio, so the scan stands aside until it is done.
            if (scanMode != null) {
                Log.i(TAG, "[BLE] pausing the scan while the admin link is open")
                stopScan()
            }
            return
        }
        val want = wantedMode()
        if (scanMode == want) return
        val now = SystemClock.elapsedRealtime()
        if (now < retryAt) return
        while (starts.isNotEmpty() && now - starts.first() > 30_000) starts.removeFirst()
        if (starts.size >= 4) return                   // stay under Android's 5 starts per 30 s
        stopScan()
        startScan(adapter.bluetoothLeScanner ?: return, want)
    }

    @SuppressLint("MissingPermission")                 // checked by canScan() just before
    private fun startScan(s: BluetoothLeScanner, mode: Int) {
        val filter = ScanFilter.Builder().setManufacturerData(companyId, ByteArray(0), ByteArray(0)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(mode)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .setMatchMode(ScanSettings.MATCH_MODE_AGGRESSIVE)
            .setNumOfMatches(ScanSettings.MATCH_NUM_MAX_ADVERTISEMENT)
            .setReportDelay(0)
            .build()
        try {
            starts.addLast(SystemClock.elapsedRealtime())
            s.startScan(listOf(filter), settings, callback)
            scanner = s
            scanMode = mode
            GridHub.radio.value = Radio.SCANNING
            GridHub.scanError.value = null
            Log.i(TAG, "[BLE] scanning, " + if (mode == ScanSettings.SCAN_MODE_LOW_LATENCY) "low latency (app open)" else "balanced (background)")
        } catch (e: SecurityException) {
            GridHub.radio.value = Radio.NO_PERMISSION
        } catch (e: IllegalStateException) {
            // Bluetooth went off between the check and the call.
            GridHub.radio.value = Radio.BLUETOOTH_OFF
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        val s = scanner ?: return
        try {
            s.stopScan(callback)
        } catch (_: Exception) {
            // Bluetooth off or permission withdrawn: the scan is gone either way.
        }
        scanner = null
        scanMode = null
    }

    private fun updateWatchNotification() {
        val snap = GridHub.snapshot.value ?: return
        val sos = snap.alert?.let { it.active && !it.allClear } == true
        val text = Notifications.watchText(this, snap.apsHeard, sos)
        if (text == lastWatchText) return
        lastWatchText = text
        getSystemService(NotificationManager::class.java)
            .notify(Notifications.ID_WATCH, Notifications.watching(this, text))
    }

    private fun scanErrorText(code: Int) = when (code) {
        ScanCallback.SCAN_FAILED_ALREADY_STARTED -> "a scan was already running"
        ScanCallback.SCAN_FAILED_APPLICATION_REGISTRATION_FAILED -> "Android refused to register the scan (too many starts?)"
        ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED -> "this phone does not support the scan settings"
        ScanCallback.SCAN_FAILED_INTERNAL_ERROR -> "internal Bluetooth error"
        ScanCallback.SCAN_FAILED_OUT_OF_HARDWARE_RESOURCES -> "the Bluetooth chip is out of resources"
        ScanCallback.SCAN_FAILED_SCANNING_TOO_FREQUENTLY -> "scans started too often; retrying"
        else -> "error $code"
    } + "; retrying in 10 s"

    companion object {
        const val ACTION_STOP = "org.localgrid.gridwatch.STOP"
        private const val TAG = "GridWatch"
    }
}
