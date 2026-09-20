package org.localgrid.gridwatch.link

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * One short BLE connection to one AP: connect, say what is needed, disconnect (D70).
 *
 * The AP takes one client at a time and drops an idle link after 60 s, so this never lingers:
 * the app connects when the user is looking or asks, pulls, and lets go. Nothing is advertised,
 * nothing is written to the grid, and the AP's hardware address is never logged or shown (D21) —
 * the [device] handle comes straight from the scan and is used and dropped.
 *
 * This is the only part of the app that transmits at all, and it transmits only to an AP's BLE
 * admin service: it still never joins the grid's Wi-Fi and never carries a grid message.
 *
 * Every step writes a line to [LinkLog], and every failure says what actually happened with the
 * GATT status code, because on Android these failures are nothing alike and a guess is useless.
 *
 * Two things learned the hard way on a Galaxy S24+ (2026-09-20):
 *  - The ATT MTU must be checked, not assumed. Android falls back to the 23-byte default when
 *    the radio is busy, and the AP then closes the link rather than answer, because a 23-byte
 *    chunk cannot carry a body. The app now refuses to talk below [LinkProtocol.MTU_MIN] and
 *    says so, instead of writing a request nobody can answer.
 *  - A link that drops mid-conversation has to wake whoever is waiting for a reply. It used to
 *    let them sit out the twelve-second timeout and then blame the AP for being busy.
 */
class BleAdminLink(
    private val context: Context,
    private val device: BluetoothDevice,
    private val key: ByteArray,
    private val apName: String,
) {
    private val ready = LinkBlocker()
    private val written = LinkBlocker()
    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private lateinit var session: LinkSession
    private var request: BluetoothGattCharacteristic? = null

    @Volatile private var mtu = DEFAULT_MTU
    @Volatile private var talking = false          // past the subscription: a drop must wake the wait

    private class LinkBlocker {
        private val q = LinkedBlockingQueue<Any>()
        fun signal(v: Any = OK) = q.put(v)
        fun await(timeoutMs: Long, what: String): Any =
            q.poll(timeoutMs, TimeUnit.MILLISECONDS) ?: throw LinkException("$what timed out")
        companion object { val OK = Any() }
    }

    private fun log(text: String) = LinkLog.add("$apName: $text")

    private fun giveUp(reason: String) {
        log(reason)
        ready.signal(LinkException(reason))
        written.signal(LinkException(reason))
        if (talking) session.fail(reason)
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                log("connected (${GattStatus.describe(status)})")
                // Ask for a fast connection interval: the whole conversation is a few packets and
                // the AP drops an idle watcher, so there is no reason to dawdle.
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                // A breath before the MTU request: Samsung's stack answers badly if it is asked
                // in the same instant the connection completes.
                main.postDelayed({ askForMtu(g) }, SETTLE_MS)
                return
            }
            if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                giveUp(GattStatus.disconnectReason(apName, status))
            }
        }

        @SuppressLint("MissingPermission")
        private fun askForMtu(g: BluetoothGatt) {
            if (!g.requestMtu(LinkProtocol.MTU_WANTED)) {
                giveUp("this phone would not even ask $apName for a bigger MTU")
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, value: Int, status: Int) {
            mtu = value
            log("MTU $value (${GattStatus.describe(status)})")
            if (value < LinkProtocol.MTU_MIN) {
                // Talking anyway would write a request the AP cannot answer, and the AP would
                // close the link: that is exactly what looked like "busy" before.
                giveUp(GattStatus.mtuTooSmall(apName, value))
                return
            }
            // Keep a request chunk inside what the connection negotiated. Requests are tiny.
            session.chunk = maxOf(
                16,
                minOf(LinkProtocol.CHUNK_BODY, value - 3 - LinkProtocol.HEADER - LinkProtocol.TAG),
            )
            main.postDelayed({ discover(g) }, SETTLE_MS)
        }

        @SuppressLint("MissingPermission")
        private fun discover(g: BluetoothGatt) {
            if (!g.discoverServices()) giveUp("this phone would not look up $apName's services")
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != GattStatus.SUCCESS) {
                giveUp("looking up $apName's services failed: ${GattStatus.describe(status)}")
                return
            }
            val service = g.getService(LinkProtocol.SERVICE)
            if (service == null) {
                giveUp("$apName has no admin link service (firmware older than D70)")
                return
            }
            val req = service.getCharacteristic(LinkProtocol.REQUEST)
            val reply = service.getCharacteristic(LinkProtocol.REPLY)
            if (req == null || reply == null) {
                giveUp("$apName's admin link service is not the one this app speaks")
                return
            }
            request = req
            log("found the admin link service")
            if (!g.setCharacteristicNotification(reply, true)) {
                giveUp("this phone would not turn on notifications for $apName")
                return
            }
            val cccd = reply.getDescriptor(LinkProtocol.CCCD)
            if (cccd == null) {
                giveUp("$apName's reply characteristic has no notify descriptor")
                return
            }
            val on = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            val sent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                val code = g.writeDescriptor(cccd, on)
                if (code != BluetoothStatusCodes.SUCCESS) {
                    giveUp("this phone could not subscribe to $apName's replies (code $code)")
                    return
                }
                true
            } else {
                cccd.value = on
                g.writeDescriptor(cccd)
            }
            if (!sent) giveUp("this phone could not subscribe to $apName's replies")
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (status == GattStatus.SUCCESS) {
                talking = true
                log("subscribed; waiting for the session number")
                ready.signal()
            } else {
                giveUp("$apName refused the subscription: ${GattStatus.describe(status)}")
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (status == GattStatus.SUCCESS) {
                written.signal()
            } else {
                val why = "a request to $apName was not accepted: ${GattStatus.describe(status)}"
                log(why)
                written.signal(LinkException(why))
            }
        }

        // Android 13 and newer.
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == LinkProtocol.REPLY) session.feed(value)
        }

        @Deprecated("Kept for Android 12, which has no value parameter.")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (c.uuid == LinkProtocol.REPLY) c.value?.let { session.feed(it.copyOf()) }
        }
    }

    @SuppressLint("MissingPermission")     // the caller checks BLUETOOTH_CONNECT first
    private fun write(payload: ByteArray) {
        val g = gatt ?: throw LinkException("the link is gone")
        val c = request ?: throw LinkException("the link has no request characteristic")
        if (payload.size > mtu - 3) {
            throw LinkException(
                "a ${payload.size}-byte request does not fit the ${mtu}-byte MTU agreed with $apName",
            )
        }
        val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, payload, type) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                c.writeType = type
                c.value = payload
                g.writeCharacteristic(c)
            }
        }
        if (!ok) throw LinkException("this phone could not send a request to $apName")
        val got = written.await(WRITE_MS, "a request to $apName")
        if (got is LinkException) throw got
    }

    /**
     * Connects, pulls, and disconnects. Blocking: call it from a worker thread, never from the
     * UI thread. Returns true when the login worked and the replies arrived.
     */
    @SuppressLint("MissingPermission")
    fun pull(auth: AdminAuth, data: LinkData, wantTraffic: Boolean): Boolean {
        session = LinkSession(key) { write(it) }
        log("connecting")
        try {
            gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
                ?: throw LinkException("this phone would not open a link to $apName")
            val got = ready.await(CONNECT_MS, "connecting to $apName")
            if (got is LinkException) throw got
            val ok = pullOnce(session, auth, data, apName, wantTraffic)
            log(if (ok) "done" else "logged out again: ${data.message}")
            return ok
        } finally {
            talking = false
            val g = gatt
            gatt = null
            request = null
            try {
                g?.disconnect()
                g?.close()
            } catch (e: Exception) {
                log("closing the link: ${e.javaClass.simpleName}")
            }
        }
    }

    private companion object {
        const val DEFAULT_MTU = 23         // what Android uses until it negotiates something better
        const val CONNECT_MS = 15_000L
        const val WRITE_MS = 8_000L
        const val SETTLE_MS = 250L         // a breath between connect, MTU and discovery
    }
}
