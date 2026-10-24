package dev.fileflow.tx

import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.os.SystemClock
import android.util.Log
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Presents optical frames as a fullscreen quad (C03, the renderer half).
 *
 * ## The one idea that makes this cheap and exact
 *
 * The texture is uploaded at **cell resolution** (e.g. 144×240 = 34,560 bytes), not panel
 * resolution (1440×3120 = 4.5 MB), and scaled up with `GL_NEAREST`.
 *
 * That is **bit-exact, not an approximation**, and only because the cell pitch is required to be an
 * integer number of panel pixels (DEVICE-MATRIX). At an exact 10× horizontal and 13× vertical
 * factor, nearest-neighbour sampling reproduces precisely the pixels a full-resolution render would
 * have produced, for 1/130th of the upload. **If the integer-pitch requirement is ever relaxed this
 * stops being exact** and the renderer must draw at panel resolution instead.
 *
 * ## Why no filtering, anywhere
 *
 * `GL_NEAREST` on both min and mag filters, and no mipmaps. Any interpolation would blur cell
 * boundaries — softening exactly the high-spatial-frequency edges the receiver's sampler exists to
 * resolve, and doing it on the transmitter where no amount of receiver cleverness can undo it.
 *
 * ## What this does NOT claim
 *
 * It reports states **submitted** and vsyncs **observed**. It does not report `Fd`. A transmitter
 * cannot confirm presentation — that is the premise of the sequence number in every frame header
 * (`frame.h`), and `Fd` is a quantity the RECEIVER measures by decoding those numbers. Calling a
 * submission rate `Fd` would be exactly the unlabelled-rate defect ADR-0012 forbids.
 */
class OpticalRenderer(
    private val tx: Transmitter,
    /** Present a new optical state every Nth vsync. 1 = every vsync. */
    private var vsyncDivisor: Int,
    /**
     * Pixels of black to keep clear on every side, before the pitch is computed.
     *
     * NOT cosmetic. A phone's display has generously rounded corners, so a full-bleed optical frame
     * has its four corners **physically clipped by the glass** — measured at up to 77 camera pixels,
     * about 10 cells, which puts an entire corner marker outside the emitting area (F34). Screen
     * localisation finds the quad from the extremes of `x+y` and `x-y` and then verifies the corner
     * markers, so both steps fail while framing, focus, exposure and density all look healthy.
     *
     * The simulator renders a perfect rectangle and cannot exhibit this at all, which is what makes it
     * RISK-024's failure mode rather than a bug: a kinder-than-reality model producing a confident
     * wrong conclusion.
     */
    private val marginPx: Int = 0,
    /**
     * When > 0, states per second to aim for, with the divisor re-derived from the MEASURED vsync
     * interval rather than taken as given.
     *
     * The requested display mode is only a request. On the Pixel 8 a 120 Hz request came back as a
     * panel running at 60, so a divisor computed for 120 presented 7.5 states a second instead of 15
     * and halved the link without a word (F59). Panels also change rate on their own (adaptive
     * refresh), so it is re-measured every [REMEASURE_FRAMES] frames, not once.
     */
    private val targetStateRate: Double = 0.0,
) : GLSurfaceView.Renderer {

    /** Divisor in use right now, and the vsync rate it was derived from (0 = not yet measured). */
    val divisor: Int get() = vsyncDivisor
    @Volatile var measuredVsyncHz: Double = 0.0; private set
    private val vsyncIntervals = LongArray(REMEASURE_FRAMES)
    private var vsyncIntervalCount = 0
    private var lastDrawNs = 0L

    /** Observed, not requested. */
    @Volatile var framesDrawn: Long = 0L; private set
    @Volatile var statesSubmitted: Long = 0L; private set
    @Volatile var renderErrors: Long = 0L; private set
    @Volatile var firstDrawUptimeMs: Long = -1L; private set
    @Volatile var lastDrawUptimeMs: Long = -1L; private set
    @Volatile var lastSequence: Int = -1; private set

    /**
     * The surface size GL actually gave us, which is **not** necessarily the panel's native
     * resolution (F31). Everything about cell pitch has to be computed from this rather than from
     * `Display` metrics, because this is what the pixels are drawn into.
     */
    @Volatile var surfaceWidth: Int = 0; private set
    @Volatile var surfaceHeight: Int = 0; private set

    /** The integer cell pitch actually used, and the pixel size of the drawn frame. */
    @Volatile var pitchPx: Int = 0; private set
    @Volatile var drawnWidth: Int = 0; private set
    @Volatile var drawnHeight: Int = 0; private set

    /**
     * Wall-clock interval between consecutive *state submissions*, in nanoseconds.
     *
     * Bounded and pre-allocated: the render thread must not allocate, and an unbounded log of a
     * multi-minute run would itself become the performance problem.
     */
    private val submitIntervals = LongArray(MAX_LOGGED_INTERVALS)
    @Volatile var loggedIntervals: Int = 0; private set
    private var lastSubmitNs = 0L

    private var program = 0
    private var texture = 0
    private var posAttrib = 0
    private var texAttrib = 0
    private var texUniform = 0
    private var vsyncCounter = 0

    fun submitIntervalsNs(): LongArray = submitIntervals.copyOf(loggedIntervals)

    /** Render thread. Median vsync interval over the last window sets the divisor. */
    private fun adaptDivisor(now: Long) {
        if (lastDrawNs != 0L) vsyncIntervals[vsyncIntervalCount++] = now - lastDrawNs
        lastDrawNs = now
        if (vsyncIntervalCount < vsyncIntervals.size) return
        vsyncIntervalCount = 0
        vsyncIntervals.sort()  // in place: the window is reused, nothing allocated
        val median = vsyncIntervals[vsyncIntervals.size / 2]
        if (median <= 0L) return
        val hz = 1e9 / median
        val d = Math.round(hz / targetStateRate).toInt().coerceAtLeast(1)
        if (d != vsyncDivisor || measuredVsyncHz == 0.0) {
            dev.fileflow.SessionLog.i(TAG, "panel measured at %.1f Hz: divisor %d -> %d (%.1f states/s)".format(
                hz, vsyncDivisor, d, hz / d))
        }
        measuredVsyncHz = hz
        vsyncDivisor = d
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        program = buildProgram()
        posAttrib = GLES20.glGetAttribLocation(program, "aPos")
        texAttrib = GLES20.glGetAttribLocation(program, "aTex")
        texUniform = GLES20.glGetUniformLocation(program, "uTex")

        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        texture = ids[0]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
        // No filtering and no wrapping. See the class comment: interpolation here would blur cell
        // edges irrecoverably.
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST,
        )
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST,
        )
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE,
        )
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE,
        )
        // Rows are tightly packed one byte per cell, so alignment must be 1. The default of 4 would
        // make GL read padding that is not there whenever the cell count is not a multiple of 4 --
        // shearing the image for some grids and not others, which is a horrible class of bug.
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        surfaceWidth = width
        surfaceHeight = height

        // Largest INTEGER pitch that fits inside the margin, then centre the result.
        //
        // This replaces stretching the grid across the whole surface, and it removes a whole failure
        // mode rather than merely warning about it. Full-bleed rendering required the surface to be an
        // exact multiple of the grid, and when it was not — as on a 1080x2340 surface with the 144x240
        // charter grid — the pitch came out at 7.5 x 9.75 px and every cell boundary landed on a
        // fractional pixel (F31). Choosing the pitch and letterboxing makes it integer for ANY
        // grid/surface pair, and the leftover margin is what keeps the boundary ring off the panel's
        // rounded corners (F34).
        val usableW = (width - 2 * marginPx).coerceAtLeast(1)
        val usableH = (height - 2 * marginPx).coerceAtLeast(1)
        pitchPx = minOf(usableW / tx.cols, usableH / tx.rows).coerceAtLeast(1)
        drawnWidth = pitchPx * tx.cols
        drawnHeight = pitchPx * tx.rows

        dev.fileflow.SessionLog.i(TAG, "surface ${width}x$height, margin $marginPx => pitch $pitchPx px/cell, " +
                   "drawn ${drawnWidth}x$drawnHeight for a ${tx.cols}x${tx.rows} grid")

        if (drawnWidth > width || drawnHeight > height) {
            Log.e(TAG, "grid ${tx.cols}x${tx.rows} cannot fit ${width}x$height even at 1 px/cell")
        }

        // Quad in normalised device coordinates, centred, sized to the drawn frame. Everything
        // outside it is cleared to black, which is exactly the margin the corners need.
        val hx = drawnWidth.toFloat() / width
        val hy = drawnHeight.toFloat() / height
        QUAD_POS.position(0)
        QUAD_POS.put(floatArrayOf(-hx, -hy, hx, -hy, -hx, hy, hx, hy))
        QUAD_POS.position(0)
    }

    override fun onDrawFrame(gl: GL10?) {
        val now = SystemClock.elapsedRealtimeNanos()
        if (firstDrawUptimeMs < 0) firstDrawUptimeMs = SystemClock.uptimeMillis()
        lastDrawUptimeMs = SystemClock.uptimeMillis()
        framesDrawn++
        if (targetStateRate > 0.0) adaptDivisor(now)

        // Advance the optical state only every Nth vsync. Redrawing the SAME state on the other
        // vsyncs is deliberate: the display keeps refreshing regardless, and presenting an
        // unchanged frame is what "one display state spanning N refreshes" physically means.
        val advance = (vsyncCounter % vsyncDivisor) == 0
        vsyncCounter++

        if (advance) {
            val code = tx.nextFrame()
            if (code != 0) {
                renderErrors++
            } else {
                statesSubmitted++
                lastSequence = tx.lastSequence
                if (lastSubmitNs != 0L && loggedIntervals < submitIntervals.size) {
                    submitIntervals[loggedIntervals++] = now - lastSubmitNs
                }
                lastSubmitNs = now
                uploadTexture()
            }
        }

        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
        GLES20.glUniform1i(texUniform, 0)

        QUAD_POS.position(0)
        GLES20.glVertexAttribPointer(posAttrib, 2, GLES20.GL_FLOAT, false, 0, QUAD_POS)
        GLES20.glEnableVertexAttribArray(posAttrib)
        QUAD_TEX.position(0)
        GLES20.glVertexAttribPointer(texAttrib, 2, GLES20.GL_FLOAT, false, 0, QUAD_TEX)
        GLES20.glEnableVertexAttribArray(texAttrib)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun uploadTexture() {
        tx.cells.position(0)
        // GL_LUMINANCE: one byte per cell, broadcast to RGB by the sampler. M0 is a luminance
        // modulation, so a single channel is the honest representation — an RGB texture would
        // triple the upload to carry the same one bit per cell.
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE,
            tx.cols, tx.rows, 0,
            GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, tx.cells,
        )
    }

    private fun buildProgram(): Int {
        val vs = compile(GLES20.GL_VERTEX_SHADER, VERTEX_SRC)
        val fs = compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SRC)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        val status = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "program link failed: ${GLES20.glGetProgramInfoLog(p)}")
        }
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val status = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "shader compile failed: ${GLES20.glGetShaderInfoLog(s)}")
        }
        return s
    }

    companion object {
        private const val TAG = "FileFlow.Tx"
        private const val MAX_LOGGED_INTERVALS = 20_000
        /** Vsyncs per divisor re-measurement: half a second at 120 Hz. */
        private const val REMEASURE_FRAMES = 60

        // Texture V is flipped relative to GL's convention so cell row 0 lands at the TOP of the
        // screen. Getting this wrong produces a vertically mirrored frame, which decodes to garbage
        // in a way that looks like a geometry bug rather than an orientation one.
        // Rewritten in onSurfaceChanged once the pitch is known; the initial full-bleed values are
        // only a placeholder for the first frame.
        private val QUAD_POS = floatBuffer(
            -1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f,
        )
        private val QUAD_TEX = floatBuffer(
            0f, 1f, 1f, 1f, 0f, 0f, 1f, 0f,
        )

        private const val VERTEX_SRC = """
            attribute vec2 aPos;
            attribute vec2 aTex;
            varying vec2 vTex;
            void main() {
                vTex = aTex;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        """

        private const val FRAGMENT_SRC = """
            precision mediump float;
            varying vec2 vTex;
            uniform sampler2D uTex;
            void main() {
                gl_FragColor = texture2D(uTex, vTex);
            }
        """

        private fun floatBuffer(vararg v: Float): java.nio.FloatBuffer {
            val b = java.nio.ByteBuffer.allocateDirect(v.size * 4)
                .order(java.nio.ByteOrder.nativeOrder())
                .asFloatBuffer()
            b.put(v)
            b.position(0)
            return b
        }
    }
}
