package dev.fileflow

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.os.SystemClock
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.net.URL
import java.security.KeyStore
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.net.ssl.HttpsURLConnection

data class ProSession(
    val token: String,
    val userId: String,
    val email: String,
    val expiresAt: Long,
    val license: String? = null,
    val verificationRequested: Boolean = false,
    val verificationResendAt: Long = 0,
)

data class ProStatus(
    val pro: Boolean,
    val billingEnabled: Boolean,
    val productId: String,
    val purchaseAccountId: String,
    val emailVerified: Boolean,
    val verificationAvailable: Boolean,
    val verificationResendAt: Long = 0,
)

/** Small HTTPS account client. Called on a worker thread, never by the optical transfer path. */
object ProAccount {
    private lateinit var storage: AtomicFile
    private const val keyAlias = "fileflow-account-v1"
    private val background = Executors.newSingleThreadExecutor()
    private val refreshing = AtomicBoolean(false)
    @Volatile private var lastBackgroundRefresh = -300_000L
    @Volatile var session: ProSession? = null
        private set
    @Volatile var status: ProStatus? = null
        private set
    @Volatile private var authenticationRejected = false
    val requiresSignIn: Boolean get() = authenticationRejected || (session?.expiresAt ?: 0) <= System.currentTimeMillis()

    @Synchronized fun initialize(context: Context) {
        storage = AtomicFile(File(context.noBackupFilesDir, "account.enc"))
        session = runCatching {
            require(storage.baseFile.length() in 1..32_768)
            val stored = JSONObject(storage.readFully().toString(Charsets.UTF_8))
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, encryptionKey(), GCMParameterSpec(128, Base64.getDecoder().decode(stored.getString("iv"))))
            val plain = cipher.doFinal(Base64.getDecoder().decode(stored.getString("data")))
            val account = JSONObject(plain.toString(Charsets.UTF_8))
            // A staging build must never send a cached production bearer to another origin.
            require(account.getString("origin") == BuildConfig.ACCOUNT_ORIGIN)
            ProSession(account.getString("token"), account.getString("userId"), account.getString("email"),
                account.getLong("expiresAt"), account.optString("license").takeIf { it.isNotEmpty() },
                account.optBoolean("verificationRequested", false),
                VerificationCooldown.serverDeadline(account.optLong("verificationResendAt", 0)))
        }.getOrNull()
        Entitlements.useVerifiedLicense(session?.license, session?.userId.orEmpty())
    }

    @Synchronized fun login(email: String, password: String) {
        val result = request("/api/native/login", JSONObject().put("email", email).put("password", password))
        val user = result.getJSONObject("user")
        val id = user.getString("id")
        val previous = session?.takeIf { it.userId == id }
        val cached = previous?.license
        val next = ProSession(result.getString("token"), id, user.getString("email"), result.getLong("expiresAt"), cached,
            previous?.verificationRequested ?: false, previous?.verificationResendAt ?: 0)
        persist(next)
        session = next
        authenticationRejected = false
        status = null
        Entitlements.useVerifiedLicense(cached, next.userId)
        refresh()
    }

    /** A quiet, bounded refresh after foregrounding the main app. Offline errors keep access. */
    fun refreshInBackground() {
        if (session == null || SystemClock.elapsedRealtime() - lastBackgroundRefresh < 300_000 || !refreshing.compareAndSet(false, true)) return
        lastBackgroundRefresh = SystemClock.elapsedRealtime()
        background.execute {
            try { runCatching { refresh() } } finally { refreshing.set(false) }
        }
    }

    @Synchronized fun refresh(): ProStatus = applyStatus(request("/api/native/entitlements", token = requireSession().token))

    @Synchronized fun claim(productId: String, purchaseToken: String, expectedUserId: String): ProStatus {
        val account = requireSession()
        if (account.userId != expectedUserId || requiresSignIn)
            throw AccountError("Sign in to the DataLux account used to buy Pro, then restore purchases.")
        return applyStatus(request("/api/native/play-purchase",
            JSONObject().put("productId", productId).put("purchaseToken", purchaseToken), account.token))
    }

    @Synchronized fun requestVerification(): String {
        val account = requireSession()
        if (requiresSignIn) throw AccountError("Sign in again before requesting a verification email.")
        if (status?.emailVerified == true) return "Your email is already verified."
        if (VerificationCooldown.secondsRemaining(account.verificationResendAt) > 0)
            return "Please wait before requesting another verification email."
        // A lost response may still have queued mail. Pace retries without claiming delivery.
        updateVerification(account.copy(verificationResendAt = System.currentTimeMillis() + VerificationCooldown.REQUEST_MILLIS))
        val response = request("/api/native/account/verification/request", JSONObject(), account.token)
        val verified = response.optBoolean("emailVerified", false)
        val resendAt = if (verified) 0 else if (response.has("verificationResendAt"))
            VerificationCooldown.serverDeadline(response.optLong("verificationResendAt", 0))
            else System.currentTimeMillis() + VerificationCooldown.REQUEST_MILLIS
        updateVerification(requireSession().copy(verificationRequested = !verified, verificationResendAt = resendAt))
        status = status?.copy(emailVerified = verified, verificationResendAt = resendAt)
        return if (verified) "Your email is already verified." else "Verification email requested. Check your inbox and spam folder."
    }

    private fun updateVerification(account: ProSession) {
        persist(account)
        session = account
    }

    @Synchronized fun deleteAccount(password: String, confirmation: String, expectedUserId: String) {
        val account = requireSession()
        if (account.userId != expectedUserId || requiresSignIn) throw AccountError("Sign in again before deleting your account.")
        if (confirmation != "DELETE") throw AccountError("Type DELETE to confirm account deletion.")
        val response = request("/api/native/account/delete",
            JSONObject().put("password", password).put("confirmation", confirmation), account.token)
        if (!response.has("user") || !response.isNull("user")) throw AccountError("Account deletion could not be confirmed. Please refresh your account before trying again.")
        // A failed request must keep local account and Pro state. No transfer files or keys are touched.
        clearLocalAccount()
    }

    @Synchronized fun logout() {
        val token = session?.token
        // Remove local credentials and access even if the network is unavailable.
        clearLocalAccount()
        if (token != null) runCatching { request("/api/native/logout", JSONObject(), token) }
    }

    private fun clearLocalAccount() {
        storage.delete()
        session = null
        status = null
        authenticationRejected = false
        Entitlements.useVerifiedLicense(null, "")
    }

    fun expectedPurchaseAccountId(): String = MessageDigest.getInstance("SHA-256")
        .digest("fileflow-play:${requireSession().userId}".toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it.toInt() and 255) }

    private fun applyStatus(body: JSONObject): ProStatus {
        val previous = requireSession()
        val pro = body.getBoolean("pro")
        val license = if (pro) body.getString("license") else null
        if (pro && !ProLicense.verify(license!!, BuildConfig.LICENSE_PUBLIC_KEY, previous.userId))
            throw AccountError("Pro access could not be verified. Please try again later.")
        val billing = body.getJSONObject("billing")
        if (billing.getString("productId") != "fileflow_pro") throw AccountError("This Pro product is not supported by this version.")
        val accountId = body.getString("purchaseAccountId")
        if (accountId != expectedPurchaseAccountId()) throw AccountError("The account response could not be verified.")
        val emailVerified = body.optBoolean("emailVerified", false)
        val serverRequested = body.optLong("verificationResendAt", 0) > 0
        val serverResendAt = VerificationCooldown.serverDeadline(body.optLong("verificationResendAt", 0))
        val resendAt = if (emailVerified) 0 else maxOf(previous.verificationResendAt, serverResendAt)
        val next = previous.copy(license = license, verificationRequested = !emailVerified &&
            (previous.verificationRequested || serverRequested), verificationResendAt = resendAt)
        persist(next)
        session = next
        Entitlements.useVerifiedLicense(license, next.userId)
        return ProStatus(pro, billing.getBoolean("enabled") && emailVerified, billing.getString("productId"), accountId,
            emailVerified, body.optBoolean("verificationAvailable", false), resendAt).also { status = it }
    }

    private fun requireSession() = session ?: throw AccountError("Sign in to your DataLux account first.")

    private fun persist(account: ProSession) {
        val json = JSONObject().put("origin", BuildConfig.ACCOUNT_ORIGIN)
            .put("token", account.token).put("userId", account.userId).put("email", account.email)
            .put("expiresAt", account.expiresAt).put("license", account.license.orEmpty())
            .put("verificationRequested", account.verificationRequested).put("verificationResendAt", account.verificationResendAt)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, encryptionKey())
        val encrypted = cipher.doFinal(json.toString().toByteArray(Charsets.UTF_8))
        val wrapped = JSONObject().put("iv", Base64.getEncoder().encodeToString(cipher.iv))
            .put("data", Base64.getEncoder().encodeToString(encrypted))
        val out = storage.startWrite()
        try {
            out.write(wrapped.toString().toByteArray(Charsets.UTF_8))
            storage.finishWrite(out)
        } catch (error: Exception) {
            storage.failWrite(out)
            throw error
        }
    }

    private fun encryptionKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(keyAlias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(keyAlias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
            generateKey()
        }
    }

    private fun request(route: String, body: JSONObject? = null, token: String? = null): JSONObject {
        val connection = URL(BuildConfig.ACCOUNT_ORIGIN + route).openConnection() as HttpsURLConnection
        connection.connectTimeout = 15_000
        connection.readTimeout = 20_000
        connection.instanceFollowRedirects = false
        connection.requestMethod = if (body == null) "GET" else "POST"
        connection.setRequestProperty("Accept", "application/json")
        token?.let { connection.setRequestProperty("Authorization", "Bearer $it") }
        try {
            if (body != null) {
                connection.doOutput = true
                connection.setRequestProperty("Content-Type", "application/json")
                connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            }
            val code = connection.responseCode
            if (code !in 200..299) {
                // Read only a bounded error code. Server text may contain implementation details.
                val errorBytes = connection.errorStream?.use { it.readNBytes(8193) }
                val errorCode = if (errorBytes != null && errorBytes.size <= 8192)
                    runCatching { JSONObject(errorBytes.toString(Charsets.UTF_8)).optString("code") }.getOrDefault("") else ""
                if (code == 429 && route == "/api/native/account/verification/request") {
                    session?.let { updateVerification(it.copy(verificationResendAt =
                        VerificationCooldown.retryDeadline(connection.getHeaderField("Retry-After")))) }
                }
                val error = AccountError.fromResponse(code, errorCode, token != null,
                    route == "/api/native/account/delete", route == "/api/native/account/verification/request")
                if (error.invalidatesAuthentication) { status = null; authenticationRejected = true }
                throw error
            }
            val data = connection.inputStream.use { it.readNBytes(65_537) }
            if (data.size > 65_536) throw AccountError("The account response was too large.")
            return JSONObject(data.toString(Charsets.UTF_8))
        } finally {
            connection.disconnect()
        }
    }
}
