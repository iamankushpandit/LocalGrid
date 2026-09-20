package org.localgrid.gridwatch.watch

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import kotlinx.coroutines.flow.MutableStateFlow
import org.localgrid.gridwatch.grid.GridEvent
import org.localgrid.gridwatch.grid.GridState
import org.localgrid.gridwatch.link.ApChoice
import org.localgrid.gridwatch.grid.Snapshot
import org.localgrid.gridwatch.proto.Pairing
import org.localgrid.gridwatch.store.PairingStore

/** Why scanning is not running, shown on the dashboard. */
enum class Radio { IDLE, SCANNING, BLUETOOTH_OFF, NO_PERMISSION, NO_ADAPTER, FAILED }

/**
 * The app's one shared place: the pairing, the grid state the service fills, and what the
 * screens show. The watch service writes; the screens read.
 */
object GridHub {
    val pairing = MutableStateFlow<Pairing?>(null)
    val snapshot = MutableStateFlow<Snapshot?>(null)
    val watching = MutableStateFlow(false)
    val visible = MutableStateFlow(false)
    val radio = MutableStateFlow(Radio.IDLE)
    val scanError = MutableStateFlow<String?>(null)

    /** Where the service hears grid events (for notifications) while it runs. */
    @Volatile var eventSink: ((GridEvent) -> Unit)? = null

    private var state: GridState? = null
    private var store: PairingStore? = null

    /**
     * The scan's own handle for each AP, so the admin link (D70) can connect to the one heard
     * best. Held in memory only, never written down, never shown and never logged: a BLE handle
     * carries a hardware address, and D21 says those never leave the radio.
     */
    private class Seen(val device: android.bluetooth.BluetoothDevice, val rssi: Int, val atMs: Long)

    private val seen = HashMap<Int, Seen>()

    @Synchronized
    fun noteAp(ap: Int, device: android.bluetooth.BluetoothDevice, rssi: Int) {
        seen[ap] = Seen(device, rssi, android.os.SystemClock.elapsedRealtime())
    }

    /**
     * Every AP heard lately, loudest first, as (AP index, handle). They all carry the same
     * answers, so a failure with one is no reason to give up (ApChoice).
     */
    @Synchronized
    fun apsForLink(): List<Pair<Int, android.bluetooth.BluetoothDevice>> {
        val now = android.os.SystemClock.elapsedRealtime()
        val heard = seen.map { ApChoice.Heard(it.key, it.value.rssi, now - it.value.atMs) }
        return ApChoice.order(heard).mapNotNull { ap -> seen[ap]?.let { ap to it.device } }
    }

    /** How loud an AP was when last heard, for the connection log. */
    @Synchronized
    fun rssiOf(ap: Int): Int? = seen[ap]?.rssi

    /**
     * While a watcher is talking to an AP, the scan stands aside. Bluetooth has one radio: on a
     * Galaxy S24+ a running scan drags the MTU negotiation down to the 23-byte default, and the
     * AP then closes the link rather than answer a request it cannot reply to. One stop and one
     * start per pull, and a pull happens at most every 30 s, so this stays well inside Android's
     * five scan starts per 30 s.
     */
    val scanPaused = MutableStateFlow(false)

    @Synchronized
    fun forgetSeen() = seen.clear()

    /** The AP's chosen name if the beacon has carried one, else "AP n". */
    fun apName(ap: Int): String =
        snapshot.value?.aps?.firstOrNull { it.index == ap }?.name ?: "AP $ap"

    fun init(context: Context) {
        if (store != null) return
        store = PairingStore(context).also { pairing.value = it.load() }
        LinkHub.init(context)
    }

    @Synchronized
    fun gridState(): GridState? {
        val p = pairing.value ?: return null
        return state ?: GridState(p.key, p.discriminator, onEvent = { e -> eventSink?.invoke(e) })
            .also { state = it }
    }

    fun publish() {
        state?.let { snapshot.value = it.snapshot() }
    }

    fun pair(context: Context, p: Pairing) {
        stopWatching(context)
        store?.save(p)
        LinkHub.logOut(context)
        forgetSeen()
        synchronized(this) { state = null }
        snapshot.value = null
        pairing.value = p
    }

    fun forget(context: Context) {
        stopWatching(context)
        setWantWatch(context, false)
        store?.forget()
        LinkHub.logOut(context)
        forgetSeen()
        synchronized(this) { state = null }
        snapshot.value = null
        pairing.value = null
    }

    // -- watching

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences("settings", Context.MODE_PRIVATE)

    fun wantWatch(context: Context) = prefs(context).getBoolean("watch", false)

    fun setWantWatch(context: Context, on: Boolean) {
        prefs(context).edit { putBoolean("watch", on) }
    }

    /** Permissions the watch needs at runtime: BLE scan, and notifications on Android 13+. */
    fun missingPermissions(context: Context): List<String> {
        val need = mutableListOf(Manifest.permission.BLUETOOTH_SCAN)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) need += Manifest.permission.POST_NOTIFICATIONS
        return need.filter { ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED }
    }

    fun canScan(context: Context) = ContextCompat.checkSelfPermission(
        context, Manifest.permission.BLUETOOTH_SCAN,
    ) == PackageManager.PERMISSION_GRANTED

    fun startWatching(context: Context) {
        if (pairing.value == null || !canScan(context)) return
        setWantWatch(context, true)
        ContextCompat.startForegroundService(context, Intent(context, WatchService::class.java))
    }

    fun stopWatching(context: Context) {
        context.stopService(Intent(context, WatchService::class.java))
    }
}
