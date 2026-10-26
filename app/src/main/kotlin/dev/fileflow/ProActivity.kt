package dev.fileflow

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Looper
import android.text.InputType
import android.view.View
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
        if (!busy && ProAccount.session != null) {
            if (billing?.isReady == true) restore() else refresh()
        }
    }

    private fun render() {
        if (Looper.myLooper() != Looper.getMainLooper()) { runOnUiThread { render() }; return }
        if (isFinishing || isDestroyed) return
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
            add(text(if (session == null) "Sign in to link Pro across your devices. An account is optional for free transfers."
                else "Sign in again to sync your account. Your saved offline Pro access remains available."))
            val email = EditText(this).apply {
                hint = "Email"
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
            add(card(title(if (pro) "Pro is active" else "Free", 20f), space(6), text(if (pro)
                "Your verified Pro access is saved on this device and works offline. Sign in to the same FileFlow account on your other devices."
            else "Sign in on each device to share your FileFlow Pro purchase.")))
            add(space(12))
            val status = ProAccount.status
            add(card(title(if (status?.emailVerified == true) "Email verified" else "Verify your email", 18f), space(6),
                text(if (status?.emailVerified == true) "Your account is ready to link paid features across devices."
                    else "Verify your email before buying Pro or linking a new device. Open the verification email, confirm your FileFlow password, then refresh this account.")))
            if (status?.emailVerified != true) {
                add(button(if (busy) "Please wait…" else "Send verification email", style = Ui.Style.Tonal) {
                    work("Verification request failed", "Check your email for the verification link.") { ProAccount.requestVerification() }
                }.apply { isEnabled = !busy && status?.verificationAvailable == true })
                if (status?.verificationAvailable == false)
                    add(text("Verification email is not configured yet. Free transfers and your existing offline access continue to work."))
            }
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
        if (notice.isNotEmpty()) { add(space(12)); add(text(notice)) }
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
                notice = result.exceptionOrNull()?.let { if (it is AccountError) it.message.orEmpty() else "$failure. Check your connection and try again." }
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
        billing?.endConnection()
        worker.shutdown()
        super.onDestroy()
    }
}
