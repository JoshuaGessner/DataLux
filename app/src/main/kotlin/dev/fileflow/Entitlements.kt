package dev.fileflow

import android.content.Context
import android.content.Intent
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * What this copy of DataLux may do.
 *
 * DataLux is open source: anyone who builds it from source gets every feature, and that is
 * deliberate (-Pstore=false). Normal Android builds ([BuildConfig.STORE]) ask what was bought; buying is how people
 * who don't build it themselves get the extras, and how they support the project.
 */
object Entitlements {

    enum class Tier { Free, Pro }

    enum class Feature(val tier: Tier, val label: String) {
        /** Several files as one transfer. */
        Batch(Tier.Pro, "Sending several files at once"),
        /** A batch sent by desktop or another phone is unlocked on this receiving phone. */
        BatchReceive(Tier.Pro, "Receiving several files at once"),
        /** Selecting received files to share, send or delete together. */
        BulkManage(Tier.Pro, "Managing several received files at once"),
        /** Encrypted sending to a group that holds one key. */
        Teams(Tier.Free, "Teams"),
    }

    /** Updated only after signature verification, never from a client-side purchase flag. */
    @Volatile
    private var purchased: Tier = Tier.Free

    val tier: Tier get() = if (BuildConfig.STORE) purchased else Tier.Pro

    internal fun useVerifiedLicense(license: String?, userId: String) {
        purchased = if (license != null && ProLicense.verify(license, BuildConfig.LICENSE_PUBLIC_KEY, userId))
            Tier.Pro else Tier.Free
    }

    fun has(f: Feature): Boolean = tier >= f.tier

    /** True when [f] is available; otherwise says which plan has it, and false. */
    fun check(context: Context, f: Feature): Boolean {
        if (has(f)) return true
        MaterialAlertDialogBuilder(context)
            .setTitle("DataLux Pro")
            .setMessage("${f.label} needs Pro on this phone. The US launch price is $10, one time, " +
                "with the final price shown by Google Play. Already bought it? Sign in and restore. " +
                "You can keep sending and receiving one file at a time for free, at full speed, with encryption.")
            .setPositiveButton("View Pro / restore") { _, _ -> openPro(context, f) }
            .setNegativeButton("Not now", null)
            .show()
        return false
    }

    fun openPro(context: Context, feature: Feature? = null) {
        context.startActivity(Intent(context, ProActivity::class.java).apply {
            feature?.let { putExtra("feature", it.name) }
        })
    }
}
