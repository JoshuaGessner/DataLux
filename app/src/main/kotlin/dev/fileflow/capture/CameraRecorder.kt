package dev.fileflow.capture

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The CPU capture path of C05: opens the camera with locked settings and writes every Y plane
 * into a capture bundle, measuring what actually arrived.
 *
 * WHAT THIS IS FOR. The probe (C02) can only report what the camera *advertises*. This is the
 * first thing that finds out what it *delivers* — the verification half of EXP-007 — and it is the
 * step that makes every analysis tool in the repo apply to real data, because the bundle it writes
 * is the format `ffreplay` already consumes and F17 proved bit-identical to live decode.
 *
 * WHY THE CPU PATH FIRST. `YUV_420_888` through `ImageReader` tops out at 60 fps on the reference
 * device (F27). The ≥120 fps arm needs a constrained high-speed session driven from a
 * `SurfaceTexture`, because such a session **rejects `ImageReader`** `[FACT]` — a different code
 * path, and one whose value depends on the distinctness question this run answers first.
 *
 * WHAT IT DELIBERATELY MEASURES, beyond writing frames:
 *  - **delivered rate**, from sensor timestamps, not from what was requested
 *  - **duplicate fraction**, byte-identical frames (see [Recorder.duplicateFrames])
 *  - **whether the manual settings were honoured**, read back from `CaptureResult` rather than
 *    trusted from the request — RISK-011's whole point
 *
 * Nothing here interprets those numbers. It records them; the decision belongs to C02's policy.
 */
class CameraRecorder(private val context: Context) {

    /**
     * `SENSOR_ORIENTATION` for the camera in use: clockwise degrees to turn a sensor frame into the
     * device's natural display orientation. 90 on both reference devices' rear cameras.
     *
     * Anything that draws camera coordinates on screen needs this. Without it the aim schematic was
     * drawn in SENSOR space while the operator held the phone in DISPLAY space, so moving the phone
     * left moved the box up -- reported, accurately, as impossible to line up with (F38).
     */
    @Volatile
    var sensorOrientation: Int = 0
        private set

    /**
     * Focus distance most recently REPORTED by the camera, in reciprocal metres.
     *
     * Continuous autofocus is right while aiming, because the rig is being moved and the distance is
     * unknown. It is wrong while recording: AF keeps hunting, and every refocus changes the optical
     * blur mid-dataset, so frames captured seconds apart are not comparable. Reading the converged
     * value here lets the recording phase LOCK to it.
     */
    @Volatile
    var lastFocusDiopters: Float = -1f
        private set

    /** `LENS_INFO_MINIMUM_FOCUS_DISTANCE` of the camera in use: the CLOSEST it can focus, diopters. */
    @Volatile
    var minFocusDiopters: Float = 0f
        private set

    /**
     * The U and V planes of a YUV_420_888 frame: half size both ways, sharing one row and pixel
     * stride (the format guarantees it), usually interleaved with each other (pixel stride 2).
     */
    class Chroma(
        val u: java.nio.ByteBuffer,
        val v: java.nio.ByteBuffer,
        val rowStride: Int,
        val pixelStride: Int,
    )

    /**
     * The colour planes of the frame being handed to `onFrame`, valid only DURING that call and
     * only on the camera thread -- the buffers return to the camera as soon as it returns. M5's
     * eight colours are read from these (F62).
     */
    @Volatile
    var frameChroma: Chroma? = null
        private set

    /** `SENSOR_ROLLING_SHUTTER_SKEW` of the latest frame, ns; -1 when the camera does not say. */
    @Volatile
    var rollingShutterSkewNs: Long = -1L
        private set

    /**
     * `DISTORTION_CORRECTION_MODE` the camera APPLIED to the latest frame (0 off, 1 fast, 2 high
     * quality); -1 when it does not report one, which is what a camera without the control does.
     */
    @Volatile
    var distortionCorrection: Int = -1
        private set

    /** Latest `CONTROL_AF_STATE`, or -1 before the first result. */
    @Volatile
    var afState: Int = -1
        private set

    /** Sensitivity currently requested, and the range the sensor accepts. */
    @Volatile
    var iso: Int = DEFAULT_ISO
        private set
    @Volatile
    var isoRange: android.util.Range<Int> = android.util.Range(DEFAULT_ISO, DEFAULT_ISO)
        private set

    /**
     * Run-time control of the repeating request: refocus and exposure, while frames keep flowing.
     *
     * Everything is posted to the camera handler thread, which owns the session and the builder, so
     * callers on any thread never touch either directly.
     */
    private inner class LiveControl(
        val session: CameraCaptureSession,
        val builder: CaptureRequest.Builder,
        val callback: CameraCaptureSession.CaptureCallback,
        val handler: Handler,
        val chars: CameraCharacteristics,
    ) {
        /** Bumped by every refocus, so a stale "return to continuous" from an older one is ignored. */
        var generation = 0
        var scanning = false

        fun repeat() {
            runCatching { session.setRepeatingRequest(builder.build(), callback, handler) }
        }
    }

    @Volatile
    private var control: LiveControl? = null

    /**
     * Run a full focus scan now, weighted on ([nx], [ny]) — normalised SENSOR coordinates, 0..1 —
     * then resume continuous autofocus. Returns false when there is no running session or this
     * camera's focus is fixed.
     *
     * WHY A SCAN AND NOT A TRIGGER IN CONTINUOUS MODE. A trigger in `CONTINUOUS_PICTURE` locks
     * wherever passive focus already is; if it has settled on the wrong distance (the desk behind a
     * phone, or a lens pinned at its closest limit) the trigger just confirms that. `AUTO` plus a
     * trigger always sweeps. Continuous is restored afterwards because a hand-held receiver changes
     * distance constantly and a depth of field of a few millimetres does not survive a lock (F57).
     */
    fun focusAt(nx: Float, ny: Float): Boolean {
        val c = control ?: return false
        if (minFocusDiopters <= 0f) return false  // fixed-focus lens: nothing to do
        c.handler.post {
            if (control !== c) return@post
            val gen = ++c.generation
            c.scanning = true
            afRegion(c.chars, nx, ny, AF_TAP_FRACTION)?.let {
                c.builder.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(it))
            }
            c.builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_AUTO)
            c.builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            c.repeat()
            runCatching {
                c.builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
                c.session.capture(c.builder.build(), c.callback, c.handler)
                c.builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            }
            // A scan that never reports locked must not leave AF switched off for good.
            c.handler.postDelayed({ resumeContinuous(c, gen) }, AF_SCAN_TIMEOUT_MS)
        }
        return true
    }

    /** Refocus on the centre of the view. */
    fun refocus(): Boolean = focusAt(0.5f, 0.5f)

    /** Zoom currently requested; 1 = the whole sensor. */
    @Volatile
    var zoom: Float = 1f
        private set

    /**
     * Zooms by `CONTROL_ZOOM_RATIO`, clamped to what the camera offers and to [MAX_LIVE_ZOOM].
     * Returns false when the camera cannot zoom or is already there.
     */
    fun setZoom(ratio: Float): Boolean {
        val c = control ?: return false
        val range = c.chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) ?: return false
        val clamped = ratio.coerceIn(maxOf(1f, range.lower), minOf(range.upper, MAX_LIVE_ZOOM))
        if (kotlin.math.abs(clamped - zoom) < 0.01f) return false
        zoom = clamped
        c.handler.post {
            if (control !== c) return@post
            c.builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, clamped)
            c.repeat()
        }
        return true
    }

    /** Sets sensor sensitivity, clamped to what the sensor accepts. No-op without manual control. */
    fun setIso(value: Int) {
        val c = control ?: return
        val clamped = value.coerceIn(isoRange.lower, isoRange.upper)
        if (clamped == iso) return
        iso = clamped
        c.handler.post {
            if (control !== c) return@post
            c.builder.set(CaptureRequest.SENSOR_SENSITIVITY, clamped)
            c.repeat()
        }
    }

    /** Exposure currently requested, ns; 0 before a manual session starts. */
    @Volatile
    var exposureNs: Long = 0L
        private set

    /**
     * Sets the exposure time, clamped to the sensor's range and to the frame period. Shorter is
     * the way to go darker once ISO is at its floor -- and it also cuts motion blur.
     */
    fun setExposure(ns: Long) {
        val c = control ?: return
        if (exposureNs <= 0L) return  // not under manual control
        val range = c.chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
        val clamped = ns.coerceIn(range?.lower ?: 1L, minOf(range?.upper ?: ns, maxExposureNs))
        if (clamped == exposureNs) return
        exposureNs = clamped
        c.handler.post {
            if (control !== c) return@post
            c.builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, clamped)
            c.repeat()
        }
    }

    /** The frame period: an exposure longer than this would lower the frame rate. */
    @Volatile
    private var maxExposureNs: Long = Long.MAX_VALUE

    /** Camera thread. Ends a scan once it has locked -- focused or not -- after a short hold. */
    private fun onAfState(state: Int) {
        val c = control ?: return
        if (!c.scanning) return
        if (state == CaptureResult.CONTROL_AF_STATE_FOCUSED_LOCKED ||
            state == CaptureResult.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED
        ) {
            c.scanning = false
            val gen = c.generation
            c.handler.postDelayed({ resumeContinuous(c, gen) }, AF_HOLD_AFTER_SCAN_MS)
        }
    }

    private fun resumeContinuous(c: LiveControl, gen: Int) {
        if (control !== c || c.generation != gen) return
        c.scanning = false
        c.builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
        c.builder.set(
            CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE,
        )
        c.repeat()
    }

    /** The middle of the view, for continuous AF. Null when the camera takes no AF regions. */
    private fun centreAfRegion(chars: CameraCharacteristics) =
        afRegion(chars, 0.5f, 0.5f, AF_CENTRE_FRACTION)

    /**
     * A metering rectangle centred on ([nx], [ny]) in normalised sensor coordinates, [fraction] of
     * the active array on a side.
     *
     * Active-array coordinates are correct here because nothing in this app sets a zoom ratio or a
     * crop region; with either, regions are relative to the zoomed field of view instead.
     */
    private fun afRegion(
        chars: CameraCharacteristics, nx: Float, ny: Float, fraction: Float,
    ): android.hardware.camera2.params.MeteringRectangle? {
        if ((chars.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0) <= 0) return null
        val active = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return null
        val w = (active.width() * fraction).toInt().coerceAtLeast(1)
        val h = (active.height() * fraction).toInt().coerceAtLeast(1)
        val x = (active.width() * nx - w / 2f).toInt().coerceIn(0, active.width() - w)
        val y = (active.height() * ny - h / 2f).toInt().coerceIn(0, active.height() - h)
        return android.hardware.camera2.params.MeteringRectangle(
            x, y, w, h, android.hardware.camera2.params.MeteringRectangle.METERING_WEIGHT_MAX,
        )
    }

    /**
     * Set to stop an unbounded run. Checked on the camera callback thread.
     *
     * `@Volatile` rather than a lock: it is written from the UI thread and read from the camera
     * thread, and a torn read of a boolean is not possible — the only requirement is visibility.
     */
    @Volatile
    private var cancelled = false

    /** Ends a run started with `frameCount <= 0`. Safe to call from any thread. */
    fun cancel() {
        cancelled = true
    }

    /**
     * Facts about the transmitter and the physical rig that the receiver cannot observe.
     *
     * Defaults are the format's **sentinels**, not plausible values: a capture whose distance was
     * never measured must not claim 30 cm (C17).
     */
    data class RigMetadata(
        val senderModel: String = "",
        val displayMode: String = "",
        val gridCols: Int = 0,
        val gridRows: Int = 0,
        val modulationProfile: String = "",
        val screenBrightness: Double = -1.0,
        val distanceCm: Double = -1.0,
        val angleDeg: Double = -1.0,
        val ambientLux: Double = -1.0,
        val motionCondition: String = "",
        val payloadSha256: String = "",
        val payloadBytes: Long = 0L,
    )

    /** Everything one run produced. All of it observed, none of it requested. */
    data class Outcome(
        val cameraId: String,
        val size: Size,
        val requestedFps: Int,
        val framesWritten: Int,
        val duplicateFrames: Int,
        val timestampsNs: LongArray,
        /** Exposure/ISO/AF as the camera REPORTED them, not as we asked. */
        val reportedExposureNs: Long,
        val reportedIso: Int,
        val reportedFocusDistance: Float,
        /** `LENS_INFO_MINIMUM_FOCUS_DISTANCE` in diopters — the closest the lens can focus. */
        val minFocusDiopters: Float,
        val aeMode: Int,
        /** `SENSOR_FRAME_DURATION` as reported. The rate ceiling under manual sensor mode. */
        val reportedFrameDurationNs: Long,
        val edgeMode: Int,
        val noiseReductionMode: Int,
        /** `SHADING_MODE` as reported back, not as requested. -1 = the device said nothing. */
        val shadingMode: Int,
        val manualRequested: Boolean,
        val error: String?,
        /**
         * False when the run deliberately skipped disk writes to isolate camera rate from writer
         * throughput. When false, [duplicateFrames] was **not measured** and must not be read as
         * zero — duplicate detection lives in the writer.
         */
        val framesWereWritten: Boolean,
    ) {
        /**
         * Delivered rate from the FIRST to the LAST sensor timestamp.
         *
         * Uses sensor timestamps rather than wall clock so writer stalls do not flatter or
         * penalise the camera, and spans the whole run rather than averaging per-frame deltas so a
         * single long gap cannot hide behind many short ones.
         */
        val deliveredFps: Double
            get() {
                if (timestampsNs.size < 2) return 0.0
                val span = timestampsNs.last() - timestampsNs.first()
                if (span <= 0L) return 0.0
                return (timestampsNs.size - 1) * 1_000_000_000.0 / span
            }

        /** Null when the run did not write frames, because then it was never measured. */
        val duplicateFraction: Double?
            get() = when {
                !framesWereWritten -> null
                framesWritten > 0 -> duplicateFrames.toDouble() / framesWritten
                else -> null
            }

        /**
         * Distinct frames per second — the rate that actually matters, and the one the goodput
         * model's `Pc` depends on. A session delivering 240 buffers/s of which half repeat has an
         * effective rate of 120, and quoting 240 would be the unlabelled-number defect ADR-0012
         * exists to prevent.
         */
        val distinctFps: Double?
            get() = duplicateFraction?.let { deliveredFps * (1.0 - it) }

        /** Largest gap between consecutive frames, in units of the nominal frame period. */
        val worstGapFrames: Double
            get() {
                if (timestampsNs.size < 2 || requestedFps <= 0) return 0.0
                val period = 1_000_000_000.0 / requestedFps
                var worst = 0.0
                for (i in 1 until timestampsNs.size) {
                    val d = (timestampsNs[i] - timestampsNs[i - 1]) / period
                    if (d > worst) worst = d
                }
                return worst
            }
    }

    /**
     * Records [frameCount] frames into [bundleDir].
     *
     * Blocking, and meant to be called off the main thread. Returns an [Outcome] even on failure —
     * a run that captured 12 frames and then died tells us more than an exception does, and losing
     * the 12 frames' worth of timing would waste the trip to the device.
     */
    @Suppress("LongMethod")
    fun record(
        bundleDir: String,
        frameCount: Int,
        targetFps: Int,
        maxWidth: Int,
        notes: String,
        /**
         * Focus, in reciprocal metres. **Negative means continuous autofocus.**
         *
         * 5.0 focuses at 20 cm, 3.33 at 30 cm. There is no safe fixed default: infinity is correct
         * for a distant target and catastrophic for a screen at arm's length (F32).
         */
        focusDiopters: Float = -1f,
        /**
         * Optional surface to also send frames to, so a human can see through the camera.
         *
         * Added as a SECOND target on the same session rather than by converting the Y plane
         * ourselves: the compositor scales and colour-converts for free, while a per-frame
         * YUV-to-RGB of a multi-megapixel image in Kotlin would compete with the analysis for the
         * budget that makes the analysis worth having.
         *
         * The aiming screen originally drew only a schematic of the analysis, on the reasoning that
         * the schematic shows the facts that decide success. It does — but it is not something you
         * can line a camera up through, and an operator with no viewfinder is aiming blind (F37).
         */
        previewSurface: android.view.Surface? = null,
        /**
         * Exposure in nanoseconds, and ISO. Both **parameters, not constants**, because the right
         * values depend entirely on what is being photographed and EXP-004/EXP-005 exist to choose
         * them.
         *
         * The first values here — a quarter of the frame period at ISO 400 — were reasonable for a
         * general scene and badly wrong for this one: a full-brightness OLED at close range came
         * back at 66% bright against 9% dark, washed out well past the point where a threshold can
         * separate two levels (F33). Pointing a camera at a light source is not the average case,
         * and the average-case defaults flattered nothing.
         *
         * 0 means "derive from the frame period" (the old behaviour), so a sweep can vary one axis
         * at a time.
         */
        exposureNs: Long = 0L,
        iso: Int = DEFAULT_ISO,
        /**
         * What the TRANSMITTER is doing, and how the rig is arranged.
         *
         * None of this is discoverable from the receiver — the camera cannot know the grid it is
         * looking at, and it certainly cannot know the distance. `ffreplay` REFUSES a bundle whose
         * grid is unset (F29), so without this a capture is not decodable; and CAPTURE-HARNESS
         * requires the rig figures because a capture whose conditions were not recorded is not
         * evidence. Unset fields keep their sentinels rather than plausible defaults.
         */
        rig: RigMetadata = RigMetadata(),
        /**
         * When false, frames are counted and timed but never written.
         *
         * This is the A/B that makes a low delivered rate ATTRIBUTABLE. Writing a 1920x1440 Y
         * plane is 2.76 MB, so 60 fps is ~166 MB/s of I/O; if that saturates, unreturned
         * `ImageReader` buffers throttle the camera and the result is indistinguishable from a
         * sensor that cannot hit the rate. Run both arms and the difference names the culprit.
         */
        writeFrames: Boolean = true,
        /**
         * Called for every delivered frame, on the CAMERA THREAD, with a view of the driver's own Y
         * plane. Must return promptly: the buffer is released as soon as it returns, and holding it
         * starves the pipeline in a way that looks exactly like a camera that cannot hit its rate.
         *
         * This exists so aiming analysis shares ONE session and one set of manual settings with
         * recording. A separate preview path would duplicate the request configuration, and the two
         * copies would drift — which is the mistake ADR-0014 was written about, in a different place.
         */
        onFrame: ((java.nio.ByteBuffer, Int, Int, Int) -> Unit)? = null,
    ): Outcome {
        // `rig` carries the grid, and the grid decides which capture mode maximises px/cell, so it
        // must be known before the mode is chosen.
        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

        // Back camera. The front camera is not a candidate: it is lower resolution on every
        // device we care about, and a receiver is held facing the transmitting screen.
        val cameraId = mgr.cameraIdList.firstOrNull { id ->
            mgr.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: mgr.cameraIdList.firstOrNull()
            ?: return failure("no camera on this device")

        val chars = mgr.getCameraCharacteristics(cameraId)
        sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        minFocusDiopters = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?: return failure("no stream configuration map", cameraId)

        val size = pickSize(map, targetFps, maxWidth, rig.gridCols, rig.gridRows)
            ?: return failure("no YUV_420_888 size at <= $maxWidth px wide", cameraId)

        var thread: HandlerThread? = null
        var reader: ImageReader? = null
        var device: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var recorder: Recorder? = null

        val timestamps = ArrayList<Long>(maxOf(frameCount, 64))
        var reportedExposure = -1L
        var reportedIso = -1
        var reportedFocus = -1f
        val minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: -1f
        var aeMode = -1
        var reportedFrameDuration = -1L
        var edgeMode = -1
        var shadingMode = -1
        var nrMode = -1
        var manualRequested = false
        var error: String? = null

        try {
            thread = HandlerThread("ff-capture").apply { start() }
            val handler = Handler(thread.looper)

            val meta = CaptureMetadata(
                senderModel = rig.senderModel,
                receiverModel = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}",
                osBuild = android.os.Build.FINGERPRINT,
                appCommit = "see git; app ${versionName()}",
                notes = notes,
                displayMode = rig.displayMode,
                gridCols = rig.gridCols,
                gridRows = rig.gridRows,
                modulationProfile = rig.modulationProfile,
                screenBrightness = rig.screenBrightness,
                cameraId = cameraId,
                width = size.width,
                height = size.height,
                fps = targetFps.toDouble(),
                distanceCm = rig.distanceCm,
                angleDeg = rig.angleDeg,
                ambientLux = rig.ambientLux,
                motionCondition = rig.motionCondition,
                sourcePayloadSha256 = rig.payloadSha256,
                sourcePayloadBytes = rig.payloadBytes,
            )
            recorder = Recorder.open(bundleDir, meta)

            // maxImages: enough to absorb a writer hiccup without the camera pipeline stalling,
            // small enough that a leak surfaces fast. C05: return buffers promptly or the pipeline
            // stops — every image is closed in the listener below.
            reader = ImageReader.newInstance(
                size.width, size.height, ImageFormat.YUV_420_888, IMAGE_BUFFERS,
            )

            val done = CountDownLatch(1)
            val rec = recorder
            // `frameCount <= 0` means run until cancelled, which is what the aiming UI wants: it
            // needs a steady stream of verdicts, not a fixed-size dataset.
            val unbounded = frameCount <= 0
            reader.setOnImageAvailableListener({ r ->
                val image = r.acquireNextImage() ?: return@setOnImageAvailableListener
                try {
                    if (cancelled) {
                        done.countDown()
                    } else if (unbounded || timestamps.size < frameCount) {
                        onFrame?.let { cb ->
                            val plane = image.planes[0]
                            // The colour planes ride along for the callback's duration only:
                            // the buffers go back to the camera when it returns.
                            frameChroma = image.planes.takeIf { it.size >= 3 }?.let {
                                Chroma(it[1].buffer, it[2].buffer, it[1].rowStride, it[1].pixelStride)
                            }
                            try {
                                cb(plane.buffer, image.width, image.height, plane.rowStride)
                            } finally {
                                frameChroma = null
                            }
                        }
                        val code = if (writeFrames) rec.writeFrame(image) else 0
                        if (code == 0) {
                            timestamps.add(image.timestamp)
                        } else if (error == null) {
                            error = "writeFrame: ${Recorder.errorName(code)}"
                        }
                        if (!unbounded && timestamps.size >= frameCount) done.countDown()
                    }
                } finally {
                    // Always. A held image starves the pipeline, and a starved pipeline looks
                    // exactly like a camera that cannot hit its rate.
                    image.close()
                }
            }, handler)

            device = openCamera(mgr, cameraId, handler)
                ?: return failure("could not open camera $cameraId", cameraId, size, targetFps)

            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(reader.surface)
                if (previewSurface != null) addTarget(previewSurface)
                set(
                    CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                    android.util.Range(targetFps, targetFps),
                )
                // Everything below exists so consecutive frames are photometrically comparable.
                // Auto-anything re-tunes between frames, which moves the dark/bright references
                // the photometric field (C09) is trying to estimate.
                // Focus is the one manual setting that CANNOT have a fixed sensible default.
                //
                // The first two-device run captured with `LENS_FOCUS_DISTANCE = 0`, i.e. INFINITY,
                // against a transmitting screen roughly 20 cm away — so every frame was recorded
                // out of focus, and a failed decode would have looked like the grid being
                // unresolvable rather than the lens being pointed at nothing (F32). Defocus and
                // insufficient resolution both destroy high-spatial-frequency cell structure, and
                // they are indistinguishable in a decode log.
                //
                // `focusDiopters < 0` means "let the camera decide": continuous AF runs even with
                // manual exposure, since AF and AE are independent. That is the robust default,
                // because it needs no knowledge of the rig geometry. A non-negative value is
                // reciprocal metres (5.0 = 20 cm) and locks focus for the session, which is what a
                // repeatable measurement wants once the distance IS known.
                if (focusDiopters < 0f) {
                    set(
                        CaptureRequest.CONTROL_AF_MODE,
                        CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE,
                    )
                } else {
                    set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                }
                set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF)
                // OQ-016: vendor sharpening and denoise destroy high-spatial-frequency cell
                // structure — the exact signal we are trying to read. Requested off; whether the
                // request is honoured is read back below, not assumed.
                set(CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_OFF)
                set(CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_OFF)
                // Lens shading correction ON -- the one piece of ISP processing this pipeline wants.
                //
                // Edge enhancement and noise reduction are switched off above because they invent
                // structure at the spatial frequency the cells occupy. Shading correction is the
                // opposite kind of operation: it removes a smooth multiplicative field the LENS
                // added, and that field is pure interference here. It was never requested, so the
                // vignette went uncorrected, and RISK-025 called the consequence in advance --
                // "vignetting, worsens at closer range (exactly where we want density), and is not
                // corrected".
                //
                // Measured on the first above-cliff capture: bright nonuniformity 6.51x across the
                // frame, 26% of lattice nodes below the separation floor, and mean pilot residual
                // 32.3 against a budget of 16.6. Getting closer to clear the density cliff made it
                // worse, because vignetting is strongest exactly where the framing is tightest.
                set(CaptureRequest.SHADING_MODE, CameraMetadata.SHADING_MODE_HIGH_QUALITY)

                if (chars.hasCapability(
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
                    )
                ) {
                    manualRequested = true
                    set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    // Exposure one quarter of the frame period: short enough to limit temporal
                    // mixing across display states (the Pc term), long enough to keep SNR usable.
                    // EXP-004 picks the real value; this is a defensible starting point, not one.
                    // Default to nearly the whole frame period, not a quarter of it.
                    //
                    // A quarter was chosen to limit temporal mixing across display states, which is
                    // a real concern at HIGH state rates -- but it costs four times the light, and
                    // the transmitter is a small emissive panel a few centimetres away, not a
                    // scene. At 60 fps it produced a capture with mean luminance 3.4 and 1.1% of
                    // pixels lit, which failed geometry on 55 frames of 60; the run that worked had
                    // used 16 ms (F36).
                    //
                    // The honest position is that the right exposure depends on the STATE rate,
                    // which the receiver does not know -- a slow state rate has no mixing to avoid
                    // and wants all the light it can get, while a fast one must cut exposure to
                    // catch a state before it changes. Choosing light by default is the safer
                    // failure: an overexposed frame still localises, an underexposed one does not.
                    // EXP-004 picks the real value.
                    val exposure =
                        if (exposureNs > 0L) exposureNs
                        else (1_000_000_000L / targetFps) * 9 / 10
                    set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposure)
                    set(CaptureRequest.SENSOR_SENSITIVITY, iso)
                    this@CameraRecorder.iso = iso
                    this@CameraRecorder.exposureNs = exposure
                    maxExposureNs = 1_000_000_000L / targetFps
                    chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let { isoRange = it }
                    if (focusDiopters >= 0f) {
                        // Clamped to what the lens can actually do. Requesting 10 diopters on a lens
                        // whose minimum focus distance is 5 would otherwise be silently ignored, and
                        // the read-back below is what proves which happened.
                        val maxDiopters =
                            chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                        val want = if (maxDiopters > 0f) focusDiopters.coerceAtMost(maxDiopters)
                                   else focusDiopters
                        set(CaptureRequest.LENS_FOCUS_DISTANCE, want)
                    }

                    // THE frame-rate control once AE is off, and the reason the first run of this
                    // recorder delivered 31 fps against a requested 60 (finding F28).
                    //
                    // `CONTROL_AE_TARGET_FPS_RANGE` is an AUTO-EXPOSURE control: with
                    // CONTROL_AE_MODE_OFF the auto-exposure routine is not running, so nothing
                    // consumes the range and the frame period falls back to whatever the request
                    // template carries. Under manual sensor control the period is set explicitly
                    // by SENSOR_FRAME_DURATION, and omitting it does not fail loudly -- it just
                    // silently produces the wrong rate, which is indistinguishable from a device
                    // that cannot hit the rate.
                    set(CaptureRequest.SENSOR_FRAME_DURATION, 1_000_000_000L / targetFps)
                } else {
                    set(CaptureRequest.CONTROL_AE_LOCK, true)
                }

                // Optical stabilisation ON, electronic stabilisation OFF -- for a hand-held receiver.
                //
                // OIS moves the lens against hand tremor DURING the exposure, which is exactly the
                // motion blur that loses frames (F53). EIS is the opposite kind of help: it warps
                // and crops each frame after capture, which bends the straight boundary edges the
                // detector fits and throws away resolution the density budget needs.
                if (chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)
                        ?.contains(CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON) == true
                ) {
                    set(
                        CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE,
                        CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON,
                    )
                }
                set(
                    CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                    CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF,
                )

                // Lens distortion corrected in the camera, wherever it offers that without costing
                // frame rate (FAST). The decoder maps the screen with ONE homography, which is
                // exact for a flat screen through an ideal lens and cannot bend: a wide phone lens
                // bows straight edges, most of all near the edges of the picture -- exactly where a
                // screen that fills the view has its corners. From the Pixel 8's own calibration
                // (`LENS_DISTORTION` 0.089, -0.162, 0.119), a screen filling 85% of the frame is
                // off by ~10 px, about 0.7 of a cell, along its long edges; at a third of the frame,
                // a quarter of that. That is "the Pixel reads best from far away". It offers FAST
                // and was never asked for it. The S26 Ultra's main camera does not list the
                // control at all, and its own coefficients put the same framing at ~0.35 of a cell.
                chars.get(CameraCharacteristics.DISTORTION_CORRECTION_AVAILABLE_MODES)
                    ?.takeIf { CameraMetadata.DISTORTION_CORRECTION_MODE_FAST in it }
                    ?.let {
                        set(
                            CaptureRequest.DISTORTION_CORRECTION_MODE,
                            CameraMetadata.DISTORTION_CORRECTION_MODE_FAST,
                        )
                    }

                // Focus on the MIDDLE of the view, where the sender's screen is. Unweighted, AF
                // can settle on whatever has the most contrast -- the desk, a hand -- and the
                // screen at a different distance is then outside a depth of field that is only a
                // few millimetres deep at 15 cm (F57).
                if (focusDiopters < 0f) centreAfRegion(chars)?.let {
                    set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(it))
                }
            }
            val request = builder.build()

            session = createSession(device, reader.surface, previewSurface, handler)
                ?: return failure("could not configure a capture session", cameraId, size, targetFps)

            val callback = object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(
                    s: CameraCaptureSession,
                    r: CaptureRequest,
                    result: TotalCaptureResult,
                ) {
                    // First row to last row, per frame. With the exposure it bounds how fast the
                    // display can change state before captures tear (F60) -- measured, not assumed.
                    result.get(CaptureResult.SENSOR_ROLLING_SHUTTER_SKEW)?.let {
                        rollingShutterSkewNs = it
                    }
                    result.get(CaptureResult.DISTORTION_CORRECTION_MODE)?.let {
                        distortionCorrection = it
                    }
                    result.get(CaptureResult.CONTROL_AF_STATE)?.let {
                        afState = it
                        onAfState(it)
                    }
                    // Read back what the camera DID, not what we asked for. RISK-011: a
                    // device advertising MANUAL_SENSOR may ignore the settings, and the only
                    // way to know is to compare the result against the request.
                    result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { reportedExposure = it }
                    result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { reportedIso = it }
                    result.get(CaptureResult.LENS_FOCUS_DISTANCE)?.let {
                        reportedFocus = it
                        lastFocusDiopters = it
                    }
                    result.get(CaptureResult.CONTROL_AE_MODE)?.let { aeMode = it }
                    result.get(CaptureResult.SENSOR_FRAME_DURATION)?.let {
                        reportedFrameDuration = it
                    }
                    result.get(CaptureResult.EDGE_MODE)?.let { edgeMode = it }
                    result.get(CaptureResult.SHADING_MODE)?.let { shadingMode = it }
                    result.get(CaptureResult.NOISE_REDUCTION_MODE)?.let { nrMode = it }
                }
            }
            control = LiveControl(session, builder, callback, handler, chars)
            session.setRepeatingRequest(request, callback, handler)

            if (unbounded) {
                // No deadline: an aiming session lasts as long as the user needs. Polled rather than
                // awaited without limit so a cancel is never missed if it lands between checks.
                while (!cancelled) {
                    if (done.await(200, TimeUnit.MILLISECONDS)) break
                }
            } else {
                // Generous: a cold camera start plus AE settling can take a second or two, and the
                // point is to fail with data rather than to fail fast.
                val timeoutSec = 20L + frameCount / maxOf(targetFps, 1)
                if (!done.await(timeoutSec, TimeUnit.SECONDS)) {
                    error = "timed out after ${timestamps.size}/$frameCount frames"
                }
            }
        } catch (e: Exception) {
            error = "${e::class.simpleName}: ${e.message}"
            Log.e(TAG, "capture failed", e)
        } finally {
            control = null
            runCatching { session?.stopRepeating() }
            runCatching { session?.close() }
            runCatching { device?.close() }
            runCatching { reader?.close() }
            val code = recorder?.finish() ?: 0
            if (code != 0 && error == null) error = "finish: ${Recorder.errorName(code)}"
            val dupes = recorder?.duplicateFrames ?: -1
            val written = recorder?.framesWritten ?: -1
            runCatching { recorder?.close() }
            runCatching { thread?.quitSafely() }

            return Outcome(
                framesWereWritten = writeFrames,
                cameraId = cameraId,
                size = size,
                requestedFps = targetFps,
                framesWritten = if (writeFrames) written else timestamps.size,
                duplicateFrames = dupes,
                timestampsNs = timestamps.toLongArray(),
                reportedExposureNs = reportedExposure,
                reportedIso = reportedIso,
                reportedFocusDistance = reportedFocus,
                minFocusDiopters = minFocus,
                aeMode = aeMode,
                reportedFrameDurationNs = reportedFrameDuration,
                edgeMode = edgeMode,
                noiseReductionMode = nrMode,
                shadingMode = shadingMode,
                manualRequested = manualRequested,
                error = error,
            )
        }
    }

    // ------------------------------------------------------------------ helpers

    object Modes {
        /**
         * The capture rate to ask for: the highest of [rates] this camera offers at (nearly) the
         * same resolution as the lowest.
         *
         * More captures per display state is more chances that one of them is clean, and a shorter
         * frame period caps the exposure, which cuts both motion blur and state mixing (F60). But
         * resolution decides how far away the phone can be held (F57), so 60 fps is only worth it
         * where it costs almost none: the Pixel 8 offers 4032x2268 at 60 against 4080x3072 at 30,
         * the same long axis -- and the long axis is the one the screen's long side lies along.
         */
        fun chooseFps(context: Context, maxWidth: Int, rates: List<Int> = listOf(60, 50, 30)): Int {
            val longest = { fps: Int ->
                runCatching {
                    val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
                    val id = mgr.cameraIdList.firstOrNull { cid ->
                        mgr.getCameraCharacteristics(cid).get(CameraCharacteristics.LENS_FACING) ==
                            CameraCharacteristics.LENS_FACING_BACK
                    } ?: return@runCatching 0
                    val map = mgr.getCameraCharacteristics(id)
                        .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                        ?: return@runCatching 0
                    map.getOutputSizes(ImageFormat.YUV_420_888)
                        .filter { it.width <= maxWidth }
                        .filter {
                            map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, it) <=
                                1_000_000_000L / fps
                        }
                        .maxOfOrNull { maxOf(it.width, it.height) } ?: 0
                }.getOrDefault(0)
            }
            // The fastest rate that keeps (nearly) the resolution of the slowest. Not just 60-or-30:
            // the S26 Ultra offers 60 fps only up to 2336x1080, but 50 fps at 4000x1848 -- the
            // same long side as its 30 fps mode, so 50 is two thirds more captures for free.
            val slowest = rates.minOrNull() ?: 30
            val base = longest(slowest)
            return rates.sortedDescending().firstOrNull { r ->
                val l = longest(r)
                l > 0 && l >= base * 0.9
            } ?: slowest
        }
    }

    private fun CameraCharacteristics.hasCapability(cap: Int): Boolean =
        get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.contains(cap) == true

    /**
     * The `YUV_420_888` size that maximises **resolvable pixels per cell** for the target grid.
     *
     * NOT largest-by-area, which is what this did first and which is subtly wrong. Asked for a mode
     * at or below 1920 wide, largest-area picked a **1920×1920** square sensor mode — and a square
     * frame wastes most of its pixels on a portrait screen, delivering no more px/cell than
     * 1920×1080 while costing 78% more bandwidth (F33).
     *
     * The criterion that actually matters: the screen is a rectangle of aspect `cols/rows`, and it
     * must fit inside the frame **with margin** or the boundary ring is clipped and localisation
     * fails outright. Given a mode `W×H` and a margin fraction `m`, the largest the screen's long
     * axis can be is `m·min(W, H/r)` — so px/cell is that over `rows`. Maximising it weighs
     * resolution and aspect together, which area alone cannot.
     *
     * Both orientations are considered, because the receiver can be rotated and the sensor's long
     * axis may carry either screen axis.
     */
    private fun pickSize(
        map: StreamConfigurationMap,
        targetFps: Int,
        maxWidth: Int,
        cols: Int,
        rows: Int,
    ): Size? {
        val minDurationNs = 1_000_000_000L / targetFps
        val candidates = map.getOutputSizes(ImageFormat.YUV_420_888)
            ?.filter { it.width <= maxWidth }
            ?.filter { map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, it) <= minDurationNs }
            ?: return null
        if (cols <= 0 || rows <= 0) return candidates.maxByOrNull { it.width.toLong() * it.height }

        val r = cols.toDouble() / rows       // screen short : long
        val margin = 0.85                    // must leave a visible ring of background on all sides
        fun score(s: Size): Double {
            val a = margin * minOf(s.width.toDouble(), s.height / r)
            val b = margin * minOf(s.height.toDouble(), s.width / r)
            return maxOf(a, b) / rows
        }

        // Ties are common and must be broken deliberately.
        //
        // Whenever the same axis is limiting, modes of very different shape score IDENTICALLY:
        // 1920x1080 and 1920x1920 both yield 6.28 px/cell for this grid, because the screen's long
        // axis is bounded by the 1920 either way. `maxByOrNull` then returns whichever came first in
        // the driver's list, and it returned the SQUARE one -- which wastes 78% more bandwidth for no
        // extra resolvable density, and shows the operator a square viewport that looks nothing like
        // the phone they are aiming (F38, F41).
        //
        // So among equal-scoring modes, take the smallest area. That is not a cosmetic preference:
        // write throughput is the harness's binding constraint (F28), and the discarded pixels
        // contribute nothing to px/cell by construction.
        // Among equal-scoring modes, prefer a STANDARD aspect ratio.
        //
        // "Smallest area" was the previous tie-break and it selected 1920x960 -- a 2:1 crop, which
        // is not a shape sensors normally deliver. Unusual crops are where non-square pixels and
        // anamorphic squeezes live, and the operator reported the preview still stretched
        // vertically after the orientation was confirmed correct. 4:3 and 16:9 are the shapes a
        // sensor is actually read out in, so they are the safe ones to trust geometrically.
        //
        // Area still breaks any remaining tie, because write throughput is the harness's binding
        // constraint (F28) and the extra pixels add no resolvable density.
        val standard = doubleArrayOf(4.0 / 3.0, 16.0 / 9.0, 3.0 / 2.0)
        fun aspectPenalty(s: Size): Double {
            val a = maxOf(s.width, s.height).toDouble() / minOf(s.width, s.height)
            return standard.minOf { kotlin.math.abs(a - it) / it }
        }
        val best = candidates.maxOfOrNull { score(it) } ?: return null
        return candidates
            .filter { score(it) >= best - 1e-6 }
            .sortedWith(
                compareBy({ if (aspectPenalty(it) <= 0.02) 0 else 1 },
                          { it.width.toLong() * it.height }),
            )
            .firstOrNull()
    }

    private fun openCamera(mgr: CameraManager, id: String, handler: Handler): CameraDevice? {
        // The callers ask for the permission first; checked again here because it can be revoked
        // from Settings while the app is running, and openCamera would then throw.
        if (context.checkSelfPermission(android.Manifest.permission.CAMERA) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            Log.e(TAG, "camera permission not granted")
            return null
        }
        var device: CameraDevice? = null
        val latch = CountDownLatch(1)
        mgr.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(c: CameraDevice) { device = c; latch.countDown() }
            override fun onDisconnected(c: CameraDevice) { c.close(); latch.countDown() }
            override fun onError(c: CameraDevice, e: Int) {
                Log.e(TAG, "camera error $e")
                c.close()
                latch.countDown()
            }
        }, handler)
        latch.await(10, TimeUnit.SECONDS)
        return device
    }

    @Suppress("DEPRECATION")  // SessionConfiguration is API 28+, but takes an Executor we do not
    // want here: the capture callbacks must land on the camera Handler thread, and the deprecated
    // overload expresses that directly.
    private fun createSession(
        device: CameraDevice,
        surface: android.view.Surface,
        preview: android.view.Surface?,
        handler: Handler,
    ): CameraCaptureSession? {
        var session: CameraCaptureSession? = null
        val latch = CountDownLatch(1)
        device.createCaptureSession(
            listOfNotNull(surface, preview),
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) { session = s; latch.countDown() }
                override fun onConfigureFailed(s: CameraCaptureSession) {
                    Log.e(TAG, "session configure failed")
                    latch.countDown()
                }
            },
            handler,
        )
        latch.await(10, TimeUnit.SECONDS)
        return session
    }

    private fun versionName(): String = runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
    }.getOrElse { "?" }

    private fun failure(
        why: String,
        cameraId: String = "",
        size: Size = Size(0, 0),
        fps: Int = 0,
    ) = Outcome(
        cameraId = cameraId, size = size, requestedFps = fps, framesWritten = 0,
        duplicateFrames = 0, timestampsNs = LongArray(0), reportedExposureNs = -1,
        reportedIso = -1, reportedFocusDistance = -1f, minFocusDiopters = -1f, aeMode = -1,
        reportedFrameDurationNs = -1L, edgeMode = -1,
        noiseReductionMode = -1, shadingMode = -1,
        manualRequested = false, error = why, framesWereWritten = true,
    )

    companion object {
        private const val TAG = "FileFlow.Capture"
        /**
         * Most a live receive zooms. Zoom here is a crop, not optics: it lets the person stand back to
         * where the lens focuses while the screen still fills the picture. It cannot add sensor
         * pixels, but it is taken before the camera halves colour resolution, so colour per cell
         * gains all of it. At 2x with the screen filling the picture, a 260-row grid still has
         * ~6.5 sensor pixels a cell on a 4032-wide stream.
         */
        const val MAX_LIVE_ZOOM = 2.0f
        private const val IMAGE_BUFFERS = 6
        /** Side of the tap-to-focus region, and of the standing centre region, as array fractions. */
        private const val AF_TAP_FRACTION = 0.15f
        private const val AF_CENTRE_FRACTION = 0.4f
        /** A scan that has not locked by now is abandoned and continuous AF resumed. */
        private const val AF_SCAN_TIMEOUT_MS = 2500L
        /** How long a completed scan holds before continuous AF takes over again. */
        private const val AF_HOLD_AFTER_SCAN_MS = 600L

        /**
         * Low, deliberately. The subject is an emissive display at full brightness, not a room.
         * ISO 400 overexposed it badly (F33), and overexposure destroys the dark level the
         * photometric field needs to separate two states.
         */
        const val DEFAULT_ISO = 60
    }
}
