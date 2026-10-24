package dev.fileflow

import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * Keeps content clear of the status bar, navigation bar and camera cutout.
 *
 * Apps targeting API 35 are drawn edge to edge whether they ask or not, so without this the title
 * sat under the status bar and the last lines of every screen under the navigation bar -- and on
 * the receive screen the first line of the live status, the one sentence that matters, was under
 * the camera cutout.
 */
val SYSTEM_INSETS: Int = WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()

/** Pads this view by the system insets on top of the padding it already has. */
fun <T : View> T.padForSystemBars(): T {
    val l = paddingLeft
    val t = paddingTop
    val r = paddingRight
    val b = paddingBottom
    ViewCompat.setOnApplyWindowInsetsListener(this) { v, insets ->
        val i = insets.getInsets(SYSTEM_INSETS)
        v.setPadding(l + i.left, t + i.top, r + i.right, b + i.bottom)
        insets
    }
    return this
}
