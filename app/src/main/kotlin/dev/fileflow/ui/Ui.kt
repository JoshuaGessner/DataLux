package dev.fileflow.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.annotation.AttrRes
import androidx.annotation.DrawableRes
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * The few building blocks every screen uses, so the screens read as one app: Material 3 cards,
 * buttons and type, sized in dp, coloured from the theme (and so from the wallpaper where the phone
 * offers dynamic colour).
 *
 * Built in code rather than XML layouts because every screen in this app already is, and two ways
 * of building a screen would be one too many.
 */
object Ui {

    fun Context.dp(v: Number): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics)
            .toInt()

    fun Context.themeColor(@AttrRes attr: Int): Int {
        val tv = TypedValue()
        theme.resolveAttribute(attr, tv, true)
        return tv.data
    }

    fun Context.text(
        s: CharSequence,
        sizeSp: Float = 15f,
        @AttrRes colour: Int = com.google.android.material.R.attr.colorOnSurfaceVariant,
        bold: Boolean = false,
    ) = TextView(this).apply {
        text = s
        textSize = sizeSp
        setTextColor(themeColor(colour))
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        setLineSpacing(0f, 1.15f)
    }

    /** A heading. Large ones are set a little tighter, as display type is. */
    fun Context.title(s: CharSequence, sizeSp: Float = 22f) =
        text(s, sizeSp, com.google.android.material.R.attr.colorOnSurface, bold = true).apply {
            if (sizeSp >= 24f) letterSpacing = -0.015f
            setLineSpacing(0f, 1.05f)
        }

    /** A rounded, tinted surface holding [content] vertically. */
    fun Context.card(vararg content: View): MaterialCardView = MaterialCardView(this).apply {
        radius = dp(28).toFloat()
        cardElevation = 0f
        strokeWidth = 0
        setCardBackgroundColor(themeColor(com.google.android.material.R.attr.colorSurfaceContainer))
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(20))
            content.forEach { addView(it) }
        })
    }

    /** Icon in a soft circle, beside a title: the head of a card. */
    fun Context.cardHeader(@DrawableRes icon: Int, title: String, trailing: View? = null) =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(ImageView(context).apply {
                setImageResource(icon)
                imageTintList = ColorStateList.valueOf(
                    themeColor(com.google.android.material.R.attr.colorOnPrimaryContainer),
                )
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(themeColor(com.google.android.material.R.attr.colorPrimaryContainer))
                }
                val p = dp(10)
                setPadding(p, p, p, p)
            }, LinearLayout.LayoutParams(dp(44), dp(44)))
            addView(title(title).apply { setPadding(dp(14), 0, 0, 0) },
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            if (trailing != null) addView(trailing)
        }

    enum class Style { Filled, Tonal, Text }

    fun Context.button(
        label: String,
        @DrawableRes icon: Int? = null,
        style: Style = Style.Filled,
        onClick: () -> Unit,
    ): MaterialButton {
        val attr = when (style) {
            Style.Filled -> com.google.android.material.R.attr.materialButtonStyle
            Style.Tonal -> com.google.android.material.R.attr.materialButtonOutlinedStyle
            Style.Text -> androidx.appcompat.R.attr.borderlessButtonStyle
        }
        return MaterialButton(this, null, attr).apply {
            text = label
            isAllCaps = false
            textSize = 16f
            minHeight = dp(52)
            cornerRadius = dp(26)
            if (icon != null) {
                setIconResource(icon)
                iconGravity = MaterialButton.ICON_GRAVITY_TEXT_START
                iconPadding = dp(8)
            }
            if (style == Style.Tonal) {
                // A filled tonal button: outlined style for the shape, a container fill for weight.
                strokeWidth = 0
                backgroundTintList = ColorStateList.valueOf(
                    themeColor(com.google.android.material.R.attr.colorSecondaryContainer),
                )
                val on = themeColor(com.google.android.material.R.attr.colorOnSecondaryContainer)
                setTextColor(on)
                iconTint = ColorStateList.valueOf(on)
            }
            setOnClickListener { onClick() }
            Motion.pressable(this)
        }
    }

    /**
     * The small "?" that opens an explanation. Detail lives behind it, so the screen itself can
     * say only what is needed to act.
     */
    fun Context.helpButton(title: String, body: CharSequence): ImageView = ImageView(this).apply {
        setImageResource(dev.fileflow.R.drawable.ic_help)
        imageTintList = ColorStateList.valueOf(
            themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant),
        )
        contentDescription = "About: $title"
        val p = dp(10)
        setPadding(p, p, p, p)
        layoutParams = LinearLayout.LayoutParams(dp(44), dp(44))
        val ripple = TypedValue()
        context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, ripple, true)
        setBackgroundResource(ripple.resourceId)
        setOnClickListener { showHelp(title, body) }
    }

    fun Context.showHelp(title: String, body: CharSequence) {
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(body)
            .setPositiveButton("Got it", null)
            .show()
    }

    fun Context.space(hDp: Int) = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(hDp))
    }

    /**
     * Holds its one child to at most [maxPx] wide, centred: on a tablet or a phone on its side, a
     * column of text and buttons stays a readable width instead of stretching edge to edge.
     */
    class Capped(context: Context, private val maxPx: Int) : android.widget.FrameLayout(context) {
        override fun onMeasure(widthSpec: Int, heightSpec: Int) {
            val size = MeasureSpec.getSize(widthSpec)
            val inner = if (MeasureSpec.getMode(widthSpec) != MeasureSpec.UNSPECIFIED && size > maxPx) maxPx else size
            super.onMeasure(MeasureSpec.makeMeasureSpec(inner, MeasureSpec.EXACTLY), heightSpec)
            setMeasuredDimension(maxOf(size, inner), measuredHeight)
        }
    }

    /** [content] capped to a readable width ([Capped]) and centred, filling the height it is given. */
    fun Context.capped(content: View, maxDp: Int = 600): View = Capped(this, dp(maxDp)).apply {
        addView(content, android.widget.FrameLayout.LayoutParams(
            android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
            android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
            Gravity.CENTER_HORIZONTAL,
        ))
    }

    /**
     * What a page shows when it has nothing to list: an icon on a soft lit disc, a line saying what
     * will be here, and -- when there is one -- the button that puts it there.
     */
    fun Context.emptyState(@DrawableRes icon: Int, headline: String, body: String, action: View? = null): View =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(24), dp(40), dp(24), dp(24))
            val glow = themeColor(androidx.appcompat.R.attr.colorPrimary)
            addView(ImageView(context).apply {
                setImageResource(icon)
                imageTintList = ColorStateList.valueOf(glow)
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    gradientType = GradientDrawable.RADIAL_GRADIENT
                    gradientRadius = dp(56).toFloat()
                    colors = intArrayOf(
                        androidx.core.graphics.ColorUtils.setAlphaComponent(glow, 70),
                        androidx.core.graphics.ColorUtils.setAlphaComponent(glow, 12),
                    )
                }
                val p = dp(28)
                setPadding(p, p, p, p)
            }, LinearLayout.LayoutParams(dp(112), dp(112)))
            addView(title(headline, 20f).apply { gravity = Gravity.CENTER; setPadding(0, dp(20), 0, dp(6)) })
            addView(text(body).apply { gravity = Gravity.CENTER })
            if (action != null) addView(action, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(20) })
        }

    /** A person, as a disc with their initials, coloured by their name -- the same colour everywhere. */
    fun Context.avatar(name: String, sizeDp: Int = 40): TextView = TextView(this).apply {
        val initials = name.split(' ', '-', '_').filter { it.isNotBlank() }.take(2)
            .joinToString("") { it.first().uppercase() }.ifEmpty { "?" }
        text = initials
        gravity = Gravity.CENTER
        textSize = sizeDp * 0.38f
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        val hues = intArrayOf(
            com.google.android.material.R.attr.colorPrimaryContainer,
            com.google.android.material.R.attr.colorSecondaryContainer,
            com.google.android.material.R.attr.colorTertiaryContainer,
        )
        val ons = intArrayOf(
            com.google.android.material.R.attr.colorOnPrimaryContainer,
            com.google.android.material.R.attr.colorOnSecondaryContainer,
            com.google.android.material.R.attr.colorOnTertiaryContainer,
        )
        val i = Math.floorMod(name.hashCode(), hues.size)
        setTextColor(themeColor(ons[i]))
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(themeColor(hues[i]))
        }
        layoutParams = LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp))
    }

    fun matchWidth(topMarginPx: Int = 0) =
        LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                  LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = topMarginPx }
}
