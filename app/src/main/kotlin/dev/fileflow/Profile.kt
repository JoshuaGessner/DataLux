package dev.fileflow

import android.content.Context
import android.provider.Settings
import androidx.core.content.edit
import dev.fileflow.secure.Team

/**
 * The person using this phone, as teams know them: a name, given on joining and shown in every
 * member's list. Set in Teams › Your name; until then, the name the phone itself goes by
 * ("Josh's Pixel"), or its model.
 */
object Profile {
    private const val PREFS = "profile"
    private const val PREF_NAME = "name"

    fun name(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(PREF_NAME, null)
            ?.takeIf { it.isNotBlank() } ?: defaultName(context)

    fun setName(context: Context, name: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit {
            putString(PREF_NAME, Team.cleanName(name))
        }
    }

    private fun defaultName(context: Context): String = Team.cleanName(
        runCatching { Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) }
            .getOrNull()?.takeIf { it.isNotBlank() } ?: android.os.Build.MODEL,
    )
}
