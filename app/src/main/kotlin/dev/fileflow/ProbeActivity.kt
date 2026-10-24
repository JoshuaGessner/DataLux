package dev.fileflow

import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import dev.fileflow.probe.CapabilityProbe
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.card
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title

/**
 * Runs the capability probe and shows its verdict. Deliberately the whole UI for now.
 *
 * The shortest path to real data is: Gradle/NDK skeleton → capability probe → camera recorder.
 * There is no transmitter, no UI flow and no live link here, and adding them before the probe has
 * run on hardware would be building on unmeasured ground (EXP-006, EXP-007).
 *
 * ⚠ This screen will report `UNSUPPORTED` on every device today, and that is the correct answer.
 * `CapabilityProbe.readClaims` gathers claims only; the probe's policy refuses to act on an
 * unverified claim (RISK-011), so no tier is granted until measurement upgrades the evidence. The
 * notes list explains it on screen rather than leaving the reader to guess.
 */
class ProbeActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(window, false)

        val body = runCatching { report() }
            .getOrElse { e -> "probe failed: ${e::class.simpleName}: ${e.message}\n" +
                              e.stackTraceToString() }
        emit(body)

        // The full report, folded away: it is evidence for a bug report, not something to read.
        val full = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            text = body
            setTextIsSelectable(true)  // so a tester can copy the verdict into a bug report
            visibility = android.view.View.GONE
            setPadding(0, dp(12), 0, 0)
        }
        val col = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(24))
            addView(android.widget.LinearLayout(context).apply {
                orientation = android.widget.LinearLayout.HORIZONTAL
                gravity = android.view.Gravity.CENTER_VERTICAL
                addView(com.google.android.material.button.MaterialButton(
                    context, null, com.google.android.material.R.attr.materialIconButtonStyle,
                ).apply {
                    setIconResource(R.drawable.ic_close)
                    contentDescription = "Close"
                    setOnClickListener { finish() }
                })
                addView(title("Device check", 24f).apply { setPadding(dp(4), 0, 0, 0) })
            })
            addView(card(*summary().map { (k, v) -> fact(k, v) }.toTypedArray()), matchWidth(dp(8)))
            addView(card(
                button("Show the full report", R.drawable.ic_info, Style.Text) {
                    full.visibility = if (full.visibility == android.view.View.VISIBLE)
                        android.view.View.GONE else android.view.View.VISIBLE
                }.apply { layoutParams = matchWidth() },
                full,
            ), matchWidth(dp(12)))
            addView(button("Share report", R.drawable.ic_share, Style.Tonal) {
                val send = android.content.Intent(android.content.Intent.ACTION_SEND)
                    .setType("text/plain")
                    .putExtra(android.content.Intent.EXTRA_SUBJECT, "FileFlow device check")
                    .putExtra(android.content.Intent.EXTRA_TEXT, body)
                runCatching { startActivity(android.content.Intent.createChooser(send, null)) }
            }, matchWidth(dp(12)))
        }
        setContentView(ScrollView(this).apply {
            setBackgroundColor(getColor(R.color.ff_background))
            addView(col)
        }.padForSystemBars())
    }

    /** A label and its value, one row of the summary. */
    private fun fact(label: String, value: String) = android.widget.LinearLayout(this).apply {
        orientation = android.widget.LinearLayout.VERTICAL
        setPadding(0, dp(6), 0, dp(6))
        addView(text(label, 13f))
        addView(text(value, 16f, com.google.android.material.R.attr.colorOnSurface))
    }

    /**
     * What this phone means for FileFlow, in plain words: what the receiver will run at, how
     * close it can focus, how many cores it will decode on. Claims, labelled as such -- the full
     * report below is the evidence.
     */
    private fun summary(): List<Pair<String, String>> = runCatching {
        val (claims, _) = CapabilityProbe.readClaims(this)
        val fps = dev.fileflow.capture.CameraRecorder.Modes.chooseFps(this, 4096)
        val mgr = getSystemService(CAMERA_SERVICE) as android.hardware.camera2.CameraManager
        val back = mgr.cameraIdList.firstOrNull {
            mgr.getCameraCharacteristics(it)
                .get(android.hardware.camera2.CameraCharacteristics.LENS_FACING) ==
                android.hardware.camera2.CameraCharacteristics.LENS_FACING_BACK
        }
        val minFocus = back?.let {
            mgr.getCameraCharacteristics(it)
                .get(android.hardware.camera2.CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE)
        } ?: 0f
        listOf(
            "Screen" to "${claims.panelWidth}×${claims.panelHeight}, up to " +
                "${claims.maxRefreshHz.toInt()} Hz",
            "Receiving camera" to "$fps fps" +
                if (fps >= 50) " — can receive every speed" else " — best with Steady",
            "Closest focus" to if (minFocus > 0f) "%.0f cm".format(100f / minFocus) else "fixed focus",
            "Decoding" to "${dev.fileflow.rx.LiveDecoder.DEFAULT_LANES} cores in parallel",
        )
    }.getOrElse { listOf("Summary" to "unavailable: ${it.message}") }

    /**
     * Emits the report to logcat and to a file, so it can be collected as raw experiment data
     * rather than transcribed off a screen.
     *
     * `data/experiments/` is append-only and every result must carry its provenance
     * (CONTRIBUTING). A number read off a phone by eye has none, so the machine-collectable copy
     * is the one that counts. Chunked because logcat truncates long lines.
     */
    private fun emit(body: String) {
        body.lineSequence().forEach { android.util.Log.i(LOG_TAG, it) }
        runCatching {
            java.io.File(filesDir, REPORT_FILE).writeText(body)
            android.util.Log.i(LOG_TAG, "report written to $filesDir/$REPORT_FILE")
        }.onFailure { android.util.Log.w(LOG_TAG, "could not write report: ${it.message}") }
    }

    companion object {
        const val LOG_TAG = "FileFlow.Report"
        const val REPORT_FILE = "probe-report.txt"
    }

    /** Human-readable `INFO_SUPPORTED_HARDWARE_LEVEL`. The raw ordinal is not ordered by
     *  capability (LEGACY=2 sits above FULL=1), which is a genuine trap when reading a report. */
    private fun hardwareLevelName(level: Int): String = when (level) {
        0 -> "LIMITED"
        1 -> "FULL"
        2 -> "LEGACY  <-- worst tier despite the highest number"
        3 -> "LEVEL_3"
        4 -> "EXTERNAL"
        else -> "unknown($level)"
    }

    /** Every display mode the panel advertises, so OQ-005's question about holding a high mode
     *  has a list of candidates to be asked about. */
    private fun displayModes(): List<String> = runCatching {
        // `display` is API 30+ and minSdk is 33, so there is no legacy branch to keep here.
        (display?.supportedModes ?: emptyArray()).sortedByDescending { it.refreshRate }.map {
            "${it.physicalWidth}x${it.physicalHeight} @ ${"%.3f".format(it.refreshRate)} Hz"
        }
    }.getOrElse { listOf("unavailable: ${it.message}") }

    private fun appVersion(): String = runCatching {
        packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
    }.getOrElse { "?" }

    private fun report(): String = buildString {
        appendLine("FileFlow capability probe (C02)")
        appendLine("=".repeat(52))
        appendLine()

        // Provenance first. A capability report without the exact build it came from is not
        // usable as evidence (CONTRIBUTING).
        appendLine("PROVENANCE")
        appendLine("  collected        ${java.util.Date()}")
        appendLine("  fingerprint      ${android.os.Build.FINGERPRINT}")
        appendLine("  android          ${android.os.Build.VERSION.RELEASE} " +
                   "(API ${android.os.Build.VERSION.SDK_INT})")
        appendLine("  app              $packageName ${appVersion()}")
        appendLine("  abis             ${android.os.Build.SUPPORTED_ABIS.joinToString(",")}")
        appendLine()

        val (claims, modes) = CapabilityProbe.readClaims(this@ProbeActivity)
        appendLine("CLAIMED BY THE PLATFORM")
        appendLine("  model            ${claims.model}")
        appendLine("  soc              ${claims.soc}")
        appendLine("  max refresh      ${claims.maxRefreshHz} Hz")
        appendLine("  panel            ${claims.panelWidth}x${claims.panelHeight}")
        appendLine("  hardware level   ${hardwareLevelName(claims.hardwareLevel)}")
        appendLine("  manual sensor    ${claims.claimsManualSensor}")
        appendLine("  realtime clock   ${claims.timestampSourceRealtime}")
        appendLine("  RS skew          ${claims.rollingShutterSkewNs} ns")
        appendLine()

        appendLine("DISPLAY MODES (all)")
        displayModes().forEach { appendLine("    $it") }
        appendLine()

        // The FULL list, deliberately untruncated. This is EXP-007's enumeration half, and a
        // truncated list could silently drop the high-speed entries milestone 6 depends on.
        appendLine("CAMERA MODES (all ${modes.size})")
        modes.forEach {
            val kind = if (it.highSpeed) "HIGH-SPEED" else if (it.cpuReadable) "cpu-yuv" else "?"
            appendLine("    ${it.width}x${it.height} @ ${"%.2f".format(it.maxFps)} fps  $kind")
        }
        val fastestCpu = modes.filter { it.cpuReadable }.maxOfOrNull { it.maxFps } ?: 0.0
        val fastestHs = modes.filter { it.highSpeed }.maxOfOrNull { it.maxFps } ?: 0.0
        appendLine("  high-speed modes offered   ${modes.count { it.highSpeed }}")
        appendLine("  fastest cpu-readable       ${"%.2f".format(fastestCpu)} fps")
        appendLine("  fastest high-speed         ${"%.2f".format(fastestHs)} fps")
        appendLine("  OQ-035 asks whether Samsung restricts >=60 fps capture for third-party")
        appendLine("  apps. THIS IS ONLY WHAT IS ADVERTISED. Whether those frames arrive, and")
        appendLine("  whether they are DISTINCT, is EXP-007 and is not answered here.")
        appendLine()

        appendLine("NATIVE VERDICT (core/device.cpp — ADR-0014)")
        appendLine(dev.fileflow.probe.NativeProbe.decide(claims, modes).toString())
        appendLine()

        appendLine("-".repeat(52))
        appendLine("UNSUPPORTED is expected here. Nothing above was VERIFIED — the probe")
        appendLine("gathers claims only, and a claim is not evidence (RISK-011). Fd in")
        appendLine("particular is NOT the refresh rate and must be measured: EXP-006.")
        appendLine("The camera path needs EXP-007. Until those run, no tier is earned.")
    }
}
