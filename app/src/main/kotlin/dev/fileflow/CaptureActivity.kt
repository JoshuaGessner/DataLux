package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.fileflow.aim.AimAnalyser
import dev.fileflow.aim.AimVerdict
import dev.fileflow.aim.AimView
import dev.fileflow.aim.LumaPreviewView
import dev.fileflow.capture.CameraRecorder
import dev.fileflow.rx.Grids
import dev.fileflow.ui.StatusCard
import dev.fileflow.ui.Ui.Style
import dev.fileflow.ui.Ui.button
import dev.fileflow.ui.Ui.dp
import dev.fileflow.ui.Ui.matchWidth
import dev.fileflow.ui.Ui.showHelp
import dev.fileflow.ui.Ui.space
import dev.fileflow.ui.Ui.text
import dev.fileflow.ui.Ui.title
import dev.fileflow.rx.LiveDecoder
import dev.fileflow.rx.LiveHint
import dev.fileflow.rx.LivePhase
import dev.fileflow.rx.LiveStatus
import java.io.File

/**
 * Runs one capture and reports what the camera actually delivered.
 *
 * This is the verification half of EXP-007. The probe (C02) reports what the camera *advertises*;
 * this reports what arrives, which is a different thing and the one the goodput model depends on.
 *
 * Launch it with the frame count and rate as intent extras so a run is scriptable and its
 * parameters land in the report rather than in someone's memory:
 *
 * ```
 * adb shell am start -n dev.fileflow/.CaptureActivity \
 *     --ei frames 300 --ei fps 60 --ei maxWidth 1920 --es notes "rigid mount, office light"
 * ```
 *
 * The bundle is written to app-private storage and pulled with `run-as`. It is the same format
 * `ffreplay` consumes, so the moment this works every analysis tool in the repo applies to real
 * frames — which is the entire point of doing it before any transmitter exists.
 */
class CaptureActivity : AppCompatActivity() {

    private lateinit var text: TextView
    private var aimView: AimView? = null
    private var previewView: LumaPreviewView? = null
    /** Focus the aim phase settled on; carried into the recording so it stops hunting. */
    private var lockedFocus: Float = -1f
    private var aimRecorder: CameraRecorder? = null
    private var aimThread: Thread? = null
    /** Receive a file live (the Receive button) rather than record a dataset (scripts). */
    private val live: Boolean get() = intent.getBooleanExtra("live", false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        text = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            // Legible at arm's length on a phone. The first version was 11sp monospace on the default
            // LIGHT theme, which is what an operator actually saw while trying to line up a camera
            // during a two-device run: a wall of small text and no aiming help at all.
            textSize = 13f
            setTextColor(Color.parseColor("#CFD8DC"))
            setPadding(28, 28, 28, 28)
            setTextIsSelectable(true)
        }

        // Keep the receiver's screen awake for the whole run.
        //
        // `setShowWhenLocked`/`setTurnScreenOn` are kept because camera access IS refused to a
        // background process, and an early two-device attempt failed with CAMERA_DISABLED
        // ("cannot open camera from background", proc state 20). What that proves is only that the
        // app was not foreground -- the original comment here asserted a LOCKED SCREEN was the
        // cause, which was never verified, and the operator reports both phones were unlocked. Two
        // fixes were applied at once (these flags and a wake/dismiss-keyguard step in the script), so
        // which one mattered is genuinely unknown. Kept as cheap insurance, not as a diagnosis.
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            // Requested properly so the app works when driven by hand. For scripted runs the
            // permission is pre-granted with `adb shell pm grant`, which avoids a UI tap standing
            // between us and a measurement.
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
            showReport("Waiting for camera permission…")
            return
        }
        begin()
    }

    /**
     * Aim first when asked to, then record.
     *
     * Guidance before capture is the whole point: six consecutive hardware failures were framing or
     * exposure problems (F33, F34), and a capture taken before the geometry is workable produces a
     * dataset that cannot be decoded and cannot say why.
     *
     * **Defaults ON.** It defaulted off so scripted runs would not block, and the consequence was
     * that the safety check never ran in the path actually used: a run recorded 60 frames at 1.1%
     * lit pixels, failed geometry on 55 of them, and nothing objected (F36). A scripted rig that is
     * genuinely already lined up passes `--ez aim false` explicitly, which is the right way round —
     * the assertion of readiness should be the deliberate act, not the skipping of the check.
     */
    private fun begin() {
        // Live receiving has NO aim gate. It used to wait for six consecutive "Ready" verdicts, and
        // on the first hand-held test it never got them: the person had a good view of the screen
        // and the app did nothing for two minutes (F57). The decoder is the only judge that
        // matters -- if frames decode, the aim is good enough, whatever a heuristic thinks -- so
        // receiving starts at once and the aim analysis becomes advice shown while nothing reads.
        when {
            live -> startLive()
            intent.getBooleanExtra("aim", true) -> startAiming()
            else -> start()
        }
    }

    /** The camera picture with the aim overlay on it. Returns the overlay. */
    private fun buildViewfinder(cols: Int, rows: Int): AimView {
        val v = AimView(this, cols, rows)
        aimView = v

        // A real viewfinder, with the analysis drawn over it.
        //
        // The overlay alone could not be aimed with. It shows what is WRONG but not where the camera
        // is POINTING, so lining two phones up meant moving them blind and waiting for the verdict to
        // change (F37). The preview is a second target on the same capture session, so the compositor
        // scales and colour-converts it and none of the analysis budget goes on it.
        //
        // The SAME preview class the standalone aiming screen uses.
        //
        // This screen is what the Receive button opens, so it is the one an operator actually looks
        // at -- and it was the last holdout still using a SurfaceView inside a self-sizing box. That
        // arrangement can shape a container but cannot turn the pixels inside it, so a landscape
        // sensor stream stayed squashed however the box was shaped. Four rounds of "fixed the
        // preview" landed on the standalone screen while the reported stretch was here, in the path
        // nobody was testing. Two implementations of one thing meant a fix could look verified and
        // change nothing the operator saw, so there is now exactly one (F43).
        val preview = LumaPreviewView(this)
        previewView = preview
        // `--ez overlay false` hides the schematic, so a screenshot shows the preview alone. See
        // the note in AimActivity: automated checks with the overlay present measured the overlay.
        val showOverlay = intent.getBooleanExtra("overlay", true)
        val card = if (live) StatusCard(this) else null
        statusCard = card
        val controls = if (live) liveControls(v) else null
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
            addView(preview, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            if (showOverlay) addView(v, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            if (card != null) {
                addView(card, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.TOP))
            }
            if (controls != null) {
                addView(controls, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.BOTTOM))
            }
            // The picture runs edge to edge; the card, the controls and the overlay's text keep
            // clear of the system bars and the camera cutout.
            androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(this) { _, insets ->
                val i = insets.getInsets(SYSTEM_INSETS)
                val m = dp(12)
                card?.layoutParams = (card?.layoutParams as? FrameLayout.LayoutParams)?.apply {
                    setMargins(m, i.top + m, m, 0)
                }
                controls?.setPadding(dp(16), dp(8), dp(16), i.bottom + dp(12))
                v.setInsets(i.top.toFloat(), i.bottom.toFloat() + if (controls != null) dp(80) else 0)
                insets
            }
            // The framing guide fits between the card and the buttons. The card's lowest edge so
            // far, not its current one: it grows a line once the file is known, and a guide that
            // shrank mid-transfer would ask the person to move for nothing.
            if (card != null && controls != null) {
                var cardBottom = 0
                addOnLayoutChangeListener { root, _, _, _, _, _, _, _, _ ->
                    cardBottom = maxOf(cardBottom, card.bottom)
                    val clear = android.graphics.RectF(
                        0f, (cardBottom + dp(16)).toFloat(),
                        root.width.toFloat(), (controls.top + controls.paddingTop).toFloat(),
                    )
                    if (clear != v.guideClear) v.guideClear = clear
                }
            }
        })
        return v
    }

    /**
     * "2.2 MB · 34 KB/s · 0:41" once the file is known, so the person can see it is moving and how
     * fast. The speed is native's (TransferRate): verified-file bytes per second over the last few
     * seconds, smoothed so it moves with the transfer rather than with every burst. Left out while
     * nothing is arriving -- the card already says the transfer is paused.
     */
    private fun detailLine(s: LiveStatus, elapsedMs: Long): String? {
        if (s.fileSize <= 0 || s.phase.terminal) return null
        val secs = elapsedMs / 1000
        val clock = "${secs / 60}:${"%02d".format(secs % 60)}"
        val rate = if (s.bytesPerSecond > 0.0) " · ${formatRate(s.bytesPerSecond)}" else ""
        return "${TransmitActivity.formatBytes(s.fileSize)}$rate · $clock"
    }

    /**
     * Precision that holds still: whole KB/s from 10 up, so the last digit is not the one that
     * changes every frame. Below 10, one decimal, because 3 and 3.4 are a real difference there.
     */
    private fun formatRate(bytesPerSecond: Double): String {
        val kb = bytesPerSecond / 1024.0
        return when {
            kb >= 1000.0 -> "%.1f MB/s".format(kb / 1024.0)
            kb >= 10.0 -> "%.0f KB/s".format(kb)
            else -> "%.1f KB/s".format(kb)
        }
    }

    /** When the detail line was last redrawn: statuses arrive per decoded frame, 50 a second. */
    private var detailShownMs = 0L
    private var thermalListener: android.os.PowerManager.OnThermalStatusChangedListener? = null

    /** Close, details and help, along the bottom of the receive screen. */
    private fun liveControls(v: AimView): View {
        fun round(icon: Int, description: String, onClick: (View) -> Unit) =
            com.google.android.material.button.MaterialButton(
                this, null, com.google.android.material.R.attr.materialIconButtonFilledTonalStyle,
            ).apply {
                setIconResource(icon)
                iconSize = dp(24)
                contentDescription = description
                setOnClickListener(onClick)
            }
        v.details = false
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(round(R.drawable.ic_close, "Stop receiving") { finish() })
            addView(View(context), LinearLayout.LayoutParams(0, 1, 1f))
            addView(round(R.drawable.ic_info, "Show details") { b ->
                v.details = !v.details
                v.invalidate()
                b.alpha = if (v.details) 1f else 0.7f
            }.apply { alpha = 0.7f })
            addView(View(context), LinearLayout.LayoutParams(dp(12), 1))
            addView(round(R.drawable.ic_help, "Tips") {
                showHelp("Tips", RECEIVE_TIPS)
            })
        }
    }

    private var statusCard: StatusCard? = null

    private fun startAiming() {
        val cols = intent.getIntExtra("gridCols", 120)
        val rows = intent.getIntExtra("gridRows", 260)
        val v = buildViewfinder(cols, rows)
        v.setBanner("Lining up — recording starts when this is steady")
        // No surface to wait for: the preview is painted from the frames the analyser already
        // reads, so aiming starts straight away.
        if (aimThread == null) runAiming(cols, rows, v)
    }

    private fun runAiming(cols: Int, rows: Int, v: AimView) {
        val analyser = AimAnalyser()
        val rec = CameraRecorder(this)
        aimRecorder = rec
        var readyStreak = 0
        var lastMs = 0L
        var handedOver = false

        aimThread = Thread {
            rec.record(
                bundleDir = "${filesDir.absolutePath}/aim-discard",
                frameCount = 0,
                targetFps = 30,
                maxWidth = intent.getIntExtra("maxWidth", 1920),
                notes = "aiming — not a dataset",
                writeFrames = false,
                rig = CameraRecorder.RigMetadata(gridCols = cols, gridRows = rows),
                onFrame = { buf, w, h, stride ->
                    // The schematic has to be drawn in DISPLAY space, and the rotation that puts it
                    // there is known only once the camera is open (F38).
                    if (v.sensorRotation != rec.sensorOrientation) {
                        v.sensorRotation = rec.sensorOrientation
                    }
                    // Paint the preview from this frame, then register the overlay to the
                    // rectangle the image actually occupies.
                    previewView?.let { pv ->
                        pv.submit(buf, w, h, stride, rec.sensorOrientation)
                        runOnUiThread { v.contentRect = pv.contentRect() }
                    }
                    val now = System.currentTimeMillis()
                    if (!handedOver && now - lastMs >= AIM_INTERVAL_MS) {
                        lastMs = now
                        val aim = analyser.analyse(buf, w, h, stride, cols, rows)
                        if (aim != null) {
                            // Require a STREAK rather than a single good frame. One Ready verdict
                            // between two bad ones is exactly what the Ready/TooDark oscillation
                            // looks like when the exposure window lands in the panel's blanking
                            // interval, and starting a capture on it would record the bad half.
                            readyStreak = if (aim.verdict == AimVerdict.Ready) readyStreak + 1 else 0
                            val left = (READY_STREAK - readyStreak).coerceAtLeast(0)
                            runOnUiThread {
                                v.update(aim, w, h)
                                v.setBanner(
                                    if (aim.verdict == AimVerdict.Ready) {
                                        "Steady — starting in $left…"
                                    } else "Lining up — ${if (live) "receiving" else "recording"} starts when this is steady"
                                )
                            }
                            // Freeze the focus the aiming phase converged on, so the recording
                            // does not keep hunting and change the blur mid-dataset.
                            if (readyStreak >= READY_STREAK && lockedFocus < 0f) {
                                lockedFocus = rec.lastFocusDiopters
                            }
                            if (readyStreak >= READY_STREAK) {
                                handedOver = true
                                rec.cancel()
                            }
                        }
                    }
                },
            )
            if (handedOver) {
                runOnUiThread {
                    aimView?.setBanner(if (live) null else "Recording…")
                    start()
                }
            }
        }.also { it.start() }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_CAMERA &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            begin()
        } else {
            showReport("Camera permission denied — nothing can be measured without it.")
        }
    }

    private fun start() {
        if (intent.getBooleanExtra("live", false)) {
            startLive()
            return
        }
        val frames = intent.getIntExtra("frames", 300)
        val fps = intent.getIntExtra("fps", 60)
        val maxWidth = intent.getIntExtra("maxWidth", 1920)
        val notes = intent.getStringExtra("notes")
            ?: "unspecified conditions — a capture whose conditions were not recorded is not a result"
        // `--ez write false` runs the no-write arm: same camera configuration, no disk I/O. The
        // difference between the arms is what makes a low delivered rate attributable to the
        // writer rather than to the sensor.
        val writeFrames = intent.getBooleanExtra("write", true)
        // Negative = continuous autofocus, which is the right default because it needs no knowledge
        // of the rig. Positive = reciprocal metres, locked (5.0 = 20 cm).
        // An explicit value wins; otherwise use whatever the aiming phase converged on, and only
        // fall back to continuous autofocus if aiming never ran. Locking matters because AF that
        // keeps hunting during a capture changes the optical blur mid-dataset, so frames seconds
        // apart stop being comparable -- and comparability is the entire point of a recorded bundle.
        val requestedFocus = intent.getFloatExtra("focusDiopters", -1f)
        val focusDiopters = if (requestedFocus >= 0f) requestedFocus else lockedFocus
        // 0 = derive from the frame period. Exposed so EXP-004/EXP-005 can sweep one axis at a time.
        val exposureNs = intent.getLongExtra("exposureNs", 0L)
        val iso = intent.getIntExtra("iso", CameraRecorder.DEFAULT_ISO)

        // Transmitter and rig facts, passed in because the receiver cannot discover them. `ffreplay`
        // refuses a bundle with no grid (F29), so `gridCols`/`gridRows` are what make a capture
        // decodable at all.
        val rig = CameraRecorder.RigMetadata(
            senderModel = intent.getStringExtra("senderModel") ?: "",
            displayMode = intent.getStringExtra("displayMode") ?: "",
            gridCols = intent.getIntExtra("gridCols", 0),
            gridRows = intent.getIntExtra("gridRows", 0),
            modulationProfile = intent.getStringExtra("profile") ?: "",
            screenBrightness = intent.getDoubleExtra("brightness", -1.0),
            distanceCm = intent.getDoubleExtra("distanceCm", -1.0),
            angleDeg = intent.getDoubleExtra("angleDeg", -1.0),
            ambientLux = intent.getDoubleExtra("ambientLux", -1.0),
            motionCondition = intent.getStringExtra("motion") ?: "",
            payloadSha256 = intent.getStringExtra("payloadSha256") ?: "",
            payloadBytes = intent.getLongExtra("payloadBytes", 0L),
        )

        rigEcho = if (rig.gridCols > 0 && rig.gridRows > 0) {
            "${rig.gridCols}x${rig.gridRows} from ${rig.senderModel.ifEmpty { "(sender unrecorded)" }}"
        } else {
            "(unset — ffreplay will refuse this bundle)"
        }
        aimView?.setBanner("Recording $frames frames…")
        if (aimView == null) {
            showReport("Capturing $frames frames at $fps fps (max width $maxWidth)…")
        }

        // The viewfinder stays LIVE through the recording.
        //
        // The aiming phase tears its camera session down and this one opens a fresh one, and the
        // preview used to stop dead at that handover: the operator lined the phones up, the picture
        // froze, and it stayed frozen for the whole capture until the report replaced it. Holding
        // two phones still for tens of seconds is exactly when someone needs to see that they are
        // STILL lined up, and a frozen frame is worse than no preview at all -- it looks like a
        // live one that is going well, so drift is invisible until the decode fails (F46).
        val liveAnalyser = AimAnalyser()
        val gc = intent.getIntExtra("gridCols", 120)
        val gr = intent.getIntExtra("gridRows", 260)
        var lastAimMs = 0L
        var seen = 0

        // Off the main thread: the capture blocks, and a frozen UI thread would also stall the
        // callbacks it is waiting for.
        Thread {
            val dir = File(filesDir, BUNDLE_DIR)
            dir.deleteRecursively()
            val rec = CameraRecorder(this)
            val outcome = rec.record(
                bundleDir = dir.absolutePath,
                frameCount = frames,
                targetFps = fps,
                maxWidth = maxWidth,
                notes = notes,
                writeFrames = writeFrames,
                focusDiopters = focusDiopters,
                exposureNs = exposureNs,
                iso = iso,
                rig = rig,
                onFrame = { buf, w, h, stride ->
                    // Repainting is throttled inside the view, so this is a cheap call per frame.
                    previewView?.submit(buf, w, h, stride, rec.sensorOrientation)
                    seen++
                    // Guidance as well, but far slower than the aiming phase used. The writer is
                    // the binding constraint on this path -- 32 fps with writes on against 59 with
                    // them off (F28) -- so analysis here competes directly with the thing being
                    // measured. Twice a second is enough to notice drift and correct it, and cheap
                    // enough not to change what the capture is measuring.
                    val now = System.currentTimeMillis()
                    if (now - lastAimMs >= RECORDING_AIM_INTERVAL_MS) {
                        lastAimMs = now
                        val aim = liveAnalyser.analyse(buf, w, h, stride, gc, gr)
                        val n = seen
                        runOnUiThread {
                            aimView?.let { av ->
                                if (aim != null) av.update(aim, w, h)
                                previewView?.let { av.contentRect = it.contentRect() }
                                av.setBanner(
                                    if (aim != null && aim.verdict != AimVerdict.Ready) {
                                        "Recording $n/$frames — ${aim.guidance}"
                                    } else "Recording $n/$frames — hold steady"
                                )
                            }
                        }
                    }
                },
            )
            val report = format(outcome, dir)
            runOnUiThread { showReport(report) }
            emit(report, outcome)
        }.start()
    }

    /**
     * Receive a file LIVE: decode on the phone as frames arrive, until it verifies (C13b).
     *
     * The recording path above is for experiments — a fixed number of frames written to disk and
     * decoded later on a desktop. Someone receiving a file needs the opposite: no disk writes (the
     * writer is the binding constraint, F28), no fixed length (a transfer takes as long as it
     * takes), and a running answer to "is it working". The status card shows progress that never
     * goes backwards and one instruction at a time; when the phone drifts off the screen it says the
     * transfer is PAUSED, because it is — fountain coding keeps everything already received.
     *
     * The camera runs CONTINUOUS autofocus throughout. The recording path locks focus because a
     * dataset needs constant optics; a hand-held receiver needs the opposite, because at 15 cm the
     * depth of field is a few millimetres and a hand moves more than that (F57). Three things back
     * autofocus up: a full scan when the screen is in view but nothing has read for a while, a scan
     * wherever the person taps, and saying so when the lens is pinned at its closest distance --
     * which no amount of waiting fixes.
     */
    private fun startLive() {
        val maxWidth = intent.getIntExtra("maxWidth", 4096)
        // 0 = choose: 60 fps where this camera offers it at near-full resolution, else 30 (F60).
        val fps = intent.getIntExtra("fps", 0).takeIf { it > 0 }
            ?: CameraRecorder.Modes.chooseFps(this, maxWidth)
        // 4 ms. A capture is torn when a display state changes during its exposure plus readout,
        // so every millisecond cut widens the window in which a state is caught whole (F60). At
        // 8 ms the exposure servo still sat at the sensor's floor, ISO 30, on the 2.2 MB transfer:
        // the light was never the limit. Never longer than a frame period.
        val exposureNs = minOf(intent.getLongExtra("exposureNs", 4_000_000L), 1_000_000_000L / fps)
        SessionLog.start(this, "receive")
        SessionLog.i(LIVE_TAG, "live receive: $fps fps, exposure ${exposureNs / 1e6} ms, " +
            "${LiveDecoder.DEFAULT_LANES} decode lanes")
        val iso = intent.getIntExtra("iso", CameraRecorder.DEFAULT_ISO)
        // Negative = continuous autofocus. Only a script asks for a fixed distance.
        val focusDiopters = intent.getFloatExtra("focusDiopters", -1f)
        // The grid chosen on this phone is only tried FIRST; the decoder tries every grid a sender
        // can use, and keeps the one whose headers decode.
        val preferred = intent.getIntExtra("gridCols", 120) to intent.getIntExtra("gridRows", 260)
        val grids = listOf(preferred) + Grids.ALL.filter { it != preferred }

        val v = buildViewfinder(preferred.first, preferred.second)
        v.showGuidance = false
        v.showFrameGuide = true
        val decoder = LiveDecoder(grids, exposureNs)
        liveDecoder = decoder
        statusCard?.set("Fill the picture with the sending screen.", 0f, LIVE_OK)
        val started = SystemClock.elapsedRealtime()
        val rec = CameraRecorder(this)
        liveRecorder = rec
        val coach = LiveCoach(rec, exposureNs)

        // Tap the picture to focus there.
        previewView?.setOnTouchListener { view, ev ->
            if (ev.action == android.view.MotionEvent.ACTION_UP) {
                val p = toSensor(ev.x, ev.y, rec.sensorOrientation)
                if (p != null && rec.focusAt(p.first, p.second)) {
                    coach.focusRequested()
                    SessionLog.i(LIVE_TAG, "tap to focus at sensor (%.2f, %.2f)".format(p.first, p.second))
                }
                view.performClick()
            }
            true
        }

        // Thermal guard (OQ-036): fewer decode lanes as the phone heats, all of them back once it
        // cools. Measured on nothing yet -- the thresholds are the platform's own.
        val power = getSystemService(android.os.PowerManager::class.java)
        val full = LiveDecoder.DEFAULT_LANES
        val thermal = android.os.PowerManager.OnThermalStatusChangedListener { status ->
            val lanes = when {
                status >= android.os.PowerManager.THERMAL_STATUS_SEVERE -> 1
                status >= android.os.PowerManager.THERMAL_STATUS_MODERATE -> (full / 2).coerceAtLeast(1)
                else -> full
            }
            if (lanes != decoder.activeLanes) {
                SessionLog.i(LIVE_TAG, "thermal status $status: decoding on $lanes of $full lanes")
                decoder.activeLanes = lanes
            }
        }
        power?.addThermalStatusListener(mainExecutor, thermal)
        thermalListener = thermal

        decoder.onStatus = { s ->
            val shown = coach.message(s)
            runOnUiThread {
                aimView?.let { av ->
                    if (s.gridKnown) av.setGrid(s.cols, s.rows)
                    av.locked = s.screenInView
                }
                statusCard?.set(shown.text, s.progress.toFloat(), shown.colour)
                val now = SystemClock.elapsedRealtime()
                if (now - detailShownMs >= DETAIL_EVERY_MS || s.phase.terminal) {
                    detailShownMs = now
                    statusCard?.setDetail(detailLine(s, now - started))
                }
                if (s.phase.terminal) finishLive(s, SystemClock.elapsedRealtime() - started)
            }
        }

        val liveAnalyser = AimAnalyser()
        var lastAimMs = 0L
        Thread {
            rec.record(
                bundleDir = "${filesDir.absolutePath}/live-discard",
                frameCount = 0,  // until the file verifies or the person leaves
                targetFps = fps,
                maxWidth = maxWidth,
                notes = "live receive — not a dataset",
                writeFrames = false,
                focusDiopters = focusDiopters,
                exposureNs = exposureNs,
                iso = iso,
                rig = CameraRecorder.RigMetadata(gridCols = preferred.first, gridRows = preferred.second),
                onFrame = { buf, w, h, stride ->
                    if (aimView?.sensorRotation != rec.sensorOrientation) {
                        aimView?.sensorRotation = rec.sensorOrientation
                    }
                    previewView?.submit(buf, w, h, stride, rec.sensorOrientation)
                    decoder.offer(buf, w, h, stride, SystemClock.elapsedRealtimeNanos(), rec.frameChroma)
                    // Aim analysis drives the coaching, exposure and refocus decisions, and the
                    // coverage map. It runs on this thread and costs a pass over the frame, so a
                    // couple of times a second rather than per frame.
                    val now = System.currentTimeMillis()
                    if (now - lastAimMs >= LIVE_AIM_INTERVAL_MS) {
                        lastAimMs = now
                        val grid = coach.grid
                        val aim = liveAnalyser.analyse(buf, w, h, stride, grid.first, grid.second)
                        coach.onAim(aim)
                        val fact = coach.cameraFact()
                        runOnUiThread {
                            aimView?.let { av ->
                                av.extraFact = fact
                                if (aim != null) av.update(aim, w, h)
                                previewView?.let { av.contentRect = it.contentRect() }
                            }
                        }
                    }
                },
            )
        }.start()
    }

    /**
     * Normalised SENSOR coordinates for a touch on the preview, or null outside the picture.
     *
     * The preview shows the sensor frame turned clockwise by the sensor orientation; this undoes
     * that. Same convention as `AimView.rotatePoint`, inverted.
     */
    private fun toSensor(x: Float, y: Float, rotation: Int): Pair<Float, Float>? {
        val r = previewView?.contentRect() ?: return null
        if (r.width() <= 0f || r.height() <= 0f || !r.contains(x, y)) return null
        val u = (x - r.left) / r.width()
        val v = (y - r.top) / r.height()
        return when (rotation) {
            90 -> v to 1f - u
            180 -> 1f - u to 1f - v
            270 -> 1f - v to u
            else -> u to v
        }
    }

    /** One sentence and its colour, for the top card. */
    private data class Shown(val text: String, val colour: Int)

    /**
     * Decides what to TELL the person, and does the camera housekeeping a person cannot.
     *
     * The decoder's own message is right whenever frames are being read: progress, "paused",
     * "hold steadier" (C13b). When NOTHING reads, the decoder can only say "not readable", and the
     * aim analysis and the camera know why: the lens is at its closest focus (move back), the
     * picture is soft (refocusing), the screen is too small in the view (move closer), or it is
     * clipped. That is the case the first hand-held test hit, with no explanation on screen (F57).
     *
     * Also runs, without being asked:
     *  - exposure: steers sensitivity so the bright level sits below clipping and above a floor,
     *    whatever the sending panel's brightness. Two phones' panels differ by several
     *    times in brightness, and one fixed ISO cannot suit both (C05);
     *  - refocus: a full AF scan when the screen is in view and nothing has read for a while.
     */
    private inner class LiveCoach(
        private val rec: CameraRecorder,
        private val baseExposureNs: Long,
    ) {
        @Volatile private var aim: dev.fileflow.aim.Aim? = null
        @Volatile private var lastReadMs = SystemClock.elapsedRealtime()
        @Volatile private var lastRefocusMs = SystemClock.elapsedRealtime()
        @Volatile private var lastShown = ""
        @Volatile var grid: Pair<Int, Int> =
            intent.getIntExtra("gridCols", 120) to intent.getIntExtra("gridRows", 260)
            private set

        fun focusRequested() {
            lastRefocusMs = SystemClock.elapsedRealtime()
        }

        /** Camera thread, a couple of times a second. */
        fun onAim(a: dev.fileflow.aim.Aim?) {
            aim = a
            val now = SystemClock.elapsedRealtime()
            // Exposure is steered ONLY on a screen the decoder has verified (its markers matched).
            // Steering on whatever the analyser calls a screen meant tuning ISO to the room: on
            // the first run it climbed while pointed at a ceiling, and would have arrived at the
            // real screen saturated and taken seconds to come back (F57). With no screen for a
            // while it returns to where it started, which is known to suit a lit panel (F36).
            if (now - screenSeenMs < STEER_WINDOW_MS) {
                if (a != null) steerExposure(a)
            } else if (now - screenSeenMs > RELAX_AFTER_MS) {
                rec.setIso(baseIso)
                rec.setExposure(baseExposureNs)
            }
            if (a == null || a.verdict == AimVerdict.NoScreenFound) return
            if (now - lastReadMs > REFOCUS_AFTER_MS && now - lastRefocusMs > REFOCUS_EVERY_MS &&
                !pinnedAtClosest()
            ) {
                lastRefocusMs = now
                if (rec.refocus()) SessionLog.i(LIVE_TAG, "nothing read for ${now - lastReadMs} ms: refocusing")
            }
        }

        /**
         * One step towards the target band. Darker: ISO first, then a shorter exposure once ISO is
         * at its floor (which also cuts blur). Brighter: give back exposure time first, up to what
         * was asked for, then ISO -- so a dim panel costs noise, never extra motion blur.
         */
        private fun steerExposure(a: dev.fileflow.aim.Aim) {
            val iso = rec.iso
            val exp = rec.exposureNs
            if (exp <= 0L) return  // no manual control on this camera
            // On the BRIGHT level only. A raised dark level was also treated as overexposure, and on
            // the first hand-held transfer it drove ISO from 60 to 9, the sensor's floor -- but
            // the dark cells were lifted by defocus at 12 cm, not by light, and less light does
            // not fix blur (F59).
            val tooBright = a.brightLevel > BRIGHT_CEILING
            val tooDark = a.brightLevel < BRIGHT_FLOOR
            when {
                tooBright && iso > rec.isoRange.lower -> rec.setIso((iso * 0.8).toInt())
                tooBright -> rec.setExposure((exp * 0.8).toLong().coerceAtLeast(MIN_EXPOSURE_NS))
                tooDark && exp < baseExposureNs -> rec.setExposure(minOf((exp * 1.25).toLong(), baseExposureNs))
                tooDark -> rec.setIso((iso * 1.25).toInt() + 1)
                else -> return
            }
            if (rec.iso != iso || rec.exposureNs != exp) {
                SessionLog.i(LIVE_TAG, "exposure: levels %.0f/%.0f, ISO %d -> %d, %.1f -> %.1f ms".format(
                    a.darkLevel, a.brightLevel, iso, rec.iso, exp / 1e6, rec.exposureNs / 1e6))
            }
        }

        private val baseIso = intent.getIntExtra("iso", CameraRecorder.DEFAULT_ISO)
        @Volatile private var screenSeenMs = 0L

        /** The lens is as close as it goes: AF cannot help, only moving back can. */
        private fun pinnedAtClosest(): Boolean {
            val min = rec.minFocusDiopters
            return min > 0f && rec.lastFocusDiopters >= min * 0.97f
        }

        fun cameraFact(): String {
            val d = rec.lastFocusDiopters
            val focus = when {
                rec.minFocusDiopters <= 0f -> "fixed"
                d > 0f -> "%.0f cm".format(100f / d)
                d == 0f -> "far"
                else -> "?"
            }
            val skew = rec.rollingShutterSkewNs
            val lens = when (rec.distortionCorrection) {
                -1 -> ""
                0 -> "    undistort off"
                else -> "    undistort on"
            }
            return "focus $focus    ISO ${rec.iso}    exp ${"%.1f".format(rec.exposureNs / 1e6)} ms" +
                (if (skew > 0) "    skew ${"%.1f".format(skew / 1e6)} ms" else "") + lens
        }

        /**
         * Decode-lane threads, per decoded frame -- several at once, and not necessarily in order,
         * so serialised here, and a status older than one already shown is answered with what is
         * already showing rather than allowed to move the card backwards (F60).
         */
        @Synchronized
        fun message(s: LiveStatus): Shown {
            val now = SystemClock.elapsedRealtime()
            val held0 = shown
            if (held0 != null && !s.phase.terminal && s.gridKnown == lastGridKnown &&
                s.frames < lastFrames
            ) {
                return held0
            }
            lastFrames = s.frames
            lastGridKnown = s.gridKnown
            if (s.gridKnown) grid = s.cols to s.rows
            if (s.recentReadFraction > 0.0) lastReadMs = now
            if (s.screenInView) screenSeenMs = now
            if (s.payloadFrames > lastPayloadFrames) {
                lastPayloadFrames = s.payloadFrames
                lastPayloadMs = now
            }
            // PRODUCTIVE means data arriving, not merely headers being read. On the first
            // hand-held transfer the headers read on every frame at 12 cm while almost no payload
            // did, and the decoder's own "reading" message hid that for 2.5 minutes (F59).
            val productive = now - lastPayloadMs < PRODUCTIVE_WINDOW_MS
            val decoderSays = "decoder:${s.phase}/${s.hint}" to Shown(s.message, colourFor(s))
            val (kind, wanted) = if (productive || s.phase.terminal) decoderSays
                                 else diagnose(s) ?: decoderSays

            // Hold a message for a moment before replacing it with a different KIND of message.
            // Without this the card changed several times a second -- "reading", "not readable",
            // "point the camera", "too close" -- which is what "it goes in and out" looked like to
            // the person holding it (F59). The same kind of message (progress ticking up) updates
            // freely, and the end of the transfer is never held back.
            val held = shown
            val out = if (held == null || kind == shownKind || s.phase.terminal ||
                now - shownSinceMs >= MIN_MESSAGE_MS
            ) {
                if (kind != shownKind) {
                    shownKind = kind
                    shownSinceMs = now
                }
                shown = wanted
                wanted
            } else held
            if (out.text != lastShown) {
                lastShown = out.text
                SessionLog.i(LIVE_TAG, "status: ${s.phase}/${s.hint} ${"%.0f".format(s.progress * 100)}% " +
                    "payloads=${s.payloadFrames} aim=${aim?.verdict} " +
                    "px/cell=${"%.1f".format(aim?.pxPerCell ?: 0.0)} ${cameraFact()} " +
                    "grid=${s.cols}x${s.rows}${if (s.gridKnown) "" else "?"} -> \"${out.text}\"")
            }
            return out
        }

        private var lastFrames = 0L
        private var lastGridKnown = false
        @Volatile private var lastPayloadMs = 0L
        private var lastPayloadFrames = 0L
        private var shown: Shown? = null
        private var shownKind = ""
        private var shownSinceMs = 0L

        /** Why nothing is arriving, when the camera and the aim analysis can tell. */
        private fun diagnose(s: LiveStatus): Pair<String, Shown>? {
            val a = aim ?: return null
            val progress = if (s.progress > 0.0) " Nothing received so far is lost." else ""
            return when {
                a.verdict == AimVerdict.NoScreenFound -> null  // the decoder's "point at" is right
                // "Too close" is now only the lens's own limit. It also used to fire under 15 cm
                // on the strength of F59, whose 12 cm failure was the exposure servo driving ISO
                // to the floor, not distance: the 2.2 MB transfer ran fastest at 11 cm, filling
                // the picture.
                pinnedAtClosest() -> "too-close" to Shown(
                    "Too close to focus — move back a little.$progress", LIVE_WARN,
                )
                a.verdict == AimVerdict.Clipped -> "clipped" to Shown(
                    "Move back a little — part of the screen is outside the picture.$progress",
                    LIVE_WARN,
                )
                a.pxPerCell in 0.1..MIN_PX_PER_CELL -> "too-far" to Shown(
                    "Move closer — fill the picture with the screen.$progress",
                    LIVE_WARN,
                )
                a.verdict == AimVerdict.Blurred -> "blurred" to
                    Shown("Focusing — hold still, or tap the screen in the picture.$progress", LIVE_WARN)
                a.verdict == AimVerdict.TooDark || a.verdict == AimVerdict.TooBright -> "exposure" to
                    Shown("Adjusting to the screen's brightness…$progress", LIVE_WARN)
                else -> null
            }
        }
    }

    private var liveRecorder: CameraRecorder? = null
    private var liveDecoder: LiveDecoder? = null
    private var liveFinished = false

    private fun colourFor(s: LiveStatus): Int = when {
        s.phase == LivePhase.Failed -> LIVE_BAD
        s.hint != LiveHint.None -> LIVE_WARN
        else -> LIVE_OK
    }

    private fun finishLive(s: LiveStatus, elapsedMs: Long) {
        if (liveFinished) return
        liveFinished = true
        liveRecorder?.cancel()
        val decoder = liveDecoder ?: return
        // Off the UI thread: the file can be megabytes.
        Thread {
            val saved = if (s.phase == LivePhase.Verified) {
                runCatching { decoder.saveVerified(this) }.getOrNull()
            } else null
            val seconds = elapsedMs / 1000.0
            val report = buildString {
                if (s.phase == LivePhase.Verified && saved != null) {
                    appendLine("RECEIVED — verified")
                    appendLine()
                    appendLine("  file        ${saved.name}")
                    appendLine("  size        ${TransmitActivity.formatBytes(saved.bytes)}")
                    appendLine("  integrity   SHA-256 matched the sender's")
                    appendLine("  time        ${"%.1f".format(seconds)} s")
                    appendLine("  goodput     ${"%.1f".format(saved.bytes / 1024.0 / seconds)} KB/s" +
                               "  (wall clock, from Receive)")
                    appendLine("  grid        ${s.cols}x${s.rows}")
                    appendLine()
                    appendLine("  saved to    ${saved.location}")
                } else if (s.phase == LivePhase.Verified) {
                    appendLine("RECEIVED and verified, but it could not be saved to storage.")
                } else {
                    appendLine("TRANSFER FAILED")
                    appendLine()
                    appendLine(s.message)
                }
                appendLine()
                appendLine("  frames read ${s.frames}")
            }
            report.lineSequence().forEach { SessionLog.i(LOG_TAG, it) }
            runOnUiThread { showResult(report, saved, s, seconds) }
        }.start()
    }

    /**
     * The end of a transfer: what arrived, and what to do with it next.
     *
     * One icon, the file's name, one line of numbers, and the actions. The full report -- grid,
     * frames, the exact goodput definition -- is behind the info button, not on the screen.
     */
    private fun showResult(report: String, saved: LiveDecoder.SavedFile?, s: LiveStatus, seconds: Double) {
        text.text = report
        statusCard = null
        aimView = null
        val ok = saved != null
        val uri = saved?.uri
        // Felt as well as seen: the phone is often still held up at the sender when it finishes.
        window.decorView.performHapticFeedback(
            if (ok) android.view.HapticFeedbackConstants.CONFIRM
            else android.view.HapticFeedbackConstants.REJECT,
        )
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(24), dp(48), dp(24), dp(24))
        }
        col.addView(android.widget.ImageView(this).apply {
            setImageResource(if (ok) R.drawable.ic_check_circle else R.drawable.ic_error)
            imageTintList = android.content.res.ColorStateList.valueOf(
                getColor(if (ok) R.color.ff_ok else R.color.ff_bad),
            )
        }, LinearLayout.LayoutParams(dp(88), dp(88)))
        col.addView(space(16))
        col.addView(title(
            when {
                ok -> "Received"
                s.phase == LivePhase.Verified -> "Couldn't save the file"
                else -> "Transfer failed"
            },
            28f,
        ).apply { gravity = Gravity.CENTER })
        col.addView(space(8))
        if (saved != null) {
            col.addView(text(saved.name, 18f, com.google.android.material.R.attr.colorOnSurface, bold = true)
                .apply { gravity = Gravity.CENTER })
            col.addView(space(4))
            val rate = saved.bytes / 1024.0 / seconds
            col.addView(text(
                "${TransmitActivity.formatBytes(saved.bytes)} · ${"%.1f".format(seconds)} s · " +
                    "${"%.1f".format(rate)} KB/s",
            ).apply { gravity = Gravity.CENTER })
            col.addView(text("Verified · saved to ${saved.location.substringBeforeLast('/')}", 13f)
                .apply { gravity = Gravity.CENTER; setPadding(0, dp(6), 0, 0) })
        } else {
            col.addView(text(s.message).apply { gravity = Gravity.CENTER })
        }
        col.addView(space(32))

        if (uri != null && saved != null) {
            val mime = contentResolver.getType(uri) ?: "application/octet-stream"
            col.addView(button("Open", R.drawable.ic_open) {
                val view = android.content.Intent(android.content.Intent.ACTION_VIEW)
                    .setDataAndType(uri, mime)
                    .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                runCatching { startActivity(android.content.Intent.createChooser(view, saved.name)) }
                    .onFailure {
                        android.widget.Toast.makeText(
                            this, "No app here opens this kind of file.", android.widget.Toast.LENGTH_LONG,
                        ).show()
                    }
            }, matchWidth())
            col.addView(button("Share", R.drawable.ic_share, Style.Tonal) {
                val send = android.content.Intent(android.content.Intent.ACTION_SEND)
                    .setType(mime)
                    .putExtra(android.content.Intent.EXTRA_STREAM, uri)
                    .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                runCatching { startActivity(android.content.Intent.createChooser(send, saved.name)) }
            }, matchWidth(dp(8)))
        }
        col.addView(button("Receive another", R.drawable.ic_camera, Style.Tonal) { recreate() },
            matchWidth(dp(8)))
        col.addView(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            addView(button("Details", R.drawable.ic_info, Style.Text) { showHelp("Details", report) })
            addView(button("Done", style = Style.Text) { finish() })
        }, matchWidth(dp(8)))

        setContentView(ScrollView(this).apply {
            setBackgroundColor(getColor(R.color.ff_background))
            isFillViewport = true
            addView(col)
        }.padForSystemBars())
    }
    private var rigEcho: String = "(unset — ffreplay will refuse this bundle)"

    private fun format(o: CameraRecorder.Outcome, dir: File): String = buildString {
        appendLine("FileFlow capture run (C05 CPU path) — EXP-007 verification half")
        appendLine("=".repeat(64))
        appendLine()
        appendLine("PROVENANCE")
        appendLine("  collected        ${java.util.Date()}")
        appendLine("  device           ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
        appendLine("  fingerprint      ${android.os.Build.FINGERPRINT}")
        appendLine("  bundle           ${dir.absolutePath}")
        appendLine()

        appendLine("REQUESTED")
        appendLine("  grid (from TX)   ${rigEcho}")
        appendLine("  camera           ${o.cameraId}")
        appendLine("  size             ${o.size.width}x${o.size.height}")
        appendLine("  fps              ${o.requestedFps}")
        appendLine()

        appendLine("DELIVERED  <-- the numbers that matter")
        appendLine("  disk writes      ${if (o.framesWereWritten) "ON" else "OFF (A/B arm: camera rate without writer I/O)"}")
        appendLine("  frames           ${o.framesWritten}")
        appendLine("  delivered rate   ${"%.2f".format(o.deliveredFps)} fps (from sensor timestamps)")
        // Never print an unmeasured quantity as a number. The no-write arm does not write, and
        // duplicate detection lives in the writer, so there is nothing to report -- and "0"
        // would read as "no duplicates found", which is a different and much stronger claim.
        val dupFrac = o.duplicateFraction
        if (dupFrac == null) {
            appendLine("  duplicate frames NOT MEASURED (no writes this arm — not the same as zero)")
            appendLine("  DISTINCT rate    NOT MEASURED")
        } else {
            appendLine("  duplicate frames ${o.duplicateFrames}  (${"%.4f".format(dupFrac)})")
            appendLine("  DISTINCT rate    ${"%.2f".format(o.distinctFps ?: 0.0)} fps  <-- bounds Fd, not the above")
        }
        appendLine("  worst gap        ${"%.2f".format(o.worstGapFrames)} frame periods")
        appendLine()

        appendLine("MANUAL CONTROL — read back from CaptureResult, not from the request (RISK-011)")
        appendLine("  manual requested ${o.manualRequested}")
        appendLine("  exposure         ${o.reportedExposureNs} ns")
        appendLine("  ISO              ${o.reportedIso}")
        appendLine("  focus distance   ${o.reportedFocusDistance} diopters" +
                   if (o.reportedFocusDistance > 0f) {
                       "  => ${"%.1f".format(100f / o.reportedFocusDistance)} cm"
                   } else if (o.reportedFocusDistance == 0f) "  => INFINITY" else "")
        appendLine("  lens min focus   ${o.minFocusDiopters} diopters" +
                   if (o.minFocusDiopters > 0f) {
                       "  => closest ${"%.1f".format(100f / o.minFocusDiopters)} cm"
                   } else "")
        appendLine("  AE mode          ${o.aeMode}  (0 = OFF, i.e. manual honoured)")
        appendLine("  frame duration   ${o.reportedFrameDurationNs} ns" +
                   if (o.reportedFrameDurationNs > 0) {
                       "  => ${"%.2f".format(1e9 / o.reportedFrameDurationNs)} fps ceiling"
                   } else "")
        appendLine("  EDGE_MODE        ${o.edgeMode}  (0 = OFF, requested; OQ-016)")
        appendLine("  NOISE_REDUCTION  ${o.noiseReductionMode}  (0 = OFF, requested; OQ-016)")
        // Requested ON, unlike the two above: the lens's vignette is a smooth multiplicative field
        // the receiver has to fight, and correcting it is the ISP doing something useful (RISK-025).
        appendLine("  SHADING_MODE     ${o.shadingMode}  (2 = HIGH_QUALITY, requested; RISK-025)")
        appendLine()

        if (o.error != null) {
            appendLine("ERROR            ${o.error}")
            appendLine()
        }

        appendLine("-".repeat(64))
        appendLine("A duplicate is a frame BYTE-IDENTICAL to its predecessor. Sensor noise makes")
        appendLine("that decisive: two real exposures of a static scene differ in a large share of")
        appendLine("their bytes, so an identical frame is a repeated buffer. Timestamps cannot")
        appendLine("detect this — repeats still carry distinct ones.")
        appendLine()
        appendLine("This measures DELIVERY, not the optical channel. It says nothing about goodput,")
        appendLine("and Fd remains unmeasured until a transmitter presents known states (EXP-006).")
    }

    private fun emit(report: String, o: CameraRecorder.Outcome) {
        report.lineSequence().forEach { android.util.Log.i(LOG_TAG, it) }
        runCatching { File(filesDir, REPORT_FILE).writeText(report) }

        // Per-frame timestamps as a sibling of the bundle, not inside it: the bundle format is a
        // contract (C17) and this is recorder telemetry, not part of it. Kept because the
        // aggregate rate hides the shape — a steady 59 fps and a 60 fps stream with periodic
        // stalls are different problems.
        runCatching {
            val csv = StringBuilder("index,timestamp_ns,delta_ns\n")
            o.timestampsNs.forEachIndexed { i, t ->
                val d = if (i == 0) 0L else t - o.timestampsNs[i - 1]
                csv.append("$i,$t,$d\n")
            }
            File(filesDir, FRAMES_CSV).writeText(csv.toString())
        }
    }

    /** Swap to the report view. Dark and readable, because it is read on a phone in a room. */
    private fun showReport(s: String) {
        text.text = s
        (text.parent as? android.view.ViewGroup)?.removeView(text)
        setContentView(
            ScrollView(this).apply {
                setBackgroundColor(Color.parseColor("#101418"))
                addView(text)
            }.padForSystemBars()
        )
        aimView = null
    }

    override fun onDestroy() {
        super.onDestroy()
        aimRecorder?.cancel()
        aimThread?.join(2000)
        liveRecorder?.cancel()
        liveDecoder?.close()
        thermalListener?.let {
            getSystemService(android.os.PowerManager::class.java)?.removeThermalStatusListener(it)
        }
        if (liveRecorder != null) SessionLog.close()
    }

    companion object {
        const val LOG_TAG = "FileFlow.Capture"
        const val BUNDLE_DIR = "capture-bundle"
        const val REPORT_FILE = "capture-report.txt"
        const val FRAMES_CSV = "capture-frames.csv"
        private const val REQ_CAMERA = 1
        private const val AIM_INTERVAL_MS = 150L
        /** Guidance cadence WHILE recording. Slower than aiming: see the note in [start]. */
        private const val RECORDING_AIM_INTERVAL_MS = 500L
        /**
         * Aim-analysis cadence while decoding live. It drives exposure and refocus as well as the
         * coverage map, so faster than the old once a second -- but it is a pass over a 12 MP
         * frame on the camera thread, so not per frame.
         */
        private const val LIVE_AIM_INTERVAL_MS = 400L
        const val LIVE_TAG = "FileFlow.Live"
        private const val RECEIVE_TIPS =
"• Fill the picture with the sending screen: its pattern just inside the corner marks. " +
                "That is where it reads fastest.\n\n" +
                "• Keep the two phones parallel.\n\n" +
                "• Tap the picture to refocus.\n\n" +
                "• If it pauses, just aim back: nothing received so far is lost.\n\n" +
                "• The ⓘ button shows what the camera sees, for troubleshooting."
        /** Nothing read for this long with the screen in view: run a full focus scan. */
        private const val REFOCUS_AFTER_MS = 2500L
        /** At most one automatic scan this often, so a scan is never interrupted by the next. */
        private const val REFOCUS_EVERY_MS = 4000L
        /**
         * Exposure target, in 8-bit sensor counts on the screen's own cells: bright cells below
         * clipping, dark cells near black. Inside the band nothing moves, so it does not hunt.
         */
        private const val BRIGHT_CEILING = 235.0
        private const val BRIGHT_FLOOR = 150.0
        /** A payload decoded this recently counts as data arriving. */
        private const val PRODUCTIVE_WINDOW_MS = 2500L
        /** Shortest time a message of one kind stays up before a different kind replaces it. */
        private const val MIN_MESSAGE_MS = 1500L
        /** The speed and clock line: often enough to tick, not so often its digits churn. */
        private const val DETAIL_EVERY_MS = 250L
        /**
         * Fewer camera pixels per cell than this, and not decoding: "move closer". The 2.2 MB
         * transfer crawled at 3.5-4 and ran ten times faster at 7-11, filling the picture.
         */
        private const val MIN_PX_PER_CELL = 5.0
        private const val MIN_EXPOSURE_NS = 2_000_000L
        /** Exposure is steered while the decoder verified the screen this recently... */
        private const val STEER_WINDOW_MS = 1000L
        /** ...and relaxes to its starting point once it has not for this long. */
        private const val RELAX_AFTER_MS = 3000L
        private val LIVE_OK = Color.parseColor("#69F0AE")
        private val LIVE_WARN = Color.parseColor("#FFB74D")
        private val LIVE_BAD = Color.parseColor("#FF5252")
        /** Consecutive Ready verdicts before recording starts. See the note in [startAiming]. */
        private const val READY_STREAK = 6
    }
}
