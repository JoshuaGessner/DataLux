package dev.fileflow.rx

import android.content.Context
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Kotlin face of the live receiver (C13b): camera frames in, a verified file out, and a sentence the
 * person holding the phone can act on.
 *
 * Every judgement — phase, hint, wording, whether the file verified — is made in
 * `core/src/live.cpp`, where the desktop suite tests it against simulated handheld motion
 * (ADR-0014). This class only moves frames to it on a worker thread and reports back.
 *
 * ⚠ [Slot] MUST match the enum in `jni_receiver.cpp`; the count is checked at construction.
 */
enum class LivePhase {
    Looking, Locked, Reading, Receiving, Verified, Failed, Unknown;

    val terminal: Boolean get() = this == Verified || this == Failed

    companion object {
        fun from(ordinal: Int): LivePhase = entries.getOrNull(ordinal) ?: Unknown
    }
}

enum class LiveHint {
    None, PointAtScreen, HoldSteadier, NotReadable, Unknown;

    companion object {
        fun from(ordinal: Int): LiveHint = entries.getOrNull(ordinal) ?: Unknown
    }
}

data class LiveStatus(
    val phase: LivePhase,
    val hint: LiveHint,
    /** One complete sentence for the person, decided in native. Show it as is. */
    val message: String,
    /** 0..1, never decreases — a transfer the phone drifted off is paused, not lost. */
    val progress: Double,
    /** Negative when there is no honest estimate (nothing arriving right now). */
    val secondsRemaining: Double,
    val motionBlurCells: Double,
    val recentReadFraction: Double,
    val screenInView: Boolean,
    val frames: Long,
    val fileSize: Long,
)

/**
 * Feeds camera frames to the native receiver on ONE worker thread.
 *
 * Frames that arrive while the worker is busy are DROPPED, deliberately. The decoder cannot keep up
 * with every frame on a phone, and it does not need to: fountain coding makes any sufficiently large
 * set of frames enough, so skipping one costs a little time and nothing else. Queuing them instead
 * would build latency until the status on screen described where the phone was seconds ago — the
 * worst possible thing for someone trying to hold it steady.
 */
class LiveDecoder(cols: Int, rows: Int, exposureNs: Long) : AutoCloseable {

    private val handle = NativeReceiver.open(cols, rows, exposureNs)
    private val slots = DoubleArray(NativeReceiver.slotCount())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "ff-live-decode") }
    private val busy = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private val nativeLock = Any()
    private var frame: ByteBuffer? = null

    /** Set once the transfer has verified or failed; frames are ignored from then on. */
    @Volatile var finished = false
        private set

    /** Called on the WORKER thread after every decoded frame. */
    @Volatile var onStatus: ((LiveStatus) -> Unit)? = null

    init {
        require(slots.size == Slot.entries.size) {
            "native reports ${slots.size} slots but Kotlin expects ${Slot.entries.size} — the ABI " +
                "has drifted"
        }
    }

    /**
     * Offer one frame, from the camera thread. Copies the Y plane (the camera's buffer is released
     * the moment the callback returns) and hands it to the worker — unless the worker is busy, in
     * which case the frame is dropped. Returns whether it was taken.
     */
    fun offer(y: ByteBuffer, width: Int, height: Int, rowStride: Int, timestampNs: Long): Boolean {
        if (width <= 0 || height <= 0 || rowStride < width) return false
        // The camera reports stride and size; a buffer shorter than they imply would make the copy
        // below throw ON THE CAMERA THREAD and take the app down mid-transfer. Refuse the frame.
        if (y.capacity().toLong() < rowStride.toLong() * (height - 1) + width) return false
        if (finished || closed.get() || !busy.compareAndSet(false, true)) return false
        // From here every exit must release `busy`, or one bad frame stops decoding for good while
        // the screen keeps saying "receiving".
        try {
            val need = width * height
            val dst = frame?.takeIf { it.capacity() >= need }
                ?: ByteBuffer.allocateDirect(need).also { frame = it }
            val src = y.duplicate()  // own position/limit; the preview reads the same buffer
            dst.clear()
            for (row in 0 until height) {
                src.clear()
                src.position(row * rowStride)
                src.limit(row * rowStride + width)
                dst.put(src)
            }
            dst.flip()
            worker.execute {
                try {
                    val s = ingest(dst, width, height, timestampNs) ?: return@execute
                    if (s.phase.terminal) finished = true
                    onStatus?.invoke(s)
                } finally {
                    busy.set(false)
                }
            }
            return true
        } catch (e: RuntimeException) {
            // Includes RejectedExecutionException from a close() racing this frame.
            busy.set(false)
            return false
        }
    }

    private fun ingest(buf: ByteBuffer, width: Int, height: Int, ts: Long): LiveStatus? {
        val message = NativeReceiver.ingest(handle, buf, width, height, width, ts, slots)
        if (slots[Slot.Phase.ordinal] < 0.0) return null
        return LiveStatus(
            phase = LivePhase.from(slots[Slot.Phase.ordinal].toInt()),
            hint = LiveHint.from(slots[Slot.Hint.ordinal].toInt()),
            message = message,
            progress = slots[Slot.Progress.ordinal],
            secondsRemaining = slots[Slot.SecondsRemaining.ordinal],
            motionBlurCells = slots[Slot.MotionBlurCells.ordinal],
            recentReadFraction = slots[Slot.RecentReadFraction.ordinal],
            screenInView = slots[Slot.ScreenInView.ordinal] != 0.0,
            frames = slots[Slot.Frames.ordinal].toLong(),
            fileSize = slots[Slot.FileSize.ordinal].toLong(),
        )
    }

    /**
     * Write the VERIFIED file into the app's "received" folder and return it, or null if there is
     * no verified file. Call once [finished].
     *
     * Written to a randomly named temporary file first and renamed into place, so a crash mid-write
     * never leaves something that looks like a complete file (PROTOCOL-SPEC, file layer). The name is
     * the manifest's — sanitised in native, and sanitised again here, because it came off a screen
     * that anyone could have been showing (THREAT-MODEL T3).
     */
    fun saveVerified(context: Context): File? {
        // Under the same lock as close(): leaving the screen at the moment of completion would
        // otherwise free the native receiver while its bytes were being read out.
        val (bytes, raw) = synchronized(nativeLock) {
            if (closed.get()) return null
            val b = NativeReceiver.fileBytes(handle) ?: return null
            b to NativeReceiver.fileName(handle)
        }
        val dir = (context.getExternalFilesDir(RECEIVED_DIR) ?: File(context.filesDir, RECEIVED_DIR))
            .apply { mkdirs() }
        val safe = raw.replace(Regex("[/\\\\\\u0000-\\u001f]"), "_")
            .trimStart('.').ifBlank { "received.bin" }
        val stem = safe.substringBeforeLast('.', safe)
        val ext = if ('.' in safe) "." + safe.substringAfterLast('.') else ""
        var target = File(dir, safe)
        var n = 1
        while (target.exists()) target = File(dir, "$stem ($n)$ext").also { n++ }
        val tmp = File.createTempFile(".partial-", ".tmp", dir)
        return try {
            tmp.writeBytes(bytes)
            if (tmp.renameTo(target)) target else null
        } finally {
            tmp.delete()  // a no-op after a successful rename
        }
    }

    /**
     * Stop the worker and free native memory. The handle is freed only if the worker actually
     * stopped: freeing it under a decode still in flight would be a use-after-free in native.
     */
    override fun close() {
        synchronized(nativeLock) {
            if (!closed.compareAndSet(false, true)) return
            finished = true
            worker.shutdown()
            if (worker.awaitTermination(2, TimeUnit.SECONDS)) NativeReceiver.close(handle)
        }
    }

    /** Declaration order IS the wire format. Keep in lockstep with `Slot` in `jni_receiver.cpp`. */
    private enum class Slot {
        Phase, Hint, Progress, SecondsRemaining, MotionBlurCells, RecentReadFraction,
        ScreenInView, Frames, FileSize,
    }

    companion object {
        const val RECEIVED_DIR = "received"
    }
}

internal object NativeReceiver {
    init {
        System.loadLibrary("fileflow")
    }

    @JvmStatic external fun slotCount(): Int
    @JvmStatic external fun open(cols: Int, rows: Int, exposureNs: Long): Long
    @JvmStatic external fun ingest(
        handle: Long, y: ByteBuffer, width: Int, height: Int, rowStride: Int, timestampNs: Long,
        out: DoubleArray,
    ): String
    @JvmStatic external fun fileName(handle: Long): String
    @JvmStatic external fun fileBytes(handle: Long): ByteArray?
    @JvmStatic external fun close(handle: Long)
}
