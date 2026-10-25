package dev.fileflow.ui

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.View
import android.view.animation.PathInterpolator

/**
 * The app's motion, in one place, after Material 3's: emphasized easing, short durations, and
 * nothing at all when the person has turned animations off (Settings › Accessibility › Remove
 * animations) -- every helper here checks [enabled] and simply lands in the end state.
 */
object Motion {
    /** Material 3 "emphasized decelerate": things arriving. */
    val ENTER = PathInterpolator(0.05f, 0.7f, 0.1f, 1f)
    /** Material 3 "emphasized accelerate": things leaving. */
    val EXIT = PathInterpolator(0.3f, 0f, 0.8f, 0.15f)
    /** Material 3 "standard": things changing in place. */
    val STANDARD = PathInterpolator(0.2f, 0f, 0f, 1f)

    const val SHORT = 150L
    const val MEDIUM = 300L
    const val LONG = 450L

    /** False when the system's animator duration scale is 0: no motion, end states only. */
    val enabled: Boolean get() = ValueAnimator.areAnimatorsEnabled()

    /**
     * [views] rise into place one after another -- the order the eye should read them. Each starts
     * a little below and transparent, and settles over [LONG] with [ENTER] easing.
     */
    fun staggerIn(views: List<View>, stepMs: Long = 45L, risePx: Float = 48f) {
        views.forEachIndexed { i, v ->
            v.animate().cancel()
            if (!enabled) {
                v.alpha = 1f
                v.translationY = 0f
                return@forEachIndexed
            }
            v.alpha = 0f
            v.translationY = risePx
            v.animate().alpha(1f).translationY(0f)
                .setStartDelay(i * stepMs).setDuration(LONG).setInterpolator(ENTER).start()
        }
    }

    /**
     * Material's "fade through", for pages that replace each other: the old one fades out fast, the
     * new one fades in growing from 92%.
     */
    fun fadeThrough(out: View?, into: View, onSwapped: () -> Unit = {}) {
        if (!enabled) {
            out?.visibility = View.GONE
            into.visibility = View.VISIBLE
            into.alpha = 1f
            onSwapped()
            return
        }
        val swap = {
            out?.visibility = View.GONE
            out?.alpha = 1f
            into.alpha = 0f
            into.scaleX = 0.92f
            into.scaleY = 0.92f
            into.visibility = View.VISIBLE
            onSwapped()
            into.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(210L).setInterpolator(ENTER).start()
        }
        if (out == null || out.visibility != View.VISIBLE) swap()
        else out.animate().alpha(0f).setDuration(90L).setInterpolator(EXIT).withEndAction(swap).start()
    }

    /**
     * Pressing [view] sinks it a little and letting go springs it back: the touch is felt before
     * the click does anything. Clicks, long-presses and ripples are untouched.
     */
    @SuppressLint("ClickableViewAccessibility")
    fun pressable(view: View, scale: Float = 0.96f) {
        view.setOnTouchListener { v, e ->
            if (!enabled) return@setOnTouchListener false
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN ->
                    v.animate().scaleX(scale).scaleY(scale).setDuration(SHORT).setInterpolator(STANDARD).start()
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                    v.animate().scaleX(1f).scaleY(1f).setDuration(MEDIUM)
                        .setInterpolator(android.view.animation.OvershootInterpolator(2.5f)).start()
            }
            false
        }
    }

    /** Shows or hides [view] by sliding it down from above and fading it, instead of a jump. */
    fun reveal(view: View, show: Boolean) {
        if (show == (view.visibility == View.VISIBLE) && view.alpha == 1f) return
        view.animate().cancel()
        if (!enabled) {
            view.visibility = if (show) View.VISIBLE else View.GONE
            view.alpha = 1f
            view.translationY = 0f
            return
        }
        if (show) {
            view.visibility = View.VISIBLE
            view.alpha = 0f
            view.translationY = -view.resources.displayMetrics.density * 12
            view.animate().alpha(1f).translationY(0f).setDuration(MEDIUM).setInterpolator(ENTER).start()
        } else {
            view.animate().alpha(0f).translationY(-view.resources.displayMetrics.density * 12)
                .setDuration(SHORT).setInterpolator(EXIT)
                .withEndAction { view.visibility = View.GONE; view.translationY = 0f }.start()
        }
    }
}
