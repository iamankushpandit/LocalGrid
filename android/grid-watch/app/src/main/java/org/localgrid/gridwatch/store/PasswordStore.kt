package org.localgrid.gridwatch.store

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import androidx.core.content.edit
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * The admin password, only when the user asked for "remember on this phone".
 *
 * By default nothing is stored: the password lives in memory while the app runs and is gone when
 * it stops. When it is remembered it gets the same protection as the pairing key — app-private
 * preferences sealed with an AES-GCM key that lives in the Android Keystore and never leaves it,
 * with backups and device-to-device transfer off. It is never logged and never leaves the phone:
 * only a proof of it crosses the air (docs/ble-link.md).
 */
class PasswordStore(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences("admin", Context.MODE_PRIVATE)

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        gen.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return gen.generateKey()
    }

    fun load(): CharArray? {
        val blob = prefs.getString(PREF, null) ?: return null
        return try {
            val raw = Base64.decode(blob, Base64.NO_WRAP)
            val c = Cipher.getInstance(TRANSFORM)
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, raw.copyOfRange(0, 12)))
            val bytes = c.doFinal(raw, 12, raw.size - 12)
            val chars = String(bytes, Charsets.UTF_8).toCharArray()
            bytes.fill(0)
            chars
        } catch (e: Exception) {
            Log.w(TAG, "[BLE] the remembered admin password cannot be opened; type it again (${e.javaClass.simpleName})")
            null
        }
    }

    fun save(password: CharArray) {
        val bytes = String(password).toByteArray(Charsets.UTF_8)
        try {
            val c = Cipher.getInstance(TRANSFORM)
            c.init(Cipher.ENCRYPT_MODE, key())
            val sealed = c.iv + c.doFinal(bytes)
            prefs.edit { putString(PREF, Base64.encodeToString(sealed, Base64.NO_WRAP)) }
        } catch (e: Exception) {
            Log.w(TAG, "[BLE] could not remember the admin password (${e.javaClass.simpleName})")
        } finally {
            bytes.fill(0)
        }
    }

    fun forget() {
        prefs.edit { remove(PREF) }
        try {
            KeyStore.getInstance(KEYSTORE).apply { load(null) }.deleteEntry(ALIAS)
        } catch (e: Exception) {
            Log.w(TAG, "[BLE] could not delete the admin password key (${e.javaClass.simpleName})")
        }
    }

    fun remembered(): Boolean = prefs.contains(PREF)

    private companion object {
        const val TAG = "GridWatch"
        const val KEYSTORE = "AndroidKeyStore"
        const val ALIAS = "lgw-admin"
        const val PREF = "sealed"
        const val TRANSFORM = "AES/GCM/NoPadding"
    }
}
