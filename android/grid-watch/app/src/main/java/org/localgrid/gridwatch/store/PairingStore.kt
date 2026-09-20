package org.localgrid.gridwatch.store

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import androidx.core.content.edit
import org.localgrid.gridwatch.proto.Pairing
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Keeps the pairing (company ID, grid ID, status key K) in app-private preferences, encrypted
 * with an AES-GCM key that lives in the Android Keystore and never leaves it. Backups are off
 * (manifest), and a Keystore key does not move to another phone, so the pairing stays here.
 */
class PairingStore(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences("pairing", Context.MODE_PRIVATE)

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

    fun load(): Pairing? {
        val blob = prefs.getString(PREF, null) ?: return null
        return try {
            val raw = Base64.decode(blob, Base64.NO_WRAP)
            val iv = raw.copyOfRange(0, 12)
            val c = Cipher.getInstance(TRANSFORM)
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
            Pairing.fromBytes(c.doFinal(raw, 12, raw.size - 12))
        } catch (e: Exception) {
            Log.w(TAG, "[PAIR] stored pairing cannot be opened; pair again (${e.javaClass.simpleName})")
            null
        }
    }

    fun save(p: Pairing) {
        val c = Cipher.getInstance(TRANSFORM)
        c.init(Cipher.ENCRYPT_MODE, key())
        val sealed = c.iv + c.doFinal(p.toBytes())
        prefs.edit { putString(PREF, Base64.encodeToString(sealed, Base64.NO_WRAP)) }
    }

    fun forget() {
        prefs.edit { remove(PREF) }
        try {
            KeyStore.getInstance(KEYSTORE).apply { load(null) }.deleteEntry(ALIAS)
        } catch (e: Exception) {
            Log.w(TAG, "[PAIR] could not delete the keystore key (${e.javaClass.simpleName})")
        }
    }

    private companion object {
        const val TAG = "GridWatch"
        const val KEYSTORE = "AndroidKeyStore"
        const val ALIAS = "lgw-pairing"
        const val PREF = "sealed"
        const val TRANSFORM = "AES/GCM/NoPadding"
    }
}
