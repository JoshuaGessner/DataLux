package dev.fileflow

import android.opengl.GLSurfaceView
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import dev.fileflow.tx.OpticalRenderer
import dev.fileflow.tx.Transmitter
import java.io.File

/**
 * Presents optical frames full-screen (C03) and reports what it managed to submit.
 *
 * Scriptable, so a run's parameters land in its report rather than in someone's memory:
 *
 * ```
 * adb shell am start -n dev.fileflow/.TransmitActivity \
 *     --ei cols 144 --ei rows 240 --ei divisor 1 --ei seconds 20 --ei nsym 32
 * ```
 *
 * ## What this measures, and what it deliberately does not
 *
 * It measures **states submitted** and the intervals between them. It does **not** measure `Fd`.
 *
 * A transmitter cannot confirm presentation — that is precisely why every frame header carries its
 * own sequence number and phase indicator (`frame.h`), and why `Fd` is defined as *distinct
 * presented states per second* measured by the **receiver**. Reporting a submission rate as `Fd`
 * would be the unlabelled-rate defect ADR-0012 exists to prevent, and it would flatter us: a state
 * submitted and then dropped by the compositor has been submitted, not presented.
 *
 * So the number below is an **upper bound on `Fd`**, and it is labelled as one.
 */
class TransmitActivity : AppCompatActivity() {

    private var glView: GLSurfaceView? = null
    private var tx: Transmitter? = null
    private var renderer: OpticalRenderer? = null
    private var requestedHz = 0.0
    private var requestedMode = "?"


    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Turn the screen on and show over the keyguard.
        //
        // Not convenience: Android REFUSES camera access to a background process, and an activity
        // launched by `am start` onto a locked device never reaches the foreground. The first
        // two-device attempt failed with CAMERA_DISABLED ("cannot open camera from background") for
        // exactly this reason, and on the transmitter side a locked screen displays nothing at all.
        // A test rig that depends on someone having left both phones unlocked is not a rig.
        setShowWhenLocked(true)
        setTurnScreenOn(true)

        // The screen must stay on and at a fixed brightness for the whole run. A dimming screen
        // changes the transmitted luminance mid-transfer, which the receiver would see as the
        // channel degrading (C01's responsibility, exercised here).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_FULL
        }

        val cols = intent.getIntExtra("cols", 144)
        val rows = intent.getIntExtra("rows", 240)
        val divisor = intent.getIntExtra("divisor", 1).coerceAtLeast(1)
        val seconds = intent.getIntExtra("seconds", 20)
        val nsym = intent.getIntExtra("nsym", 32)
        // 0 = the default size, 64 symbols' worth, kept so runs stay comparable with older captures.
        //
        // Under protocol v1 a payload that did not fill its block left trailing source symbols ZERO,
        // and a zero symbol rendered as an almost-black frame (F39). Protocol v2 never transmits
        // padding at all (ADR-0015), so any size is now safe -- including a real file's.
        val payload = intent.getIntExtra("payload", 0)
        // Black border in panel pixels. Default is generous rather than zero: a full-bleed frame has
        // its corners clipped by the display glass, which breaks localisation while everything else
        // looks fine (F34). Set 0 deliberately to reproduce that.
        val marginPx = intent.getIntExtra("marginPx", 72)

        // Go genuinely edge to edge and hide the system bars.
        //
        // Not cosmetic: without this the status and navigation bars occupy part of the screen, the
        // GL surface is SMALLER than the panel, and the cell pitch is not what any calculation based
        // on panel metrics says it is. The first run of this activity presented into a surface
        // shortened by the navigation bar, and the frame's always-bright bottom border came back
        // dark in a screenshot (F31).
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
        WindowInsetsControllerCompat(window, window.decorView).apply {
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }

        // Prefer the mode with the MOST PIXELS among those at the highest refresh rate.
        //
        // Selecting on refresh alone picked a 1080x2340 mode on a 1440x3120 panel, because several
        // resolutions offer 120 Hz — and that silently made the charter's 144x240 grid fractional
        // at 7.5 x 9.75 px/cell (F31). Resolution and refresh are both load-bearing here, and
        // resolution is the one a naive `maxByOrNull { refreshRate }` throws away.
        //
        // This is still only a REQUEST. Samsung gates panel resolution behind a system display
        // setting, so the surface may come back smaller regardless; the renderer checks what it
        // actually got rather than assuming this worked.
        val modes = display?.supportedModes?.toList().orEmpty()
        val topHz = modes.maxOfOrNull { it.refreshRate } ?: 0f
        val best = modes
            .filter { it.refreshRate >= topHz - 0.5f }
            .maxByOrNull { it.physicalWidth.toLong() * it.physicalHeight }
        requestedHz = best?.refreshRate?.toDouble() ?: 0.0
        requestedMode = best?.let { "${it.physicalWidth}x${it.physicalHeight}" } ?: "?"
        if (best != null) {
            window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
        }

        val t = runCatching { Transmitter.open(cols, rows, nsym, payload, seed = 5L) }
            .getOrElse { e ->
                report("transmitter open failed: ${e.message}", null)
                return
            }
        tx = t
        val r = OpticalRenderer(t, divisor, marginPx)
        renderer = r

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(2)
            setRenderer(r)
            // CONTINUOUSLY: the renderer must be driven by the display's own vsync, because the
            // whole point is to present one state per refresh (or per Nth refresh). RENDERMODE_
            // WHEN_DIRTY would couple the cadence to our own scheduling instead of the panel's.
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        setContentView(glView)

        // Finish the run on a timer rather than on a frame count, so a device that cannot hit the
        // rate produces a SHORTER report rather than running longer to reach a quota — which would
        // hide the very shortfall we are measuring.
        glView?.postDelayed({ report(null, r) }, seconds * 1000L)
    }

    private fun report(error: String?, r: OpticalRenderer?) {
        val body = buildString {
            appendLine("FileFlow transmit run (C03) — states SUBMITTED, not Fd")
            appendLine("=".repeat(64))
            appendLine()
            appendLine("PROVENANCE")
            appendLine("  collected        ${java.util.Date()}")
            appendLine("  device           ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
            appendLine("  fingerprint      ${android.os.Build.FINGERPRINT}")
            appendLine()

            if (error != null) {
                appendLine("ERROR            $error")
                return@buildString
            }
            if (r == null) {
                appendLine("ERROR            no renderer")
                return@buildString
            }

            val t = tx
            appendLine("REQUESTED")
            appendLine("  grid             ${t?.cols}x${t?.rows}  (${(t?.cols ?: 0) * (t?.rows ?: 0)} cells)")
            appendLine("  payload cells    ${t?.payloadCells}")
            appendLine("  display mode     $requestedMode @ ${"%.3f".format(requestedHz)} Hz " +
                       "(REQUESTED — the surface below is what arrived)")
            appendLine("  payload sha256   ${t?.payloadSha256}")
            appendLine()

            val spanMs = (r.lastDrawUptimeMs - r.firstDrawUptimeMs).coerceAtLeast(0)
            val drawRate = if (spanMs > 0) r.framesDrawn * 1000.0 / spanMs else 0.0
            val stateRate = if (spanMs > 0) r.statesSubmitted * 1000.0 / spanMs else 0.0

            appendLine("OBSERVED")
            val sw = r.surfaceWidth
            val sh = r.surfaceHeight
            appendLine("  surface          ${sw}x$sh   <-- what GL actually gave us")
            appendLine("  cell pitch       ${r.pitchPx} px  (exact integer by construction)")
            appendLine("  frame drawn      ${r.drawnWidth}x${r.drawnHeight} centred, " +
                       "margin ${(sw - r.drawnWidth) / 2} x ${(sh - r.drawnHeight) / 2} px")
            appendLine("  → the margin keeps the boundary ring off the panel's rounded")
            appendLine("    corners, which clip a full-bleed frame and break localisation (F34)")
            appendLine("  span             $spanMs ms")
            appendLine("  frames drawn     ${r.framesDrawn}  (${"%.2f".format(drawRate)} /s)")
            appendLine("  states submitted ${r.statesSubmitted}  (${"%.2f".format(stateRate)} /s)")
            appendLine("  render errors    ${r.renderErrors}")
            appendLine("  last sequence    ${r.lastSequence}")
            appendLine()

            val iv = r.submitIntervalsNs()
            if (iv.size >= 2) {
                val sorted = iv.sorted()
                val median = sorted[sorted.size / 2]
                // Modal-style summary: the median interval is the true cadence, and a max far above
                // it means dropped states rather than a slower loop (the F28 lesson, applied here).
                appendLine("  state interval   median ${"%.3f".format(median / 1e6)} ms" +
                           "  => ${"%.2f".format(1e9 / median)} /s cadence")
                appendLine("  interval spread  min ${"%.3f".format(sorted.first() / 1e6)} ms, " +
                           "max ${"%.3f".format(sorted.last() / 1e6)} ms")
                appendLine("  intervals >1.5x  ${iv.count { it > median * 3 / 2 }} of ${iv.size}")
            }
            appendLine()

            appendLine("-".repeat(64))
            appendLine("THIS IS AN UPPER BOUND ON Fd, NOT Fd.")
            appendLine("A transmitter cannot confirm presentation; a state submitted and then")
            appendLine("dropped by the compositor has been submitted, not presented. Fd is")
            appendLine("distinct presented states per second and is measured by the RECEIVER,")
            appendLine("from the sequence number every frame header carries. EXP-006 needs both")
            appendLine("devices; this run needs one.")
        }

        Log.i(LOG_TAG, "--- transmit report ---")
        body.lineSequence().forEach { Log.i(LOG_TAG, it) }
        runCatching { File(filesDir, REPORT_FILE).writeText(body) }
    }

    override fun onPause() {
        super.onPause()
        glView?.onPause()
    }

    override fun onResume() {
        super.onResume()
        glView?.onResume()
    }

    override fun onDestroy() {
        super.onDestroy()
        tx?.close()
        tx = null
    }

    companion object {
        const val LOG_TAG = "FileFlow.Tx"
        const val REPORT_FILE = "transmit-report.txt"
    }
}
