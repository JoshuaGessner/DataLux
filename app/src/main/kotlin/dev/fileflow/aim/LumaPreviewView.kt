package dev.fileflow.aim

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.Log
import android.view.View
import androidx.core.graphics.createBitmap
import androidx.core.graphics.withTranslation
import java.nio.ByteBuffer

/**
 * A camera preview drawn from the luminance plane we already have, on a canvas we control.
 *
 * ## Why not a TextureView or a SurfaceView
 *
 * Five attempts to orient the preview through the platform's view classes failed, and the last
 * one failed while *reporting success*: the `TextureView` returned `identity=false` from
 * `getTransform` and mapped its content to `(0,240)-(1080,2160)` — rotated, letterboxed, exactly
 * right — while the composited screen showed an unrotated image filling all 2400 rows. The matrix
 * was held and not honoured. Whatever the reason, the operator saw a sideways, stretched preview
 * through every one of those attempts and said so every time (F38, F41, F42, F43, F44).
 *
 * The platform is not required for this. The receiver **already reads every frame's Y plane on the
 * CPU** for aim analysis, so the pixels are in hand before any of this starts. Copying them into a
 * `Bitmap` and drawing that through a `Canvas` matrix puts the rotation somewhere it cannot be
 * quietly dropped: there is no `SurfaceTexture`, no compositor transform hint and no producer
 * queue between the decision and the pixels.
 *
 * ## Why grayscale is not a compromise
 *
 * The link is luminance-only (M0). A grayscale preview shows the operator precisely what the
 * demodulator sees, which is *more* informative than a colour-corrected one — an image the ISP has
 * prettied up can look fine while the levels it is built from are unusable.
 *
 * ## Cost
 *
 * The Y plane is subsampled by an integer stride while copying, to about 720 px on the long edge,
 * and repainting is throttled. Nothing is interpolated and no frame is copied whole: at 2688×1512
 * that is a quarter-resolution strided read, which is cheaper than the aim analysis running beside
 * it.
 */
class LumaPreviewView(context: Context) : View(context) {

    private val paint = Paint().apply { isFilterBitmap = true }
    private var bitmap: Bitmap? = null
    private var pixels: IntArray = IntArray(0)
    private var bmpW = 0
    private var bmpH = 0
    private var rotationDeg = 0
    private val lock = Any()
    private var lastPaintMs = 0L
    private var logged = false

    /**
     * Hand over a camera frame's luminance plane. Safe to call from the camera thread.
     *
     * @param rotationDeg degrees CLOCKWISE to turn the frame to bring it upright. This is
     *   `SENSOR_ORIENTATION` and nothing else: every activity is locked to portrait, so there is no
     *   display-rotation term to subtract, and [AimView] rotates its schematic by the same value —
     *   one expression in both places means the overlay cannot drift off the image it annotates.
     */
    fun submit(buf: ByteBuffer, w: Int, h: Int, stride: Int, rotationDeg: Int) {
        if (w <= 0 || h <= 0) return
        val now = System.currentTimeMillis()
        // Aiming is a human feedback loop; it does not need every frame, and repainting at capture
        // rate would compete with the analysis that produces the guidance.
        if (now - lastPaintMs < PAINT_INTERVAL_MS) return
        lastPaintMs = now

        val step = maxOf(1, maxOf(w, h) / TARGET_LONG_EDGE)
        val dw = w / step
        val dh = h / step
        if (dw <= 0 || dh <= 0) return

        synchronized(lock) {
            if (bmpW != dw || bmpH != dh) {
                bmpW = dw
                bmpH = dh
                pixels = IntArray(dw * dh)
                bitmap = createBitmap(dw, dh)
                Log.i(TAG, "preview bitmap ${dw}x$dh from camera ${w}x$h (step $step)")
            }
            this.rotationDeg = ((rotationDeg % 360) + 360) % 360
            val px = pixels
            var i = 0
            for (y in 0 until dh) {
                var src = y * step * stride
                for (x in 0 until dw) {
                    // Absolute get: the buffer's position belongs to whoever else is reading it.
                    val v = buf.get(src).toInt() and 0xFF
                    px[i++] = -0x1000000 or (v shl 16) or (v shl 8) or v
                    src += step
                }
            }
            bitmap?.setPixels(px, 0, dw, 0, 0, dw, dh)
        }
        postInvalidate()
    }

    /**
     * The rectangle, in this view's coordinates, the image actually occupies.
     *
     * The overlay registers its guidance to this, so it must be derived from the same numbers
     * [onDraw] uses rather than recomputed from an assumption about them.
     */
    fun contentRect(): RectF {
        val vw = width.toFloat()
        val vh = height.toFloat()
        synchronized(lock) {
            if (bmpW <= 0 || bmpH <= 0 || vw <= 0f || vh <= 0f) return RectF(0f, 0f, vw, vh)
            val quarter = rotationDeg == 90 || rotationDeg == 270
            val dw = (if (quarter) bmpH else bmpW).toFloat()
            val dh = (if (quarter) bmpW else bmpH).toFloat()
            val s = minOf(vw / dw, vh / dh)
            val outW = dw * s
            val outH = dh * s
            val left = (vw - outW) / 2f
            val top = (vh - outH) / 2f
            return RectF(left, top, left + outW, top + outH)
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val vw = width.toFloat()
        val vh = height.toFloat()
        synchronized(lock) {
            val bmp = bitmap ?: return
            if (vw <= 0f || vh <= 0f) return

            val quarter = rotationDeg == 90 || rotationDeg == 270
            // Dimensions AFTER rotation — what the image will occupy on screen.
            val dw = (if (quarter) bmpH else bmpW).toFloat()
            val dh = (if (quarter) bmpW else bmpH).toFloat()
            // FIT, not fill. One uniform factor on both axes: separate factors are the stretch
            // this class exists to make impossible, and cropping would hide the frame running off
            // an edge, which is the single fact an operator most needs to see (F33).
            val s = minOf(vw / dw, vh / dh)

            canvas.withTranslation(vw / 2f, vh / 2f) {
                rotate(rotationDeg.toFloat())
                scale(s, s)
                drawBitmap(bmp, -bmpW / 2f, -bmpH / 2f, paint)
            }

            if (!logged) {
                logged = true
                Log.i(TAG, ("drawing %dx%d bitmap rotated %d -> %.0fx%.0f in view %.0fx%.0f " +
                            "(uniform scale %.3f, bars %.0f top/bottom)")
                    .format(bmpW, bmpH, rotationDeg, dw * s, dh * s, vw, vh, s, (vh - dh * s) / 2f))
            }
        }
    }

    companion object {
        private const val TAG = "FileFlow.Aim"
        private const val TARGET_LONG_EDGE = 720
        private const val PAINT_INTERVAL_MS = 66L  // ~15 repaints a second
    }
}
