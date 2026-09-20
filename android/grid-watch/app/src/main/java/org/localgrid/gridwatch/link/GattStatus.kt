package org.localgrid.gridwatch.link

/**
 * What Android's GATT status codes and the link's own failures actually mean, in words.
 *
 * Every failure has to say what happened, with the status code where there is one. "Busy" is
 * `ERROR` code 2 from the AP and nothing else: a timeout, a dropped connection, a missing
 * service, a refused MTU or a failed subscription are all different problems, and collapsing
 * them into one guess has cost the owner time twice.
 */
object GattStatus {
    const val SUCCESS = 0
    const val CONN_L2C_FAILURE = 1
    const val INSUFFICIENT_AUTHENTICATION = 5
    const val REQUEST_NOT_SUPPORTED = 6
    const val INSUFFICIENT_ENCRYPTION = 15
    const val CONNECTION_TIMEOUT = 8
    const val CONNECTION_TERMINATED_BY_PEER = 19
    const val CONNECTION_TERMINATED_LOCAL = 22
    const val LMP_RESPONSE_TIMEOUT = 34
    const val CONNECTION_FAIL_TO_ESTABLISH = 62
    const val CONN_CANCEL = 133             // Android's catch-all "GATT_ERROR"
    const val CONN_TERMINATE_LOCAL_HOST = 256

    /** Plain English for one status code, always ending with the number itself. */
    fun describe(status: Int): String = when (status) {
        SUCCESS -> "all right (0)"
        CONN_L2C_FAILURE -> "the Bluetooth stack could not open the channel (1)"
        INSUFFICIENT_AUTHENTICATION -> "the AP asked for pairing, which this link does not use (5)"
        REQUEST_NOT_SUPPORTED -> "the AP does not support that request (6)"
        CONNECTION_TIMEOUT -> "the connection timed out: the AP went out of range or stopped answering (8)"
        INSUFFICIENT_ENCRYPTION -> "the AP asked for encryption, which this link does not use (15)"
        CONNECTION_TERMINATED_BY_PEER -> "the AP closed the link (19)"
        CONNECTION_TERMINATED_LOCAL -> "this phone closed the link (22)"
        LMP_RESPONSE_TIMEOUT -> "the radios stopped answering each other (34)"
        CONNECTION_FAIL_TO_ESTABLISH -> "the connection never got established (62)"
        CONN_CANCEL -> "Android's general Bluetooth failure (133), usually a busy radio: " +
            "another app is scanning or connecting, or the AP is already talking to someone"
        CONN_TERMINATE_LOCAL_HOST -> "Android tore the link down (256)"
        else -> "GATT status $status"
    }

    /** What to say when the link dropped in the middle of a conversation. */
    fun disconnectReason(apName: String, status: Int): String =
        if (status == SUCCESS) "$apName closed the link"
        else "$apName's link dropped: ${describe(status)}"

    /**
     * The AP refuses an ATT MTU under 64, because a chunk would carry no body. Android falls back
     * to the 23-byte default when the radio is busy — notably while this app's own scan is
     * running — so this is worth saying exactly rather than letting it look like a timeout.
     */
    fun mtuTooSmall(apName: String, mtu: Int): String =
        "this phone and $apName settled on an ATT MTU of $mtu; the admin link needs at least " +
            "${LinkProtocol.MTU_MIN}. The radio was busy when the link opened."
}
