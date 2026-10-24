package dev.fileflow.rx

import android.content.Context
import dev.fileflow.SessionLog
import dev.fileflow.secure.Envelope
import dev.fileflow.secure.PairingKey
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
    /** Frames whose payload passed FEC and CRC. More than zero proves the grid (F58). */
    val payloadFrames: Long,
    /** Verified-file bytes per second right now, smoothed in native (TransferRate); 0 = none. */
    val bytesPerSecond: Double,
    /** Distinct display states whose header has read so far: the state rate actually seen. */
    val distinctStates: Long,
    /** Last colour frame: distance between its two closest colours, and their noise (0..255). */
    val colorSpread: Double,
    val colorNoise: Double,
    /** Once verified: seconds from the file's first data to verification (the link's own time). */
    val transferSeconds: Double,
    /** Bits per cell the sender uses, from its headers: 1, 2 (four colours) or 3; 0 before. */
    val bitsPerCell: Int,
    /**
     * The file before compression, as the sender's manifest says (0 = not said, as under
     * encryption); the codec (0 none, 1 deflate, 2 zstd) and what compressing cost the sender.
     */
    val originalSize: Long,
    val codec: Int,
    val compressMs: Long,
    /** The grid this status was decoded against: the detected one once [gridKnown]. */
    val cols: Int,
    val rows: Int,
    /** True once a frame header has decoded, which proves the grid. Before that it is a guess. */
    val gridKnown: Boolean,
)

/**
 * The grids a transmitter can be sending. The SEND screen offers exactly these, and the receiver
 * tries each of them, so nobody has to set the same grid on two phones.
 */
object Grids {
    val ALL: List<Pair<Int, Int>> = listOf(120 to 260, 144 to 240, 180 to 390)
}

/**
 * Feeds camera frames to the native receiver on [lanes] worker threads at once (F60).
 *
 * Each worker owns one decode lane: its own frame buffer, its own native pipeline. A frame goes to
 * whichever lane is idle; when every lane is busy it is DROPPED, deliberately. Fountain coding
 * makes any sufficiently large set of frames enough, so a skipped frame costs a little time and
 * nothing else, while queuing would build latency until the status on screen described where the
 * phone was seconds ago -- the worst thing for someone trying to hold it steady.
 *
 * One lane used to be all there was, and it decoded 20-50 ms of a 4080-wide frame: 20-50 frames a
 * second at best, against a camera delivering 30 or 60.
 */
class LiveDecoder(
    grids: List<Pair<Int, Int>>,
    exposureNs: Long,
    private val lanes: Int = DEFAULT_LANES,
) : AutoCloseable {

    /**
     * One native receiver per grid the sender might be using, until a frame's PAYLOAD decodes
     * against one of them. That proves the grid -- the payload spans every cell and must pass RS
     * and a CRC. A decoded header does not: a near-miss grid reads the header band at the top of
     * the frame on every frame (F58).
     *
     * The grid used to be a setting on BOTH phones that had to match, with nothing to say so when
     * it did not. Now the receiver finds it: frames go to the candidates in turn until one decodes
     * a payload, then every frame goes to that one. The losers are kept until [close], unfed --
     * another lane may be inside one of them at the moment of the switch, and freeing it there
     * would be a use-after-free.
     */
    private class Candidate(val cols: Int, val rows: Int, val handle: Long)

    private val candidates: List<Candidate> =
        grids.distinct().map { (c, r) -> Candidate(c, r, NativeReceiver.open(c, r, exposureNs, lanes)) }
    private val next = java.util.concurrent.atomic.AtomicInteger(0)
    private val lastByCandidate = java.util.concurrent.ConcurrentHashMap<Candidate, LiveStatus>()
    /** Per candidate: payload count last seen, and the capture time it last went up. */
    private val lastPayloads = java.util.concurrent.ConcurrentHashMap<Candidate, Long>()
    private val lastPayloadNs = java.util.concurrent.ConcurrentHashMap<Candidate, Long>()
    /** When each candidate last read headers on most frames: the grid is right, whatever the data. */
    private val lastHeaderNs = java.util.concurrent.ConcurrentHashMap<Candidate, Long>()
    /** Non-null once the grid is proven. */
    @Volatile private var committed: Candidate? = null
    private val handle: Long get() = (committed ?: candidates.first()).handle

    /** One per worker thread: its buffer, its slots, its busy flag, its executor. */
    private inner class Lane(val index: Int) {
        val busy = AtomicBoolean(false)
        val slots = DoubleArray(NativeReceiver.slotCount())
        var frame: ByteBuffer? = null
        var u: ByteBuffer? = null
        var v: ByteBuffer? = null
        /** Strides of the chroma copied with the current frame; 0 = no colour this frame. */
        var chromaRowStride = 0
        var chromaPixelStride = 0
        val worker = Executors.newSingleThreadExecutor { r ->
            Thread(r, "ff-live-decode-$index").apply { priority = Thread.MAX_PRIORITY - 1 }
        }
    }
    private val laneList = List(lanes.coerceIn(1, MAX_LANES)) { Lane(it) }

    /**
     * Lanes actually fed, at most [lanes]. Lowered when the phone runs hot: decoding a 60 fps
     * camera on four cores is sustained work, and a phone that throttles itself mid-transfer
     * slows everything at once, the camera included. Fewer lanes only means more frames
     * skipped, which the fountain code absorbs (OQ-036).
     */
    @Volatile var activeLanes: Int = lanes

    // Telemetry, logged periodically: the only way to know on a real phone whether the decoder
    // keeps up with the camera, which decides how much of the transmitter's rate is reaching us.
    private val offered = java.util.concurrent.atomic.AtomicLong()
    private val taken = java.util.concurrent.atomic.AtomicLong()
    private val decoded = java.util.concurrent.atomic.AtomicLong()
    private val decodeNs = java.util.concurrent.atomic.AtomicLong()
    private val windowStartNs = java.util.concurrent.atomic.AtomicLong(System.nanoTime())
    /** Distinct states at the last telemetry line, for the states-seen rate between lines. */
    @Volatile private var lastDistinct = 0L
    private val closed = AtomicBoolean(false)
    private val nativeLock = Any()

    /** Set once the transfer has verified or failed; frames are ignored from then on. */
    @Volatile var finished = false
        private set

    /** Called on a WORKER thread after every decoded frame -- from several, so possibly concurrently. */
    @Volatile var onStatus: ((LiveStatus) -> Unit)? = null

    init {
        require(laneList.first().slots.size == Slot.entries.size) {
            "native reports ${laneList.first().slots.size} slots but Kotlin expects " +
                "${Slot.entries.size} — the ABI has drifted"
        }
    }

    /**
     * Offer one frame, from the camera thread. Copies the Y plane (the camera's buffer is released
     * the moment the callback returns) into an idle lane — or drops it if every lane is busy.
     * Returns whether it was taken.
     */
    fun offer(
        y: ByteBuffer, width: Int, height: Int, rowStride: Int, timestampNs: Long,
        chroma: dev.fileflow.capture.CameraRecorder.Chroma? = null,
    ): Boolean {
        if (width <= 0 || height <= 0 || rowStride < width) return false
        // The camera reports stride and size; a buffer shorter than they imply would make the copy
        // below throw ON THE CAMERA THREAD and take the app down mid-transfer. Refuse the frame.
        if (y.capacity().toLong() < rowStride.toLong() * (height - 1) + width) return false
        offered.incrementAndGet()
        if (finished || closed.get()) return false
        val limit = activeLanes.coerceIn(1, laneList.size)
        var lane: Lane? = null
        for (i in 0 until limit) {
            if (laneList[i].busy.compareAndSet(false, true)) { lane = laneList[i]; break }
        }
        if (lane == null) return false
        taken.incrementAndGet()
        // From here every exit must release `busy`, or one bad frame stops that lane for good.
        try {
            val need = width * height
            val dst = lane.frame?.takeIf { it.capacity() >= need }
                ?: ByteBuffer.allocateDirect(need).also { lane.frame = it }
            val src = y.duplicate()  // own position/limit; the preview reads the same buffer
            dst.clear()
            if (rowStride == width) {
                src.clear()
                src.limit(need)
                dst.put(src)
            } else {
                for (row in 0 until height) {
                    src.clear()
                    src.position(row * rowStride)
                    src.limit(row * rowStride + width)
                    dst.put(src)
                }
            }
            dst.flip()
            // Colour, copied as the camera laid it out (native packs it): two bulk copies of a
            // quarter of the luma each, for the M5 sender that needs them.
            lane.chromaRowStride = 0
            if (chroma != null) {
                lane.u = copyPlane(chroma.u, lane.u).also { lane.v = copyPlane(chroma.v, lane.v) }
                lane.chromaRowStride = chroma.rowStride
                lane.chromaPixelStride = chroma.pixelStride
            }
            lane.worker.execute {
                try {
                    val s = ingest(lane, dst, width, height, timestampNs) ?: return@execute
                    if (s.phase.terminal) finished = true
                    onStatus?.invoke(s)
                } finally {
                    lane.busy.set(false)
                }
            }
            return true
        } catch (e: RuntimeException) {
            // Includes RejectedExecutionException from a close() racing this frame.
            lane.busy.set(false)
            return false
        }
    }

    /** A lane's worker thread. */
    /** A whole camera plane into a reusable direct buffer, byte for byte. */
    private fun copyPlane(src: ByteBuffer, into: ByteBuffer?): ByteBuffer {
        val s = src.duplicate().apply { clear() }
        val dst = into?.takeIf { it.capacity() >= s.remaining() }
            ?: ByteBuffer.allocateDirect(s.remaining())
        dst.clear()
        dst.put(s)
        dst.flip()
        return dst
    }

    private fun ingest(lane: Lane, buf: ByteBuffer, width: Int, height: Int, ts: Long): LiveStatus? {
        val c = committed ?: candidates[Math.floorMod(next.getAndIncrement(), candidates.size)]
        val t0 = System.nanoTime()
        val s = ingestInto(lane, c, buf, width, height, ts) ?: return null
        decodeNs.addAndGet(System.nanoTime() - t0)
        val n = decoded.incrementAndGet()
        if (n % TELEMETRY_EVERY == 0L) {
            val now = System.nanoTime()
            val wall = (now - windowStartNs.getAndSet(now)) / 1e9
            SessionLog.i(TAG, "decoded $n of ${taken.get()} taken / ${offered.get()} offered frames; " +
                "${"%.1f".format(TELEMETRY_EVERY / wall)} decoded/s on ${laneList.size} lanes, " +
                "${"%.1f".format(decodeNs.getAndSet(0) / 1e6 / TELEMETRY_EVERY)} ms each, " +
                "${width}x$height, grid ${c.cols}x${c.rows}" +
                "${if (committed == null) " (searching)" else ""}, phase ${s.phase}, " +
                "read ${"%.2f".format(s.recentReadFraction)}, payloads ${s.payloadFrames}, " +
                "blur ${"%.2f".format(s.motionBlurCells)} cells, " +
                "${"%.1f".format((s.distinctStates - lastDistinct).coerceAtLeast(0) / wall)} " +
                "states/s seen, ${"%.0f".format(s.bytesPerSecond / 1024)} KB/s" +
                if (s.colorSpread > 0) ", colours ${"%.0f".format(s.colorSpread)} apart, " +
                    "noise ${"%.1f".format(s.colorNoise)}" else "")
            lastDistinct = s.distinctStates
        }
        val payloads = s.payloadFrames
        if (payloads > (lastPayloads[c] ?: -1L)) {
            lastPayloads[c] = payloads
            lastPayloadNs[c] = ts
        }
        if (s.recentReadFraction >= 0.3) lastHeaderNs[c] = ts
        if (committed != null) {
            // The sender may have changed its speed setting -- a different grid -- and nothing on
            // this one will ever decode again. After a while with neither payload NOR headers,
            // search all grids again; the committed one is among them, so a person who merely
            // looked away is back on it within a few frames of looking back.
            //
            // Headers count. They used to not: on the Pixel reading the S26 the committed grid's
            // headers read on every frame while its colour payloads failed, the re-search split
            // the frames three ways across grids that could never read, and a transfer at 39%
            // never picked back up (F66). Our grids differ enough that one never reads another's
            // headers, so headers reading means the grid is right and only the data is struggling.
            val lastSign = maxOf(lastPayloadNs[c] ?: ts, lastHeaderNs[c] ?: Long.MIN_VALUE)
            if (committed === c && ts - lastSign > RESEARCH_AFTER_NS) {
                synchronized(nativeLock) { committed = null }
                lastByCandidate.clear()
                SessionLog.i(TAG, "no payload on ${c.cols}x${c.rows} for ${RESEARCH_AFTER_NS / 1_000_000_000} s: " +
                    "searching grids again")
            }
            return s
        }

        // A decoded PAYLOAD proves the grid. Not a decoded header -- a near-miss grid reads those
        // (F58); a payload spans every cell and carries a CRC. Fresh payloads only: a candidate
        // that decoded some before a re-search must decode a NEW one to be chosen again.
        if (payloads > 0 && lastPayloadNs[c] == ts) {
            synchronized(nativeLock) {
                if (committed == null) {
                    committed = c
                    SessionLog.i(TAG, "grid detected: ${c.cols}x${c.rows}")
                }
            }
            return if (committed === c) s.copy(gridKnown = true) else null
        }
        // Still searching: report whichever candidate has got furthest, so the status on screen
        // does not flicker between them.
        lastByCandidate[c] = s
        return lastByCandidate.values.maxByOrNull { it.phase.ordinal } ?: s
    }

    private fun ingestInto(
        lane: Lane, c: Candidate, buf: ByteBuffer, width: Int, height: Int, ts: Long,
    ): LiveStatus? {
        val slots = lane.slots
        val colour = lane.chromaRowStride > 0
        val message = NativeReceiver.ingest(
            c.handle, buf, width, height, width,
            if (colour) lane.u else null, if (colour) lane.v else null,
            lane.chromaRowStride, lane.chromaPixelStride, ts, lane.index, slots,
        )
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
            payloadFrames = slots[Slot.PayloadFrames.ordinal].toLong(),
            bytesPerSecond = slots[Slot.BytesPerSecond.ordinal],
            distinctStates = slots[Slot.DistinctStates.ordinal].toLong(),
            colorSpread = slots[Slot.ColorSpread.ordinal],
            colorNoise = slots[Slot.ColorNoise.ordinal],
            transferSeconds = slots[Slot.TransferSeconds.ordinal],
            bitsPerCell = slots[Slot.BitsPerCell.ordinal].toInt(),
            originalSize = slots[Slot.OriginalSize.ordinal].toLong(),
            codec = slots[Slot.Codec.ordinal].toInt(),
            compressMs = slots[Slot.CompressMs.ordinal].toLong(),
            cols = c.cols,
            rows = c.rows,
            gridKnown = committed === c,
        )
    }

    /**
     * Write the VERIFIED file to Downloads/FileFlow (or, failing that, the app's own "received"
     * folder) and say where, or null if there is no verified file. Call once [finished].
     *
     * Never visible half-written, either way: pending in MediaStore until complete, or a temporary
     * file renamed into place (PROTOCOL-SPEC, file layer). The name is
     * the manifest's — sanitised in native, and sanitised again here, because it came off a screen
     * that anyone could have been showing (THREAT-MODEL T3).
     */
    //
    // The verified bytes are an envelope (`Envelope`): opened here, with the pairing [key] when
    // there is one, and refused -- [Envelope.UnpackException], nothing written -- when it is sealed
    // to another pairing, has been altered, or is plain while [requireEncrypted].
    fun saveVerified(
        context: Context, key: PairingKey? = null, requireEncrypted: Boolean = false,
    ): SavedFile? {
        // Under the same lock as close(): leaving the screen at the moment of completion would
        // otherwise free the native receiver while its bytes were being read out.
        val (received, linkName) = synchronized(nativeLock) {
            if (closed.get()) return null
            val b = NativeReceiver.fileBytes(handle) ?: return null
            b to NativeReceiver.fileName(handle)
        }
        // A sender from before envelopes sent the bare file; accepted only outside encrypted mode.
        val opened = if (Envelope.isEnvelope(received) || requireEncrypted) {
            Envelope.unpack(received, key, requireEncrypted)
        } else {
            Envelope.Unpacked(linkName, received, Envelope.CODEC_NONE, encrypted = false, received.size,
                              compressMs = 0, decompressMs = 0)
        }
        val bytes = opened.data
        val safe = opened.name.replace(Regex("[/\\\\\\u0000-\\u001f]"), "_")
            .trimStart('.').ifBlank { "received.bin" }
        val saved = runCatching { saveToDownloads(context, safe, bytes) }.getOrNull()
            ?: saveToAppFolder(context, safe, bytes)?.let {
                SavedFile(it.name, null, it.absolutePath, bytes.size.toLong())
            }
        return saved?.copy(linkBytes = opened.packedSize.toLong(), codec = opened.codec,
                           encrypted = opened.encrypted, compressMs = opened.compressMs,
                           decompressMs = opened.decompressMs)
    }

    /** Where a received file ended up. [uri] is null when only the app-private fallback worked. */
    data class SavedFile(
        val name: String,
        val uri: android.net.Uri?,
        val location: String,
        val bytes: Long,
        /** Bytes that crossed the link: fewer than [bytes] when the file was compressed. */
        val linkBytes: Long = bytes,
        /** 0 none, 1 deflate, 2 zstd. */
        val codec: Int = 0,
        val encrypted: Boolean = false,
        /** What compressing cost the sender, and opening it cost here, ms. */
        val compressMs: Long = 0,
        val decompressMs: Long = 0,
    ) {
        val compressed: Boolean get() = codec != 0
    }

    /**
     * Into the public Downloads/FileFlow folder, through MediaStore.
     *
     * The app-private folder it used to go to is under `Android/data`, which the Files app hides on
     * current Android -- so a file that had been received and verified could not then be found by
     * the person who received it. MediaStore needs no storage permission for an app's own
     * downloads, de-duplicates names itself, and the file stays hidden (`IS_PENDING`) until every
     * byte is written, so nothing half-written is ever visible (PROTOCOL-SPEC, file layer).
     */
    private fun saveToDownloads(context: Context, name: String, bytes: ByteArray): SavedFile? {
        val resolver = context.contentResolver
        val ext = name.substringAfterLast('.', "").lowercase()
        val mime = android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext)
            ?: "application/octet-stream"
        val values = android.content.ContentValues().apply {
            put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, name)
            put(android.provider.MediaStore.MediaColumns.MIME_TYPE, mime)
            put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH,
                android.os.Environment.DIRECTORY_DOWNLOADS + "/" + DOWNLOADS_SUBDIR)
            put(android.provider.MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = resolver.insert(
            android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, values,
        ) ?: return null
        try {
            resolver.openOutputStream(uri)?.use { it.write(bytes) } ?: error("no output stream")
            values.clear()
            values.put(android.provider.MediaStore.MediaColumns.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
        } catch (e: Exception) {
            runCatching { resolver.delete(uri, null, null) }
            SessionLog.w(TAG, "could not save to Downloads", e)
            return null
        }
        // MediaStore may have renamed it ("name (1).ext"); report the name it actually has.
        val finalName = resolver.query(
            uri, arrayOf(android.provider.MediaStore.MediaColumns.DISPLAY_NAME), null, null, null,
        )?.use { if (it.moveToFirst()) it.getString(0) else null } ?: name
        return SavedFile(finalName, uri, "Downloads/$DOWNLOADS_SUBDIR/$finalName", bytes.size.toLong())
    }

    /** Fallback when MediaStore refuses: the app's own folder, temp file then rename. */
    private fun saveToAppFolder(context: Context, safe: String, bytes: ByteArray): File? {
        val dir = (context.getExternalFilesDir(RECEIVED_DIR) ?: File(context.filesDir, RECEIVED_DIR))
            .apply { mkdirs() }
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
        if (!closed.compareAndSet(false, true)) return
        finished = true
        // Wait for the lanes OUTSIDE the lock: a lane committing to a grid takes it, and holding it
        // here would stall that lane until the timeout.
        laneList.forEach { it.worker.shutdown() }
        val stopped = laneList.all { it.worker.awaitTermination(2, TimeUnit.SECONDS) }
        synchronized(nativeLock) {
            // Freed only if every lane actually stopped: freeing under a decode still in flight
            // would be a use-after-free in native. A leak is the lesser failure.
            if (stopped) candidates.forEach { NativeReceiver.close(it.handle) }
        }
    }

    /** Declaration order IS the wire format. Keep in lockstep with `Slot` in `jni_receiver.cpp`. */
    private enum class Slot {
        Phase, Hint, Progress, SecondsRemaining, MotionBlurCells, RecentReadFraction,
        ScreenInView, Frames, FileSize, PayloadFrames, BytesPerSecond, DistinctStates,
        ColorSpread, ColorNoise, TransferSeconds, BitsPerCell, OriginalSize, Codec, CompressMs,
    }

    companion object {
        const val RECEIVED_DIR = "received"
        const val DOWNLOADS_SUBDIR = "FileFlow"
        private const val TAG = "FileFlow.Live"
        private const val TELEMETRY_EVERY = 60L
        private const val MAX_LANES = 8  // LiveReceiver::kMaxLanes
        /** No payload on the chosen grid for this long: look at the others again. */
        private const val RESEARCH_AFTER_NS = 6_000_000_000L
        /**
         * Lanes by default: as many as there are cores to spare, at most 4. The camera, the
         * preview and the UI need theirs, and past about four the lanes queue on the session lock
         * rather than run (F60).
         */
        val DEFAULT_LANES: Int =
            (Runtime.getRuntime().availableProcessors() - 3).coerceIn(1, 4)
    }
}

internal object NativeReceiver {
    init {
        System.loadLibrary("fileflow")
    }

    @JvmStatic external fun slotCount(): Int
    @JvmStatic external fun open(cols: Int, rows: Int, exposureNs: Long, lanes: Int): Long
    @JvmStatic external fun ingest(
        handle: Long, y: ByteBuffer, width: Int, height: Int, rowStride: Int,
        u: ByteBuffer?, v: ByteBuffer?, chromaRowStride: Int, chromaPixelStride: Int,
        timestampNs: Long, lane: Int, out: DoubleArray,
    ): String
    @JvmStatic external fun fileName(handle: Long): String
    @JvmStatic external fun fileBytes(handle: Long): ByteArray?
    @JvmStatic external fun close(handle: Long)
}
