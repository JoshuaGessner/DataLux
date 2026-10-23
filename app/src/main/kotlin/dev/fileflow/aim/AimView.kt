package dev.fileflow.aim

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

/**
 * Draws the aiming analysis REGISTERED ONTO the live camera preview.
 *
 * ## One picture, not two
 *
 * This view is a child of the same letterboxed box as the preview `SurfaceView`, so its canvas is
 * *exactly* the preview rectangle. The detected screen outline is therefore drawn on top of the
 * thing it describes, at 1:1.
 *
 * It did not start that way, and the failure is worth keeping. The overlay used to draw its own
 * miniature "frame" schematic in the upper part of the screen while the preview sat letterboxed and
 * centred — two boxes, different sizes, different places, describing the same camera. An operator
 * reported it as exactly that: the camera view in the middle and a separate white box snapped to the
 * top that "don't line up" (F38). A diagram beside an image is not an overlay; it is a second thing
 * to reconcile, and reconciling it is work the operator should never have been given.
 *
 * ## Analysis over image, not instead of it
 *
 * The overlay still earns its place, because the facts that decide success are ones a live image
 * hides: six consecutive hardware failures were framing or exposure, and an overexposed screen just
 * looks like a bright screen to the eye (F33, F34). So the image shows *where* the camera points and
 * the overlay shows *what is wrong with it*.
 *
 * ## Display space, not sensor space
 *
 * A rear camera's sensor is mounted rotated (`SENSOR_ORIENTATION` is 90 on both reference devices),
 * so a portrait-held phone receives landscape frames. Every camera coordinate is rotated by
 * [sensorRotation] before being drawn; without that, moving the phone left moved the box up.
 */
class AimView(context: Context, private var cols: Int, private var rows: Int) : View(context) {

    /**
     * Clockwise degrees to turn a sensor frame into display orientation. Set from
     * `SENSOR_ORIENTATION`; 0 leaves coordinates in raw sensor space.
     */
    var sensorRotation: Int = 0
        set(v) { field = ((v % 360) + 360) % 360; postInvalidate() }

    /**
     * The rectangle the PREVIEW's image actually occupies within this view.
     *
     * The preview fits its image and letterboxes the remainder, so the image is generally smaller
     * than the view. Drawing the detected outline against the full view instead of against this
     * rectangle would put it beside the thing it describes — the misregistration that made an
     * earlier build show two boxes in different places (F38).
     */
    var contentRect: android.graphics.RectF? = null
        set(v) { field = v; postInvalidate() }

    private var aim: Aim? = null
    private var frameW = 0
    private var frameH = 0
    private var message: String? = "Starting camera…"
    private var banner: String? = null

    private val coverage = Paint().apply { style = Paint.Style.FILL }

    // Taken from native rather than duplicated, so the map's "unreadable" and the decoder's erasure
    // floor cannot drift apart and tell the operator two different stories.
    private val usableSep = NativeAim.tileUsableSeparation()

    private val screenPaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 6f
        isAntiAlias = true
    }
    private val edgePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 16f
        color = Color.parseColor("#FF5252")
        isAntiAlias = true
    }
    private val textScrim = Paint().apply {
        style = Paint.Style.FILL
        color = Color.parseColor("#B0000000")
    }
    private val bigText = Paint().apply {
        color = Color.WHITE
        textSize = 46f
        isAntiAlias = true
        isFakeBoldText = true
    }
    private val bannerText = Paint().apply {
        color = Color.parseColor("#80CBC4")
        textSize = 38f
        isAntiAlias = true
        isFakeBoldText = true
    }
    private val smallText = Paint().apply {
        color = Color.parseColor("#CFD8DC")
        textSize = 29f
        isAntiAlias = true
    }

    fun setGrid(c: Int, r: Int) {
        cols = c
        rows = r
    }

    fun setMessage(m: String) {
        message = m
        aim = null
        postInvalidate()
    }

    /** A line above the guidance, for flow state such as "recording" or "hold still to start". */
    /**
     * Paint the coverage map over the image, one block per tile.
     *
     * Colour is the whole point. A number tells an operator that something is wrong; a red patch
     * tells them WHERE, and where is the only part they can act on by moving the phone. This is the
     * measurement that found the blocker offline (F47), put where it can be acted on while the
     * phones are still in someone's hands.
     *
     * Built on the same `rotatePoint` + scale mapping the bounding box uses. An earlier version
     * derived its own placement from the bbox fractions and would have marked the wrong region at
     * 90 and 270 -- a coverage map pointing at the wrong part of the screen is worse than none,
     * because it sends the operator to adjust something that was already fine.
     */
    private fun drawCoverage(canvas: Canvas, a: Aim, ox: Float, oy: Float, scale: Float) {
        val cols = a.tileCols
        if (cols <= 0 || a.tiles.isEmpty() || a.bboxW <= 0 || a.bboxH <= 0) return
        val rows = a.tiles.size / cols
        if (rows <= 0) return

        for (i in a.tiles.indices) {
            val tx = i % cols
            val ty = i / cols
            // Tile bounds in SENSOR pixels, from the bounding box the analyser reported.
            val sx0 = a.bboxX + a.bboxW * tx / cols
            val sx1 = a.bboxX + a.bboxW * (tx + 1) / cols
            val sy0 = a.bboxY + a.bboxH * ty / rows
            val sy1 = a.bboxY + a.bboxH * (ty + 1) / rows
            val (dx0, dy0) = rotatePoint(sx0, sy0)
            val (dx1, dy1) = rotatePoint(sx1, sy1)
            val left = ox + minOf(dx0, dx1) * scale
            val top = oy + minOf(dy0, dy1) * scale
            val right = ox + maxOf(dx0, dx1) * scale
            val bottom = oy + maxOf(dy0, dy1) * scale

            val sep = a.tiles[i]
            coverage.color = when {
                sep <= usableSep -> 0x70FF1744.toInt()      // unreadable: the decoder will erase it
                sep <= usableSep * 3 -> 0x55FFC400.toInt()  // marginal
                else -> 0x2200E676                          // healthy, kept faint
            }
            // Inset, so the blocks read as a grid and the image stays visible between them.
            canvas.drawRect(left + 1.5f, top + 1.5f, right - 1.5f, bottom - 1.5f, coverage)
        }
    }

    fun setBanner(b: String?) {
        banner = b
        postInvalidate()
    }

    private var live: String? = null
    private var liveProgress = -1f
    private var liveColour = Color.parseColor("#69F0AE")
    private val liveTrack = Paint().apply { color = Color.parseColor("#40FFFFFF") }
    private val liveFill = Paint()

    /**
     * The live receiver's status, as a card at the TOP with a progress bar (C13b).
     *
     * Top, not bottom, because it answers the question the person is actually asking — "is it
     * working, and how long" — and the bottom block is already the aiming evidence. The sentence
     * comes from native unchanged; deciding what to say is not this view's job (ADR-0014).
     * [progress] < 0 hides the bar; [colour] is the bar and accent colour.
     */
    fun setLive(message: String?, progress: Float, colour: Int) {
        live = message
        liveProgress = progress
        liveColour = colour
        postInvalidate()
    }

    private fun drawLive(canvas: Canvas) {
        val msg = live ?: return
        val pad = 28f
        val lineH = 56f
        val lines = wrap(msg, 30)
        val barH = if (liveProgress >= 0f) 22f + pad else 0f
        val blockH = pad * 2 + lines.size * lineH + barH
        canvas.drawRect(0f, 0f, width.toFloat(), blockH, textScrim)
        var ty = pad + 42f
        for (line in lines) {
            canvas.drawText(line, pad, ty, bigText)
            ty += lineH
        }
        if (liveProgress >= 0f) {
            val top = ty - 20f
            val right = width - pad
            canvas.drawRect(pad, top, right, top + 22f, liveTrack)
            liveFill.color = liveColour
            canvas.drawRect(pad, top, pad + (right - pad) * liveProgress.coerceIn(0f, 1f),
                            top + 22f, liveFill)
        }
    }

    fun update(a: Aim, w: Int, h: Int) {
        aim = a
        frameW = w
        frameH = h
        message = null
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        // NO scrim over the image. This canvas is the preview itself now, and dimming the whole of
        // it would dim the thing being aimed. Text gets a local scrim behind it instead, so it stays
        // legible over a bright screen without obscuring the rest.
        val a = aim
        if (a == null) {
            drawTextBlock(canvas, listOf(message ?: ""), emptyList())
            drawLive(canvas)
            return
        }

        // The canvas IS the frame: this view is a child of the letterboxed preview box, so the
        // mapping from camera pixels to canvas pixels is a single uniform scale with no offset.
        val quarter = sensorRotation == 90 || sensorRotation == 270
        val dispW = (if (quarter) frameH else frameW).coerceAtLeast(1)
        val dispH = (if (quarter) frameW else frameH).coerceAtLeast(1)
        // Register to the PREVIEW's fitted rectangle when it is known, falling back to this view's
        // own bounds when it is not.
        val cr = contentRect
        val areaW = cr?.width() ?: width.toFloat()
        val areaH = cr?.height() ?: height.toFloat()
        val scale = minOf(areaW / dispW, areaH / dispH)
        val ox = (cr?.left ?: 0f) + (areaW - dispW * scale) / 2f
        val oy = (cr?.top ?: 0f) + (areaH - dispH * scale) / 2f

        val colour = when (a.verdict) {
            AimVerdict.Ready -> Color.parseColor("#69F0AE")
            AimVerdict.Clipped -> Color.parseColor("#FF5252")
            AimVerdict.TooFar, AimVerdict.NoScreenFound -> Color.parseColor("#90A4AE")
            AimVerdict.TooBright, AimVerdict.TooDark -> Color.parseColor("#FFB74D")
            AimVerdict.Blurred -> Color.parseColor("#CE93D8")
            AimVerdict.Unknown -> Color.GRAY
        }
        screenPaint.color = colour

        // Under the outline, so the box edges stay readable over it.

        drawCoverage(canvas, a, ox, oy, scale)


        if (a.bboxW > 0 && a.bboxH > 0) {
            // Rotate the box's corners into display space, then take their extent. Rotating the
            // rectangle rather than just its origin matters: at 90 and 270 its width and height
            // swap, and an unswapped box over swapped frame dimensions looks stretched.
            val (bx0, by0) = rotatePoint(a.bboxX, a.bboxY)
            val (bx1, by1) = rotatePoint(a.bboxX + a.bboxW, a.bboxY + a.bboxH)
            // OUTLINE, not a filled block: a filled rectangle would cover the screen image the
            // operator is trying to judge.
            canvas.drawRect(
                ox + minOf(bx0, bx1) * scale, oy + minOf(by0, by1) * scale,
                ox + maxOf(bx0, bx1) * scale, oy + maxOf(by0, by1) * scale,
                screenPaint,
            )
        }

        // Which SENSOR edge is clipped is not which DISPLAYED edge is clipped. Highlighting the
        // wrong side would send the operator the wrong way — the exact failure this class exists to
        // prevent.
        val top = when (sensorRotation) {
            90 -> a.clippedLeft; 180 -> a.clippedBottom; 270 -> a.clippedRight
            else -> a.clippedTop
        }
        val bottom = when (sensorRotation) {
            90 -> a.clippedRight; 180 -> a.clippedTop; 270 -> a.clippedLeft
            else -> a.clippedBottom
        }
        val left = when (sensorRotation) {
            90 -> a.clippedBottom; 180 -> a.clippedRight; 270 -> a.clippedTop
            else -> a.clippedLeft
        }
        val right = when (sensorRotation) {
            90 -> a.clippedTop; 180 -> a.clippedLeft; 270 -> a.clippedBottom
            else -> a.clippedRight
        }
        // On the IMAGE's edges, not the view's: with letterbox bars present those are different
        // lines, and marking the wrong one points at empty space.
        val l = ox
        val t = oy
        val rr = ox + dispW * scale
        val bb = oy + dispH * scale
        if (top) canvas.drawLine(l, t, rr, t, edgePaint)
        if (bottom) canvas.drawLine(l, bb, rr, bb, edgePaint)
        if (left) canvas.drawLine(l, t, l, bb, edgePaint)
        if (right) canvas.drawLine(rr, t, rr, bb, edgePaint)

        val facts = listOf(
            "px/cell %.1f    rotation %.0f°    screen %.0f%%".format(
                a.pxPerCell, a.rotationDeg, a.litFraction * 100.0),
            "brightness %.0f    blur %.2f    grid ${cols}x$rows".format(
                a.meanLuminance, a.midFraction),
        )
        drawTextBlock(canvas, wrap(a.guidance, 30), facts)
        drawLive(canvas)
    }

    /**
     * Guidance and evidence, in a scrim at the BOTTOM.
     *
     * Bottom rather than top because the overlay now sits on the image: text at the top would cover
     * the part of the view an operator tilts up into, and the numbers are what let someone watch
     * px/cell rise as they move — which teaches the rig far faster than a coloured light does.
     */
    private fun drawTextBlock(canvas: Canvas, lines: List<String>, facts: List<String>) {
        val pad = 28f
        val lineH = 56f
        val factH = 38f
        // The banner line counts too. It used to be drawn without being measured, so with a banner
        // showing the last line of text was pushed 46 px off the bottom of the screen.
        val bannerH = if (banner != null) 46f else 0f
        val blockH = pad * 2 + bannerH + lines.size * lineH + facts.size * factH
        val top = height - blockH
        canvas.drawRect(0f, top, width.toFloat(), height.toFloat(), textScrim)

        var ty = top + pad + 42f
        banner?.let {
            canvas.drawText(it, pad, ty, bannerText)
            ty += 46f
        }
        for (line in lines) {
            canvas.drawText(line, pad, ty, bigText)
            ty += lineH
        }
        for (f in facts) {
            canvas.drawText(f, pad, ty, smallText)
            ty += factH
        }
    }

    /** Sensor point -> display point. */
    private fun rotatePoint(x: Int, y: Int): Pair<Float, Float> {
        val fx = x.toFloat()
        val fy = y.toFloat()
        return when (sensorRotation) {
            90 -> Pair(frameH - fy, fx)
            180 -> Pair(frameW - fx, frameH - fy)
            270 -> Pair(fy, frameW - fx)
            else -> Pair(fx, fy)
        }
    }

    private fun wrap(s: String, width: Int): List<String> {
        val out = ArrayList<String>()
        var line = StringBuilder()
        for (word in s.split(' ')) {
            if (line.isNotEmpty() && line.length + 1 + word.length > width) {
                out.add(line.toString())
                line = StringBuilder()
            }
            if (line.isNotEmpty()) line.append(' ')
            line.append(word)
        }
        if (line.isNotEmpty()) out.add(line.toString())
        return out
    }
}
