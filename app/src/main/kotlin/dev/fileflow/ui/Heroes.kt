package dev.fileflow.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.view.View
import android.view.animation.LinearInterpolator
import androidx.core.graphics.ColorUtils
import dev.fileflow.ui.Ui.themeColor
import kotlin.math.PI
import kotlin.math.floor
import kotlin.math.sin

/**
 * The picture at the top of an action page, drawn rather than an icon: what FileFlow does, shown.
 * Animates only while on screen, and holds still when animations are off.
 */
abstract class HeroView(context: Context) : View(context) {
    protected val density = resources.displayMetrics.density
    protected val primary = context.themeColor(androidx.appcompat.R.attr.colorPrimary)
    protected val tertiary = context.themeColor(com.google.android.material.R.attr.colorTertiary)
    protected val secondary = context.themeColor(com.google.android.material.R.attr.colorSecondary)
    protected val surfaceHigh = context.themeColor(com.google.android.material.R.attr.colorSurfaceContainerHigh)
    protected val onSurface = context.themeColor(com.google.android.material.R.attr.colorOnSurface)

    /** Seconds since the animation started; frozen at a pleasing moment when motion is off. */
    protected var t = 1.3f
        private set

    private val ticker = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1000L
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        var started = 0L
        addUpdateListener {
            val now = System.nanoTime()
            if (started == 0L) started = now - (t * 1e9).toLong()
            t = ((now - started) / 1e9).toFloat()
            invalidate()
        }
    }

    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    /** The largest it grows; on a short screen it shrinks to what is left, rather than pushing. */
    var maxSidePx = (220 * density).toInt()

    /** Below this it is not drawn at all: a postage stamp of it looks like a fault, not a picture. */
    private val minSidePx = (112 * density).toInt()

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        fun avail(spec: Int) =
            if (MeasureSpec.getMode(spec) == MeasureSpec.UNSPECIFIED) Int.MAX_VALUE else MeasureSpec.getSize(spec)
        val side = minOf(maxSidePx, avail(widthSpec), avail(heightSpec)).let { if (it < minSidePx) 0 else it }
        setMeasuredDimension(side, side)
    }

    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        if (isVisible && Motion.enabled) {
            if (!ticker.isStarted) ticker.start()
        } else {
            ticker.cancel()
        }
    }

    override fun onDetachedFromWindow() {
        ticker.cancel()
        super.onDetachedFromWindow()
    }

    private var glowColour = 0

    /** A soft light behind everything, breathing slowly. Its gradient is made once per size. */
    protected fun drawGlow(c: Canvas, colour: Int) {
        val r = minOf(width, height) / 2f
        if (glowPaint.shader == null || glowColour != colour) {
            glowColour = colour
            glowPaint.shader = RadialGradient(width / 2f, height / 2f, r,
                intArrayOf(ColorUtils.setAlphaComponent(colour, 90), ColorUtils.setAlphaComponent(colour, 0)),
                null, Shader.TileMode.CLAMP)
        }
        val breathe = 0.92f + 0.06f * sin(t * 2 * PI.toFloat() / 4f)
        c.save()
        c.scale(breathe, breathe, width / 2f, height / 2f)
        c.drawCircle(width / 2f, height / 2f, r, glowPaint)
        c.restore()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        glowPaint.shader = null
    }

    /** A number in [0, 1) for each (cell, step): stable, so a cell's colours don't jitter. */
    protected fun hash(a: Int, b: Int): Float {
        var h = a * 374761393 + b * 668265263
        h = (h xor (h ushr 13)) * 1274126177
        return ((h xor (h ushr 16)) and 0xFFFF) / 65536f
    }

    protected fun smooth(x: Float): Float = x * x * (3 - 2 * x)
}

/**
 * Send: a frame of light -- a small grid of cells, each easing from colour to colour on its own
 * slow beat, inside a tile with the real frame's corner markers.
 */
class LightGridView(context: Context) : HeroView(context) {
    private val cells = 6
    private val palette = intArrayOf(primary, tertiary, secondary, onSurface,
        ColorUtils.setAlphaComponent(primary, 70), ColorUtils.setAlphaComponent(onSurface, 25))
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val tile = RectF()
    private val cell = RectF()

    override fun onDraw(c: Canvas) {
        if (width < 2 || height < 2) return  // hidden for want of room (see onMeasure)
        drawGlow(c, primary)
        val side = minOf(width, height) * 0.66f
        tile.set((width - side) / 2, (height - side) / 2, (width + side) / 2, (height + side) / 2)
        paint.style = Paint.Style.FILL
        paint.color = surfaceHigh
        c.drawRoundRect(tile, side * 0.16f, side * 0.16f, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = density
        paint.color = ColorUtils.setAlphaComponent(primary, 110)
        c.drawRoundRect(tile, side * 0.16f, side * 0.16f, paint)

        val pad = side * 0.13f
        val pitch = (side - 2 * pad) / cells
        val gap = pitch * 0.14f
        paint.style = Paint.Style.FILL
        for (y in 0 until cells) for (x in 0 until cells) {
            val corner = (x < 2 && y < 2) || (x >= cells - 2 && y < 2) || (x < 2 && y >= cells - 2)
            cell.set(tile.left + pad + x * pitch + gap, tile.top + pad + y * pitch + gap,
                     tile.left + pad + (x + 1) * pitch - gap, tile.top + pad + (y + 1) * pitch - gap)
            if (corner) continue
            val id = y * cells + x
            val period = 0.9f + 1.4f * hash(id, -1)
            val phase = t / period + hash(id, -2) * 7f
            val step = floor(phase).toInt()
            val f = phase - step
            val from = palette[(hash(id, step) * palette.size).toInt()]
            val to = palette[(hash(id, step + 1) * palette.size).toInt()]
            paint.color = ColorUtils.blendARGB(from, to, smooth(((f - 0.7f) / 0.3f).coerceIn(0f, 1f)))
            c.drawRoundRect(cell, pitch * 0.22f, pitch * 0.22f, paint)
        }
        // The three corner markers, as on a real frame: a ring with a dot.
        val m = 2 * pitch - 2 * gap
        for ((cx, cy) in listOf(0 to 0, cells - 2 to 0, 0 to cells - 2)) {
            val l = tile.left + pad + cx * pitch + gap
            val tp = tile.top + pad + cy * pitch + gap
            cell.set(l, tp, l + m, tp + m)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = m * 0.16f
            paint.color = onSurface
            cell.inset(paint.strokeWidth / 2, paint.strokeWidth / 2)
            c.drawRoundRect(cell, m * 0.22f, m * 0.22f, paint)
            paint.style = Paint.Style.FILL
            cell.inset(m * 0.2f, m * 0.2f)
            c.drawRoundRect(cell, m * 0.1f, m * 0.1f, paint)
        }
    }
}

/**
 * Receive: a viewfinder, its corners bracketing a faint frame, and a beam of light sweeping down
 * and up it -- lighting the cells it passes, as the camera reads them.
 */
class ScanView(context: Context) : HeroView(context) {
    private val cells = 7
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val box = RectF()
    private val cell = RectF()
    private var beamShader: LinearGradient? = null
    private val beamMatrix = android.graphics.Matrix()
    private val corners = android.graphics.Path()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        beamShader = null
        corners.reset()
    }

    override fun onDraw(c: Canvas) {
        if (width < 2 || height < 2) return  // hidden for want of room (see onMeasure)
        drawGlow(c, tertiary)
        val side = minOf(width, height) * 0.62f
        box.set((width - side) / 2, (height - side) / 2, (width + side) / 2, (height + side) / 2)

        // The beam: down and back, eased at each end, a moment's rest at the top.
        val cycle = 3.2f
        val p = (t % cycle) / cycle
        val beam = when {
            p < 0.45f -> smooth(p / 0.45f)
            p < 0.9f -> 1 - smooth((p - 0.45f) / 0.45f)
            else -> 0f
        }
        val beamY = box.top + side * (0.06f + 0.88f * beam)

        // The frame under the glass: each cell lit as the beam nears it.
        val pad = side * 0.12f
        val pitch = (side - 2 * pad) / cells
        val gap = pitch * 0.16f
        paint.style = Paint.Style.FILL
        for (y in 0 until cells) for (x in 0 until cells) {
            cell.set(box.left + pad + x * pitch + gap, box.top + pad + y * pitch + gap,
                     box.left + pad + (x + 1) * pitch - gap, box.top + pad + (y + 1) * pitch - gap)
            val near = (1 - kotlin.math.abs(cell.centerY() - beamY) / (side * 0.22f)).coerceIn(0f, 1f)
            val on = hash(y * cells + x, 3) > 0.45f
            val base = if (on) ColorUtils.setAlphaComponent(onSurface, 34) else ColorUtils.setAlphaComponent(onSurface, 12)
            paint.color = ColorUtils.blendARGB(base, if (on) tertiary else ColorUtils.setAlphaComponent(tertiary, 60), smooth(near))
            c.drawRoundRect(cell, pitch * 0.2f, pitch * 0.2f, paint)
        }

        // The beam itself: a soft band and a bright line. One gradient, moved with the beam.
        val band = side * 0.16f
        val shader = beamShader ?: LinearGradient(0f, -band, 0f, band,
            intArrayOf(Color.TRANSPARENT, ColorUtils.setAlphaComponent(tertiary, 70), Color.TRANSPARENT),
            null, Shader.TileMode.CLAMP).also { beamShader = it }
        beamMatrix.setTranslate(0f, beamY)
        shader.setLocalMatrix(beamMatrix)
        paint.shader = shader
        c.drawRect(box.left + pad * 0.4f, beamY - band, box.right - pad * 0.4f, beamY + band, paint)
        paint.shader = null
        paint.color = tertiary
        c.drawRoundRect(box.left + pad * 0.4f, beamY - density, box.right - pad * 0.4f, beamY + density,
                        density, density, paint)

        // The viewfinder's corners.
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 4 * density
        paint.strokeCap = Paint.Cap.ROUND
        paint.color = onSurface
        if (corners.isEmpty) {
            val arm = side * 0.2f
            val r = side * 0.08f
            for ((sx, sy) in listOf(-1 to -1, 1 to -1, -1 to 1, 1 to 1)) {
                val cx = if (sx < 0) box.left else box.right
                val cy = if (sy < 0) box.top else box.bottom
                corners.moveTo(cx, cy - sy * arm)
                corners.lineTo(cx, cy - sy * r)
                corners.quadTo(cx, cy, cx - sx * r, cy)
                corners.lineTo(cx - sx * arm, cy)
            }
        }
        c.drawPath(corners, paint)
        paint.strokeCap = Paint.Cap.BUTT
    }
}
