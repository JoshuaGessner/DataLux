package dev.fileflow.secure

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import androidx.core.content.edit
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** This phone's [TeamStore]: the list in private preferences, each key wrapped by the Keystore. */
object Teams {
    private const val ALIAS = "fileflow-teams"
    private const val PREFS = "teams"
    private const val PREF_LIST = "list"

    @Volatile
    private var store: TeamStore? = null

    fun store(context: Context): TeamStore = store ?: synchronized(this) {
        store ?: run {
            val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            TeamStore(
                object : TeamStore.Storage {
                    override fun load() = prefs.getString(PREF_LIST, null)
                    // Written through at once: a team key lost to a crash means being invited again.
                    override fun save(s: String) = prefs.edit(commit = true) { putString(PREF_LIST, s) }
                },
                KeystoreWrap,
            ).also { store = it }
        }
    }

    private object KeystoreWrap : TeamStore.KeyWrap {
        private const val IV_BYTES = 12

        private fun key(): SecretKey {
            val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
            return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
                init(KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build())
                generateKey()
            }
        }

        override fun wrap(key: ByteArray): ByteArray {
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.ENCRYPT_MODE, key())  // the Keystore picks the IV
            return c.iv + c.doFinal(key)
        }

        override fun unwrap(wrapped: ByteArray): ByteArray {
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, wrapped, 0, IV_BYTES))
            return c.doFinal(wrapped, IV_BYTES, wrapped.size - IV_BYTES)
        }
    }
}
