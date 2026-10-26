package dev.fileflow

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import com.android.billingclient.api.BillingClient
import com.android.billingclient.api.BillingClientStateListener
import com.android.billingclient.api.BillingFlowParams
import com.android.billingclient.api.BillingResult
import com.android.billingclient.api.PendingPurchasesParams
import com.android.billingclient.api.ProductDetails
import com.android.billingclient.api.Purchase
import com.android.billingclient.api.QueryProductDetailsParams
import com.android.billingclient.api.QueryPurchasesParams
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dev.fileflow.ui.Ui
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.capped
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title
import java.util.concurrent.Executors

/** Optional account and Play purchase screen. No file or camera data is read here. */
class ProActivity : AppCompatActivity() {
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var body: LinearLayout
    private var busy = false
    private var notice = ""
    private var verificationRequestInFlight = false
    private val verificationTimer = Handler(Looper.getMainLooper())
    private var verificationButton: Button? = null
    private val verificationTick = object : Runnable {
        override fun run() {
            updateVerificationButton()
            verificationTimer.postDelayed(this, 1000)
        }
    }
    private var billing: BillingClient? = null
    private var billingAccountId: String? = null
    private var product: ProductDetails? = null
    private var connecting = false
    private var deferredPurchases: Pair<String, List<Purchase>>? = null
    // Only touched by the serial worker. A failed verification remains retryable.
    private val verifiedPurchases = mutableSetOf<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(24))
        }
        setContentView(ScrollView(this).apply { addView(capped(body)) }.padForSystemBars())
        render()
        if (ProAccount.session != null) refresh()
    }

    override fun onResume() {
        super.onResume()
        verificationTimer.removeCallbacks(verificationTick)
        verificationTimer.post(verificationTick)
        if (!busy && ProAccount.session != null) {
            if (billing?.isReady == true) restore() else refresh()
        }
    }

    override fun onPause() {
        verificationTimer.removeCallbacks(verificationTick)
        super.onPause()
    }

    private fun render() {
        if (Looper.myLooper() != Looper.getMainLooper()) { runOnUiThread { render() }; return }
        if (isFinishing || isDestroyed) return
        verificationButton = null
        body.removeAllViews()
        add(button("Back", style = Ui.Style.Text) { finish() })
        add(title("FileFlow Pro", 30f))
        add(space(8))
        add(text("One purchase for your FileFlow account, on phone and PC. Pro adds batch sending and bulk file management. Basic transfers and encryption stay free."))
        add(space(16))
        val session = ProAccount.session
        if (!BuildConfig.STORE) {
            add(card(title("Source build", 20f), space(6), text("All features are included in this build. No purchase is needed.")))
            add(space(12))
        }
        if (session == null || ProAccount.requiresSignIn) {
            if (session?.verificationRequested == true) {
                add(card(title("Return from verification?", 20f), space(6),
                    text("Verifying your email signs you out for security. Sign in again below to confirm it is complete. Your saved offline Pro access remains available.")))
                add(space(12))
            } else add(text(if (session == null) "Sign in to link Pro across your devices. An account is optional for free transfers."
                else "Sign in again to sync your account. Your saved offline Pro access remains available."))
            val email = EditText(this).apply {
                hint = "Email"
                setText(session?.email.orEmpty())
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
                setSingleLine()
                isEnabled = !busy
                importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_YES
                setAutofillHints(View.AUTOFILL_HINT_EMAIL_ADDRESS)
            }
            val password = EditText(this).apply {
                hint = "Password"
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                setSingleLine()
                isEnabled = !busy
                setAutofillHints(View.AUTOFILL_HINT_PASSWORD)
            }
            add(email)
            add(password)
            add(button(if (busy) "Signing in…" else "Sign in") {
                val address = email.text.toString().trim()
                val secret = password.text.toString()
                password.text.clear()
                work("Sign in failed") { ProAccount.login(address, secret) }
            }.apply { isEnabled = !busy })
            add(button("Create a free account", style = Ui.Style.Text) {
                openAccountPage("/app.html#account")
            })
            add(button("Forgot password?", style = Ui.Style.Text) { openAccountPage("/account.html#forgot") })
            if (session != null) add(button("Sign out on this device", style = Ui.Style.Text) { work("Sign out failed") { ProAccount.logout() } }
                .apply { isEnabled = !busy })
        } else {
            add(text(session.email))
            add(space(8))
            val pro = session.license?.let { ProLicense.verify(it, BuildConfig.LICENSE_PUBLIC_KEY, session.userId) } == true
            val status = ProAccount.status
            addVerification(session, status)
            add(space(12))
            add(card(title(if (pro) "Pro is active" else "Free", 20f), space(6), text(if (pro)
                "Your verified Pro access is saved on this device and works offline. Sign in to the same FileFlow account on your other devices."
            else "Sign in on each device to share your FileFlow Pro purchase.")))
            add(space(12))
            val offer = permanentOffer(product)
            if (BuildConfig.STORE && !pro) {
                val ready = status?.emailVerified == true && status.billingEnabled && BuildConfig.LICENSE_PUBLIC_KEY.isNotEmpty() && offer != null && billing?.isReady == true
                add(button(if (offer != null) "Get Pro · ${offer.formattedPrice}" else "Get Pro") { buy() }
                    .apply { isEnabled = ready && !busy })
                if (BuildConfig.LICENSE_PUBLIC_KEY.isEmpty() || status?.billingEnabled == false)
                    add(text("Pro purchases aren’t available in this build yet. Free transfers continue to work."))
            }
            add(button("Restore purchases", style = Ui.Style.Tonal) {
                if (billing?.isReady == true) restore() else refresh()
            }.apply { isEnabled = !busy })
            add(button("Refresh account", style = Ui.Style.Text) { refresh() }.apply { isEnabled = !busy })
            add(button("Sign out on this device", style = Ui.Style.Text) { work("Sign out failed") { ProAccount.logout() } }
                .apply { isEnabled = !busy })
            add(space(16))
            add(button("Delete FileFlow account", style = Ui.Style.Text) { confirmDeletion(session.userId) }
                .apply { isEnabled = !busy })
        }
        add(button("Privacy & data", style = Ui.Style.Text) { openAccountPage("/privacy.html") })
        if (notice.isNotEmpty()) { add(space(12)); add(text(notice)) }
    }

    private fun addVerification(session: ProSession, status: ProStatus?) {
        if (status?.emailVerified == true) {
            add(card(title("Email verified", 20f), space(6),
                text("${session.email} is verified. Your account is ready to connect devices and share Pro access.")))
            return
        }
        val requested = session.verificationRequested
        val instructions = if (requested)
            "A verification email was requested for ${session.email}. Delivery can take a few minutes. Check your inbox and spam folder."
        else "Verify ${session.email} before buying Pro or connecting a PC. Free transfers remain available."
        val resend = button("Send verification email", style = Ui.Style.Tonal) {
            verificationRequestInFlight = true
            work("Verification request failed", "Verification request complete. Check the email status above.") { ProAccount.requestVerification() }
        }
        verificationButton = resend
        val check = button("I’ve verified — check account", style = Ui.Style.Text) { refresh() }.apply { isEnabled = !busy }
        add(card(title(if (requested) "Check your inbox" else "Verify your email", 20f), space(8), text(instructions), space(8),
            text("1. Open the FileFlow verification email.\n2. Follow its link and confirm your FileFlow password.\n3. Return here and check your account. Verification signs you out, so you may need to sign in again."),
            space(8), resend, check))
        if (requested) add(button("Open email app", style = Ui.Style.Text) {
            runCatching { startActivity(Intent.makeMainSelectorActivity(Intent.ACTION_MAIN, Intent.CATEGORY_APP_EMAIL)) }
                .onFailure { showNotice("Open your email app or webmail, then look for the FileFlow verification email.") }
        }.apply { isEnabled = !busy })
        if (status?.verificationAvailable == false)
            add(text("Verification email is unavailable right now. Your free transfers and existing offline access still work."))
        updateVerificationButton()
    }

    /** Update just the button: a countdown must not rebuild forms or steal input focus. */
    private fun updateVerificationButton() {
        val button = verificationButton ?: return
        val seconds = VerificationCooldown.secondsRemaining(ProAccount.session?.verificationResendAt ?: 0)
        button.text = when {
            busy -> if (verificationRequestInFlight) "Requesting email…" else "Please wait…"
            seconds > 0 -> "Resend available in ${seconds}s"
            ProAccount.session?.verificationRequested == true -> "Resend verification email"
            else -> "Send verification email"
        }
        button.isEnabled = !busy && seconds == 0L && ProAccount.status?.verificationAvailable == true && !ProAccount.requiresSignIn
    }

    private fun add(view: View) = body.addView(view, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

    private fun openAccountPage(path: String) {
        runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(BuildConfig.ACCOUNT_ORIGIN + path))) }
            .onFailure { showNotice("Open ${BuildConfig.ACCOUNT_ORIGIN + path} in your browser.") }
    }

    private fun confirmDeletion(expectedUserId: String) {
        if (busy || ProAccount.session?.userId != expectedUserId || ProAccount.requiresSignIn) return
        val password = EditText(this).apply {
            hint = "FileFlow password"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setSingleLine()
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
        }
        val confirmation = EditText(this).apply {
            hint = "Type DELETE"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
            setSingleLine()
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
        }
        val fields = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), 0, dp(24), 0)
            addView(text("This permanently removes your FileFlow account and shared Pro access. It does not delete your local files or transfer keys, or cancel or refund a Google Play purchase. Previously activated offline copies may keep working until they reconnect."))
            addView(password)
            addView(confirmation)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Delete your FileFlow account?")
            .setView(fields)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Delete account", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE).setOnClickListener {
                if (confirmation.text.toString() != "DELETE") {
                    confirmation.error = "Type DELETE to confirm."
                    return@setOnClickListener
                }
                val secret = password.text.toString()
                if (secret.isEmpty()) {
                    password.error = "Enter your FileFlow password."
                    return@setOnClickListener
                }
                password.text.clear()
                confirmation.text.clear()
                dialog.dismiss()
                work("Account deletion could not be confirmed", "Your FileFlow account was deleted. Local files and transfer keys remain on this device.") {
                    ProAccount.deleteAccount(secret, "DELETE", expectedUserId)
                }
            }
        }
        dialog.setOnDismissListener { password.text.clear(); confirmation.text.clear() }
        dialog.show()
    }

    private fun work(failure: String, success: String = "Account updated.", action: () -> Unit) {
        if (busy || worker.isShutdown) return
        busy = true
        render()
        worker.execute {
            val result = runCatching(action)
            runOnUiThread {
                busy = false
                verificationRequestInFlight = false
                notice = result.exceptionOrNull()?.let {
                    if (it is AccountError && it.invalidatesAuthentication && ProAccount.session?.verificationRequested == true)
                        "If you just verified your email, sign in again below to finish. Your saved offline access is kept."
                    else if (it is AccountError) it.message.orEmpty() else "$failure. Check your connection and try again."
                }
                    ?: success
                render()
                if (ProAccount.session == null) {
                    billing?.endConnection()
                    billing = null
                    billingAccountId = null
                    product = null
                }
                if (result.isSuccess) connectBilling()
                deferredPurchases?.let { (accountId, purchases) -> deferredPurchases = null; processPurchases(purchases, accountId) }
            }
        }
    }

    private fun refresh() = work("Account refresh failed") { ProAccount.refresh() }

    private fun connectBilling() {
        val accountId = ProAccount.session?.userId ?: return
        if (!BuildConfig.STORE || BuildConfig.LICENSE_PUBLIC_KEY.isEmpty() || ProAccount.session == null ||
            ProAccount.requiresSignIn || ProAccount.status?.billingEnabled != true || ProAccount.status?.emailVerified != true ||
            (billing?.isReady == true && billingAccountId == accountId) || connecting || isDestroyed) return
        connecting = true
        billing?.endConnection()
        product = null
        billingAccountId = accountId
        billing = BillingClient.newBuilder(this)
            .setListener { result, purchases ->
                runOnUiThread {
                    if (ProAccount.session?.userId != accountId) {
                        showNotice("Sign in to the FileFlow account used to buy Pro, then restore purchases.")
                    } else when (result.responseCode) {
                        BillingClient.BillingResponseCode.OK -> processPurchases(purchases.orEmpty(), accountId)
                        BillingClient.BillingResponseCode.USER_CANCELED -> showNotice("Purchase canceled. Nothing changed.")
                        BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED -> restore()
                        else -> showNotice("Google Play could not complete the purchase. Please try again.")
                    }
                }
            }
            .enablePendingPurchases(PendingPurchasesParams.newBuilder().enableOneTimeProducts().build())
            .enableAutoServiceReconnection()
            .build()
        billing!!.startConnection(object : BillingClientStateListener {
            override fun onBillingSetupFinished(result: BillingResult) {
                connecting = false
                if (result.responseCode == BillingClient.BillingResponseCode.OK) {
                    queryProduct()
                    restore()
                } else showNotice("Google Play purchases are unavailable on this device. Your existing offline access is unchanged.")
            }
            override fun onBillingServiceDisconnected() { connecting = false }
        })
    }

    private fun queryProduct() {
        val productId = ProAccount.status?.productId ?: return
        val accountId = ProAccount.session?.userId ?: return
        val client = billing ?: return
        client.queryProductDetailsAsync(QueryProductDetailsParams.newBuilder().setProductList(listOf(
            QueryProductDetailsParams.Product.newBuilder().setProductId(productId).setProductType(BillingClient.ProductType.INAPP).build(),
        )).build()) { result, details ->
            runOnUiThread {
                if (isDestroyed || billing !== client || ProAccount.session?.userId != accountId) return@runOnUiThread
                product = if (result.responseCode == BillingClient.BillingResponseCode.OK)
                    details.productDetailsList.singleOrNull { it.productId == productId } else null
                if (permanentOffer(product) == null)
                    notice = "Google Play hasn’t made a permanent Pro purchase available for this account yet. Try restoring an existing purchase."
                render()
            }
        }
    }

    /** Initial store configuration uses one base BUY option, with no rentals or preorders. */
    private fun permanentOffer(detail: ProductDetails?): ProductDetails.OneTimePurchaseOfferDetails? {
        if (detail == null) return null
        val offers = detail.oneTimePurchaseOfferDetailsList
            ?: listOfNotNull(detail.oneTimePurchaseOfferDetails)
        return offers.singleOrNull { it.rentalDetails == null && it.preorderDetails == null && it.offerId.isNullOrEmpty() }
    }

    private fun buy() {
        val client = billing ?: return
        val detail = product ?: return
        val offer = permanentOffer(detail) ?: return
        if (busy || !client.isReady || ProAccount.status?.billingEnabled != true || ProAccount.status?.emailVerified != true || BuildConfig.LICENSE_PUBLIC_KEY.isEmpty() ||
            ProAccount.requiresSignIn || billingAccountId != ProAccount.session?.userId) return
        val item = BillingFlowParams.ProductDetailsParams.newBuilder().setProductDetails(detail)
        offer.offerToken?.let { item.setOfferToken(it) }
        val params = BillingFlowParams.newBuilder()
            .setObfuscatedAccountId(ProAccount.expectedPurchaseAccountId())
            .setProductDetailsParamsList(listOf(item.build()))
            .build()
        val result = client.launchBillingFlow(this, params)
        if (result.responseCode == BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED) restore()
        else if (result.responseCode != BillingClient.BillingResponseCode.OK)
            showNotice("Google Play could not open the purchase screen. Please try again.")
    }

    private fun restore() {
        if (busy) return
        val accountId = ProAccount.session?.userId ?: return
        billing?.queryPurchasesAsync(QueryPurchasesParams.newBuilder().setProductType(BillingClient.ProductType.INAPP).build()) { result, purchases ->
            if (result.responseCode == BillingClient.BillingResponseCode.OK) processPurchases(purchases, accountId)
            else showNotice("Purchases could not be restored right now. Your existing offline access is unchanged.")
        }
    }

    private fun processPurchases(purchases: List<Purchase>, expectedAccountId: String) {
        if (Looper.myLooper() != Looper.getMainLooper()) { runOnUiThread { processPurchases(purchases, expectedAccountId) }; return }
        if (ProAccount.session?.userId != expectedAccountId) {
            showNotice("Sign in to the FileFlow account used to buy Pro, then restore purchases.")
            return
        }
        if (busy) { deferredPurchases = expectedAccountId to purchases; return }
        val productId = ProAccount.status?.productId ?: return
        val matching = purchases.filter { productId in it.products }
        val pending = matching.any { it.purchaseState == Purchase.PurchaseState.PENDING }
        val completed = matching.filter { it.purchaseState == Purchase.PurchaseState.PURCHASED }
        work("Purchase verification failed", if (pending && completed.isEmpty())
            "Payment is pending. Pro will unlock after Google Play confirms payment." else "Purchases restored.") {
            for (purchase in completed) {
                val identity = "$expectedAccountId:${purchase.purchaseToken}"
                if (identity !in verifiedPurchases) {
                    ProAccount.claim(productId, purchase.purchaseToken, expectedAccountId)
                    verifiedPurchases += identity
                }
            }
            // Empty Play results must not erase a purchase made on another platform. The server
            // owns the account entitlement and reconciles verified refunds and revocations.
            ProAccount.refresh()
        }
    }

    private fun showNotice(message: String) = runOnUiThread { notice = message; render() }

    override fun onDestroy() {
        verificationTimer.removeCallbacks(verificationTick)
        billing?.endConnection()
        worker.shutdown()
        super.onDestroy()
    }
}
