package org.localgrid.gridwatch.watch

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import kotlinx.coroutines.flow.MutableStateFlow
import org.localgrid.gridwatch.link.AdminAuth
import org.localgrid.gridwatch.link.ApChoice
import org.localgrid.gridwatch.link.BleAdminLink
import org.localgrid.gridwatch.link.History
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.link.LinkException
import org.localgrid.gridwatch.link.LinkLog
import org.localgrid.gridwatch.link.LinkProtocol
import org.localgrid.gridwatch.link.Status
import org.localgrid.gridwatch.link.Traffic
import org.localgrid.gridwatch.store.PasswordStore
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Everything the pulled tabs draw, in one value so Compose can watch it. */
data class LinkView(
    val state: LinkData.State = LinkData.State.IDLE,
    val message: String = "Not connected to an AP yet.",
    val authState: AdminAuth.State = AdminAuth.State.NONE,
    val authMessage: String = AdminAuth.DEFAULT_MESSAGE,
    val loggedIn: Boolean = false,
    val remembered: Boolean = false,
    val busy: Boolean = false,
    val status: LinkData.Section<Status.Record>? = null,
    val history: LinkData.Section<History.Record>? = null,
    val traffic: List<LinkData.Section<Traffic.Record>> = emptyList(),
)

/**
 * The admin link (D70) as the app uses it: the password, the last pull, and when to ask again.
 *
 * Battery first. The link is only opened while the app is on screen or when the user presses
 * Refresh; the background service goes on watching the beacon and sounding SOS notifications as
 * it always did, and never connects. One connection at a time, and never while another is open.
 */
object LinkHub {
    val auth = AdminAuth()
    val data = LinkData()
    val view = MutableStateFlow(LinkView())

    /** The Traffic tab is open: ask for the counters too, and a little more often. */
    val wantTraffic = MutableStateFlow(false)

    /** Map tiles from the Internet, which the user can turn off entirely. */
    val tilesOn = MutableStateFlow(true)

    /** Whether the last tile fetch worked, so the Map tab can say why it drew a plan instead. */
    val tilesOnline = MutableStateFlow<Boolean?>(null)

    private val busy = AtomicBoolean(false)
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lg-admin-link").apply { isDaemon = true }
    }
    private var store: PasswordStore? = null
    private var lastPullMs = 0L

    private const val TAG = "GridWatch"
    const val PERIOD_MS = 60_000L            // a pull cycle at rest, while the app is on screen
    const val TRAFFIC_PERIOD_MS = 30_000L    // while the Traffic tab is open
    private const val SCAN_STOP_MS = 1_500L  // long enough for the service's next pass

    /** Said plainly, because a missing permission used to look like a sulking AP. */
    const val PERMISSION_MESSAGE =
        "This phone may listen for the APs but not connect to one, so the map, the groups, " +
            "the history and the counters cannot be fetched. Android calls it the \"Nearby devices\"" +
            " permission."

    fun init(context: Context) {
        if (store != null) return
        val s = PasswordStore(context).also { store = it }
        tilesOn.value = prefs(context).getBoolean("tiles", true)
        val remembered = s.load()
        if (remembered != null) {
            auth.setPassword(remembered, remembered = true)
            remembered.fill('\u0000')
            auth.result(false, "Remembered on this phone. Reading from an AP…")
        }
        publish()
    }

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences("settings", Context.MODE_PRIVATE)

    fun setTiles(context: Context, on: Boolean) {
        prefs(context).edit { putBoolean("tiles", on) }
        tilesOn.value = on
        if (!on) tilesOnline.value = null
    }

    fun publish() {
        view.value = LinkView(
            state = data.state, message = data.message, authState = auth.state,
            authMessage = auth.message, loggedIn = auth.loggedIn, remembered = auth.remembered,
            busy = busy.get(), status = data.status, history = data.history, traffic = data.traffic,
        )
    }

    // -- logging in and out

    fun logIn(context: Context, password: CharArray, remember: Boolean) {
        auth.setPassword(password, remembered = false)
        if (remember) store?.save(password) else store?.forget()
        password.fill('\u0000')
        publish()
        refresh(context, now = true)
    }

    fun logOut(context: Context) {
        auth.forget()
        LinkLog.clear()
        store?.forget()
        data.clear()
        wantTraffic.value = false
        publish()
    }

    // -- pulling

    fun canConnect(context: Context) = ContextCompat.checkSelfPermission(
        context, Manifest.permission.BLUETOOTH_CONNECT,
    ) == PackageManager.PERMISSION_GRANTED

    /**
     * Asks a nearby AP now. [now] is the user pressing Refresh; otherwise this is the app's own
     * gentle cycle and it waits its turn.
     */
    fun refresh(context: Context, now: Boolean = false) {
        if (!auth.havePassword()) {
            data.note(LinkData.State.IDLE, AdminAuth.DEFAULT_MESSAGE)
            publish()
            return
        }
        val sinceLast = System.currentTimeMillis() - lastPullMs
        val period = if (wantTraffic.value) TRAFFIC_PERIOD_MS else PERIOD_MS
        if (!now && sinceLast < period) return
        if (!canConnect(context)) {
            // Not a guess: this is the permission state Android reports right now.
            data.note(LinkData.State.NEEDS_PERMISSION, PERMISSION_MESSAGE)
            LinkLog.add("cannot connect: the Nearby devices permission does not allow connecting")
            publish()
            return
        }
        if (!GridHub.watching.value) {
            data.note(LinkData.State.ERROR, "Start watching first: the app asks the APs it can hear.")
            publish()
            return
        }
        val pairing = GridHub.pairing.value ?: return
        val candidates = GridHub.apsForLink()
        if (candidates.isEmpty()) {
            data.note(LinkData.State.ERROR, ApChoice.summarise(emptyList()))
            publish()
            return
        }
        if (!busy.compareAndSet(false, true)) return       // one connection at a time
        lastPullMs = System.currentTimeMillis()
        val wanted = wantTraffic.value
        val app = context.applicationContext
        val linkKey = LinkProtocol.linkKey(pairing.key)
        worker.execute {
            // The scan stands aside for the whole round: one radio, and a running scan is what
            // dragged the MTU down to the 23-byte default on the owner's phone.
            GridHub.scanPaused.value = true
            waitForTheScanToStop()
            val failures = ArrayList<Pair<String, String>>()
            try {
                for ((ap, device) in candidates) {
                    val apName = GridHub.apName(ap)
                    val rssi = GridHub.rssiOf(ap)
                    data.note(LinkData.State.CONNECTING, "Asking $apName…")
                    publish()
                    LinkLog.add("trying $apName" + (rssi?.let { " ($it dBm)" } ?: ""))
                    val why = tryOne(app, device, linkKey, apName, wanted) ?: return@execute
                    failures.add(apName to why)
                    // Every AP carries the same answers, so move on to the next one.
                }
                data.note(LinkData.State.ERROR, ApChoice.summarise(failures))
                LinkLog.add("no AP answered")
            } finally {
                GridHub.scanPaused.value = false
                busy.set(false)
                publish()
            }
        }
    }

    /**
     * The scan runs in the watch service, which notices the pause on its next pass. Connecting
     * while it is still going is the whole bug, so wait for it to actually stop — briefly, and
     * carry on regardless if it does not.
     */
    private fun waitForTheScanToStop() {
        val until = System.currentTimeMillis() + SCAN_STOP_MS
        while (GridHub.radio.value == Radio.SCANNING && System.currentTimeMillis() < until) {
            Thread.sleep(50)
        }
        LinkLog.add(
            if (GridHub.radio.value == Radio.SCANNING) "the scan did not stop in time; connecting anyway"
            else "the scan has stood aside",
        )
    }

    /** One AP. Returns null when it answered, or what went wrong, in words. */
    private fun tryOne(
        app: Context,
        device: android.bluetooth.BluetoothDevice,
        linkKey: ByteArray,
        apName: String,
        wantTraffic: Boolean,
    ): String? = try {
        if (BleAdminLink(app, device, linkKey, apName).pull(auth, data, wantTraffic)) {
            null
        } else {
            // The link worked; the login did not. Every AP checks the same password, so there is
            // nothing to gain by asking the next one.
            null.also { LinkLog.add("$apName: ${data.message}") }
        }
    } catch (e: LinkException) {
        (e.message ?: "the link failed").also { LinkLog.add("$apName: $it") }
    } catch (e: SecurityException) {
        "Android did not allow the connection".also { LinkLog.add("$apName: $it") }
    } catch (e: Exception) {
        Log.w(TAG, "[BLE] admin link failed: ${e.javaClass.simpleName}")
        "the link failed (${e.javaClass.simpleName})".also { LinkLog.add("$apName: $it") }
    }

    /** Called once a second while the dashboard is on screen: the app's own gentle cycle. */
    fun tick(context: Context) {
        if (auth.havePassword()) refresh(context, now = false)
        publish()
    }
}
