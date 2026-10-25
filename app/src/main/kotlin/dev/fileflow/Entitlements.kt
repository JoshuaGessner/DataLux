package dev.fileflow

import android.content.Context
import dev.fileflow.ui.Ui.showHelp

/**
 * What this copy of FileFlow may do.
 *
 * FileFlow is open source: anyone who builds it from source gets every feature, and that is
 * deliberate. Only a store build ([BuildConfig.STORE]) asks what was bought; buying is how people
 * who don't build it themselves get the extras, and how they support the project.
 */
object Entitlements {

    enum class Tier { Free, Supporter, Pro, Enterprise }

    enum class Feature(val tier: Tier, val label: String) {
        /** Several files as one transfer. */
        Batch(Tier.Supporter, "Sending several files at once"),
        /** Selecting received files to share, send or delete together. */
        BulkManage(Tier.Supporter, "Managing several received files at once"),
        /** Encrypted sending to a group that holds one key. */
        Teams(Tier.Pro, "Teams"),
    }

    /** What was bought, in a store build. Set by billing; Free until then. */
    @Volatile
    var purchased: Tier = Tier.Free

    val tier: Tier get() = if (BuildConfig.STORE) purchased else Tier.Enterprise

    fun has(f: Feature): Boolean = tier >= f.tier

    /** True when [f] is available; otherwise says which plan has it, and false. */
    fun check(context: Context, f: Feature): Boolean {
        if (has(f)) return true
        context.showHelp(f.label, "${f.label} is part of FileFlow ${f.tier}. Everything you've " +
            "already received stays yours, and sending and receiving single files is always free.")
        return false
    }
}
