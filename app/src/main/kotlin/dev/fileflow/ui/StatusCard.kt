package dev.fileflow.ui

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.graphics.toColorInt
import com.google.android.material.card.MaterialCardView
import com.google.android.material.progressindicator.LinearProgressIndicator
import dev.fileflow.ui.Ui.dp

/**
 * The one thing the receive screen says: a sentence and a progress bar, in a floating card over
 * the camera picture.
 *
 * A real view rather than text painted onto the overlay canvas, so it wraps, scales with the
 * system font size and reads to accessibility services -- the canvas version did none of those.
 */
class StatusCard(context: Context) : MaterialCardView(context) {

    private val message = TextView(context).apply {
        textSize = 17f
        setTextColor(Color.WHITE)
        maxLines = 3
        setLineSpacing(0f, 1.1f)
    }
    private val percent = TextView(context).apply {
        textSize = 15f
        setTextColor(Color.WHITE)
        gravity = Gravity.END
        typeface = android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL)
    }
    /** A quiet second line under the bar: speed and elapsed time, once there is something to say. */
    private val detail = TextView(context).apply {
        textSize = 13f
        setTextColor("#B3FFFFFF".toColorInt())
        visibility = GONE
    }
    private val bar = LinearProgressIndicator(context).apply {
        max = 1000
        trackCornerRadius = context.dp(3)
        trackThickness = context.dp(6)
        trackColor = "#33FFFFFF".toColorInt()
        trackStopIndicatorSize = 0  // the M3 end dot read as a stray orange speck on an empty bar
    }

    init {
        radius = context.dp(24).toFloat()
        cardElevation = 0f
        strokeWidth = 0
        setCardBackgroundColor("#CC101418".toColorInt())
        contentDescription = "Transfer status"
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(context.dp(18), context.dp(14), context.dp(18), context.dp(16))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(message, LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
                addView(percent, LinearLayout.LayoutParams(
                    LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT,
                ).apply { marginStart = context.dp(12) })
            })
            addView(bar, LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = context.dp(12) })
            addView(detail, LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = context.dp(8) })
        })
        // TalkBack reads each new status without the person having to find the card.
        message.accessibilityLiveRegion = ACCESSIBILITY_LIVE_REGION_POLITE
    }

    /** The second line; null hides it. */
    fun setDetail(text: String?) {
        if (detail.text.toString() != (text ?: "")) detail.text = text ?: ""
        detail.visibility = if (text.isNullOrEmpty()) GONE else VISIBLE
    }

    /**
     * [progress] in 0..1; below 0 hides the percentage and leaves the bar empty.
     *
     * Called for every decoded frame, so only what CHANGED is touched: re-setting the same text
     * still lays the card out again, and restarting the bar's animation 50 times a second keeps it
     * from ever finishing one, which reads as a stutter instead of a glide.
     */
    fun set(text: String, progress: Float, colour: Int) {
        val p = progress.coerceIn(0f, 1f)
        val permille = (p * 1000).toInt()
        if (text != shownText || permille != shownPermille) {
            message.text = text
            percent.text = if (progress > 0f) "${(p * 100).toInt()}%" else ""
            message.contentDescription =
                text + if (progress > 0f) ", ${(p * 100).toInt()} percent" else ""
            shownText = text
        }
        if (colour != shownColour) {
            bar.setIndicatorColor(colour)
            shownColour = colour
        }
        if (permille != shownPermille) {
            bar.setProgressCompat(permille, true)
            shownPermille = permille
        }
    }

    private var shownText: String? = null
    private var shownPermille = -1
    private var shownColour = 0
}
