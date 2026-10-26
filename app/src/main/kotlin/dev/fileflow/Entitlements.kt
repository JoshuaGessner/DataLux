package dev.fileflow

import android.content.Context
import android.content.Intent
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * What this copy of FileFlow may do.
 *
 * FileFlow is open source: anyone who builds it from source gets every feature, and that is
 * deliberate. Only a store build ([BuildConfig.STORE]) asks what was bought; buying is how people
 * who don't build it themselves get the extras, and how they support the project.
 */
object Entitlements {

    enum class Tier { Free, Pro }

    enum class Feature(val tier: Tier, val label: String) {
        /** Several files as one transfer. */
        Batch(Tier.Pro, "Sending several files at once"),
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
            .setTitle("FileFlow Pro")
            .setMessage("${f.label} is part of FileFlow Pro. Basic transfers, encryption, and your received files remain free.")
            .setPositiveButton("View Pro") { _, _ -> context.startActivity(Intent(context, ProActivity::class.java)) }
            .setNegativeButton("Not now", null)
            .show()
        return false
    }
}
