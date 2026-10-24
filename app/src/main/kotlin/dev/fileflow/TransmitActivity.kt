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
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.text
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
    /** Display states per second this run presents: panel refresh / divisor. */
    private var stateRate = 0.0
    /** The Speed setting's display states per second. */
    private var targetStateRate = Speed.DEFAULT.statesPerSecond
    /** Eight colours (M5) rather than black and white (F62). */
    private var colour = Speed.DEFAULT.colour
    private var reported = false

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

        // Defaults are the forgiving grid: a launch from the share sheet carries no grid at all.
        val cols = intent.getIntExtra("cols", dev.fileflow.rx.Grids.ALL[0].first)
        val rows = intent.getIntExtra("rows", dev.fileflow.rx.Grids.ALL[0].second)
        // 0 = no timed report: the run lasts until the person presses Back, and reports then.
        // Scripted measurement runs still pass a duration.
        val seconds = intent.getIntExtra("seconds", 0)
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

        // States per second, not refreshes per state, is what has to be the same on every phone.
        //
        // The rate is the Speed setting's (F60: 20/s suits any receiving camera, 30/s a 60 fps
        // one). A fixed divisor gave the intended rate on a 120 Hz panel and half of it on a
        // 60 Hz one; the renderer now re-derives the divisor from the vsync rate it measures
        // (F59). An explicit `divisor` extra still wins, for scripted sweeps.
        targetStateRate = intent.getDoubleExtra("stateRate", Speed.DEFAULT.statesPerSecond)
            .coerceIn(1.0, 120.0)
        colour = intent.getBooleanExtra("colour", Speed.DEFAULT.colour)
        val panelHz = if (requestedHz > 0.0) requestedHz else (display?.refreshRate ?: 60f).toDouble()
        val divisor = if (intent.hasExtra("divisor")) {
            intent.getIntExtra("divisor", 1).coerceAtLeast(1)
        } else {
            Math.round(panelHz / targetStateRate).toInt().coerceAtLeast(1)
        }
        stateRate = panelHz / divisor

        // From "Send a file…" the document is the intent data; from the share sheet, EXTRA_STREAM.
        val uri = intent.data ?: if (intent.action == android.content.Intent.ACTION_SEND) {
            androidx.core.content.IntentCompat.getParcelableExtra(
                intent, android.content.Intent.EXTRA_STREAM, android.net.Uri::class.java,
            )
        } else null
        if (uri == null && intent.action == android.content.Intent.ACTION_SEND) {
            // Shared text, or a share with no file attached: never fall back to the test file.
            fail("Nothing to send — FileFlow sends a file, and this share did not include one.")
            return
        }
        if (uri == null) {
            val t = runCatching {
                Transmitter.open(cols, rows, nsym, payload, seed = 5L, colour = colour)
            }
                .getOrElse { e ->
                    fail("Could not start sending: ${e.message}")
                    return
                }
            begin(t, divisor, marginPx, seconds)
            return
        }

        // A picked file is read OFF the main thread -- it can be tens of megabytes, from a cloud
        // provider that has to download it first -- with a holding screen meanwhile.
        setContentView(message("Preparing the file…"))
        Thread {
            val opened = runCatching {
                val (name, size) = describe(uri)
                if (size > MAX_FILE_BYTES) {
                    error("$name is ${formatBytes(size)}. The most this can send is " +
                          "${formatBytes(MAX_FILE_BYTES)} — at optical speeds a bigger file " +
                          "would take too long to be practical.")
                }
                val bytes = contentResolver.openInputStream(uri)?.use { input ->
                    // Bounded read: a provider that misreported the size cannot exhaust memory.
                    val out = java.io.ByteArrayOutputStream()
                    val buf = ByteArray(64 * 1024)
                    var total = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        total += n
                        if (total > MAX_FILE_BYTES) {
                            error("$name is larger than ${formatBytes(MAX_FILE_BYTES)}.")
                        }
                        out.write(buf, 0, n)
                    }
                    out.toByteArray()
                } ?: error("The file could not be opened.")
                if (bytes.isEmpty()) error("$name is empty — there is nothing to send.")
                Transmitter.openFile(cols, rows, nsym, bytes, name, colour)
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    opened.getOrNull()?.close()
                    return@runOnUiThread
                }
                opened.fold(
                    onSuccess = { t -> begin(t, divisor, marginPx, seconds) },
                    onFailure = { e -> fail(e.message ?: "The file could not be read.") },
                )
            }
        }.start()
    }

    /** Display name and size of a picked document; size -1 when the provider does not say. */
    private fun describe(uri: android.net.Uri): Pair<String, Long> {
        var name = uri.lastPathSegment?.substringAfterLast('/') ?: "file"
        var size = -1L
        contentResolver.query(
            uri,
            arrayOf(
                android.provider.OpenableColumns.DISPLAY_NAME,
                android.provider.OpenableColumns.SIZE,
            ),
            null, null, null,
        )?.use { c ->
            if (c.moveToFirst()) {
                c.getString(0)?.let { name = it }
                if (!c.isNull(1)) size = c.getLong(1)
            }
        }
        return name to size
    }

    /** Starts presenting frames for [t]. */
    private fun begin(t: Transmitter, divisor: Int, marginPx: Int, seconds: Int) {
        tx = t
        SessionLog.start(this, "send")
        SessionLog.i(LOG_TAG, "sending ${t.fileName}, ${formatBytes(t.fileBytes)}, " +
            "${t.cols}x${t.rows} at ${"%.0f".format(targetStateRate)} states/s")
        // An explicit divisor is a scripted sweep and is held fixed; otherwise the renderer
        // re-derives it from the vsync rate the panel actually runs at (F59).
        val adaptive = !intent.hasExtra("divisor")
        val r = OpticalRenderer(t, divisor, marginPx, if (adaptive) targetStateRate else 0.0)
        renderer = r

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(2)
            setRenderer(r)
            // CONTINUOUSLY: the renderer must be driven by the display's own vsync, because the
            // whole point is to present one state per refresh (or per Nth refresh). RENDERMODE_
            // WHEN_DIRTY would couple the cadence to our own scheduling instead of the panel's.
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        val fileBytes = t.fileBytes
        val perFrame = t.symbolBytes
        val estimate = if (perFrame > 0 && stateRate > 0.0) {
            // Data frames needed, plus the manifest frames interleaved with them; a perfect link.
            val frames = Math.ceil(fileBytes.toDouble() / perFrame) * 1.05
            formatSeconds(frames / stateRate)
        } else null

        // What is being sent, and how to stop, in the black margin ABOVE the optical frame -- never
        // over it. The margin exists to keep the frame off the panel's rounded corners (F34); on
        // the reference phones it is 260-290 px tall, room for one quiet row. The text is dim so it
        // stays well below the receiver's lit threshold, and it is shown only once the frame's
        // real size is known and the row fits; otherwise a toast says the same thing once.
        val bar = sendingBar(t, estimate)
        setContentView(android.widget.FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
            addView(glView)
            addView(bar, android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.TOP,
            ))
        })
        bar.postDelayed({
            val margin = (r.surfaceHeight - r.drawnHeight) / 2
            if (r.drawnHeight > 0 && bar.height in 1..(margin - dp(8))) {
                bar.visibility = android.view.View.VISIBLE
            } else {
                android.widget.Toast.makeText(
                    this,
                    "Sending ${t.fileName} (${formatBytes(fileBytes)}). Back stops.",
                    android.widget.Toast.LENGTH_LONG,
                ).show()
            }
        }, BAR_FIT_DELAY_MS)
        startedMs = android.os.SystemClock.elapsedRealtime()
        ticker.post(tick)

        // Scripted runs finish on a timer rather than on a frame count, so a device that cannot hit
        // the rate produces a SHORTER report rather than running longer to reach a quota — which
        // would hide the very shortfall being measured.
        if (seconds > 0) glView?.postDelayed({ report(null, r) }, seconds * 1000L)
    }

    private var startedMs = 0L
    private var barMeta: android.widget.TextView? = null
    private var barMetaPrefix = ""
    private val ticker = android.os.Handler(android.os.Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            val s = (android.os.SystemClock.elapsedRealtime() - startedMs) / 1000
            barMeta?.text = "$barMetaPrefix · ${s / 60}:${"%02d".format(s % 60)}"
            ticker.postDelayed(this, 1000L)
        }
    }

    /** The row in the top margin: file, size, elapsed time, Stop. Starts hidden; see [begin]. */
    private fun sendingBar(t: Transmitter, estimate: String?): android.view.View {
        // ~12% linear luminance against the frame's 100%: readable on black, and captured far
        // below the threshold that separates the frame's lit cells from its dark ones.
        val dim = android.graphics.Color.parseColor("#70757A")
        val meta = text("", 13f).apply { setTextColor(dim) }
        barMeta = meta
        barMetaPrefix = formatBytes(t.fileBytes) +
            (estimate?.let { " · ≥$it at best" } ?: "")
        return android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            visibility = android.view.View.INVISIBLE
            setPadding(dp(20), dp(10), dp(12), dp(6))
            addView(android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.VERTICAL
                addView(text(t.fileName, 15f).apply {
                    setTextColor(dim)
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
                })
                addView(meta)
            }, android.widget.LinearLayout.LayoutParams(
                0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f,
            ))
            addView(button("Stop", R.drawable.ic_close, dev.fileflow.ui.Ui.Style.Text) { finish() }
                .apply {
                    setTextColor(dim)
                    iconTint = android.content.res.ColorStateList.valueOf(dim)
                    minHeight = dp(40)
                })
        }
    }

    /** A centred holding screen: a spinner while working, or an icon when something went wrong. */
    private fun message(s: String, error: Boolean = false) = android.widget.LinearLayout(this).apply {
        orientation = android.widget.LinearLayout.VERTICAL
        gravity = android.view.Gravity.CENTER
        setBackgroundColor(getColor(R.color.ff_background))
        setPadding(dp(32), dp(32), dp(32), dp(32))
        if (error) {
            addView(android.widget.ImageView(context).apply {
                setImageResource(R.drawable.ic_error)
                imageTintList = android.content.res.ColorStateList.valueOf(getColor(R.color.ff_bad))
            }, android.widget.LinearLayout.LayoutParams(dp(64), dp(64)))
        } else {
            addView(com.google.android.material.progressindicator.CircularProgressIndicator(context)
                .apply { isIndeterminate = true })
        }
        addView(text(s, 17f, com.google.android.material.R.attr.colorOnSurface).apply {
            gravity = android.view.Gravity.CENTER
            setPadding(0, dp(20), 0, 0)
        })
        if (error) {
            addView(button("Back", style = dev.fileflow.ui.Ui.Style.Tonal) { finish() }
                .apply { layoutParams = dev.fileflow.ui.Ui.matchWidth(dp(28)) })
        }
    }

    private fun fail(why: String) {
        report(why, null)
        setContentView(message(why, error = true))
    }

    private fun report(error: String?, r: OpticalRenderer?) {
        if (r != null) reported = true
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
            appendLine("  file             ${t?.fileName}, ${t?.fileBytes} bytes")
            appendLine("  state rate       ${"%.2f".format(stateRate)} /s requested (refresh / divisor)")
            appendLine("  panel measured   ${"%.1f".format(r.measuredVsyncHz)} Hz, divisor " +
                       "${r.divisor} at the end (0 Hz = not adaptive)")
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

        SessionLog.i(LOG_TAG, "--- transmit report ---")
        body.lineSequence().forEach { SessionLog.i(LOG_TAG, it) }
        runCatching { File(filesDir, REPORT_FILE).writeText(body) }
    }

    override fun onPause() {
        super.onPause()
        glView?.onPause()
        // A run that ends when the person presses Back still leaves its report.
        if (isFinishing) renderer?.let { if (!reported) report(null, it) }
    }

    override fun onResume() {
        super.onResume()
        glView?.onResume()
    }

    override fun onDestroy() {
        super.onDestroy()
        ticker.removeCallbacks(tick)
        tx?.close()
        tx = null
        SessionLog.close()
    }

    companion object {
        const val LOG_TAG = "FileFlow.Tx"
        const val REPORT_FILE = "transmit-report.txt"
        /**
         * Largest file accepted. The whole file is held in memory on both phones, and at the
         * simulated tens of KB/s a file this size is already many minutes of holding a phone up.
         */
        const val MAX_FILE_BYTES = 64L * 1024 * 1024
        /** Long enough for the GL surface to be sized and the bar measured. */
        private const val BAR_FIT_DELAY_MS = 500L

        fun formatBytes(n: Long): String = when {
            n < 1024 -> "$n bytes"
            n < 1024 * 1024 -> "%.1f KB".format(n / 1024.0)
            else -> "%.1f MB".format(n / (1024.0 * 1024.0))
        }

        fun formatSeconds(s: Double): String = when {
            s < 90 -> "${Math.ceil(s).toInt()} s"
            s < 5400 -> "${Math.ceil(s / 60).toInt()} min"
            else -> "%.1f h".format(s / 3600)
        }
    }
}
