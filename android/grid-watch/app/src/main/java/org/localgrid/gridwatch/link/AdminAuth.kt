package org.localgrid.gridwatch.link

/**
 * The admin password while the app runs.
 *
 * It is held as characters, never as a log line and never in a Compose state that survives the
 * screen. From it this makes the PBKDF2 hash the AP stores (once per salt and iteration count)
 * and, per connection, the HMAC proof. The password itself never crosses the air: only the
 * proof does, and the whole exchange is sealed with the grid's key anyway.
 *
 * "Remember on this phone" is a separate decision, made in the UI and carried out by
 * PasswordStore with the same Android Keystore protection as the pairing key.
 */
class AdminAuth {
    enum class State { NONE, TRYING, OK, FAILED }

    private val lock = Any()
    private var password: CharArray? = null
    private var hash: ByteArray? = null
    private var hashSalt: ByteArray? = null
    private var hashIterations: Int = 0

    @Volatile var state: State = State.NONE
        private set

    @Volatile var message: String = DEFAULT_MESSAGE
        private set

    /** True when the password was loaded from this phone's store rather than typed just now. */
    @Volatile var remembered: Boolean = false
        private set

    val loggedIn: Boolean get() = state == State.OK

    fun havePassword(): Boolean = synchronized(lock) { password != null }

    fun setPassword(chars: CharArray, remembered: Boolean = false) {
        synchronized(lock) {
            wipe()
            password = chars.copyOf()
            this.remembered = remembered
            state = State.TRYING
            message = "Checking the password with an AP…"
        }
    }

    fun forget() {
        synchronized(lock) {
            wipe()
            remembered = false
            state = State.NONE
            message = "Logged out."
        }
    }

    private fun wipe() {
        password?.fill('\u0000')
        hash?.fill(0)
        password = null
        hash = null
        hashSalt = null
        hashIterations = 0
    }

    /** The proof for one connection's challenge, or null when no password has been typed. */
    fun proof(salt: ByteArray, iterations: Int, challenge: ByteArray): ByteArray? {
        val h = synchronized(lock) {
            val pw = password ?: return null
            val cached = hash
            if (cached != null && hashIterations == iterations && hashSalt?.contentEquals(salt) == true) {
                cached
            } else {
                LinkProtocol.loginHash(pw, salt, iterations).also {
                    hash = it
                    hashSalt = salt.copyOf()
                    hashIterations = iterations
                }
            }
        }
        return LinkProtocol.loginProof(h, challenge)
    }

    fun result(ok: Boolean, text: String) {
        synchronized(lock) {
            if (ok) {
                state = State.OK
                message = text
            } else {
                state = State.FAILED
                message = text
            }
        }
    }

    /** A wrong password is dropped at once, so the app stops trying it against every AP. */
    fun wrongPassword(text: String) {
        synchronized(lock) {
            wipe()
            remembered = false
            state = State.FAILED
            message = text
        }
    }

    companion object {
        const val DEFAULT_MESSAGE =
            "Log in with the admin password to see positions, groups, availability and traffic."
    }
}
